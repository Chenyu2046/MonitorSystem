# eBPF 网络流量统计链路

## 1. 结论

“eBPF 层网络流量不走 `/proc`，而是挂一个 eBPF 程序在协议栈收发路径上，包经过就统计，用哈希表存结果，原子操作保证并发安全”这段表述和当前代码基本一致。

更精确地说：

- eBPF 程序挂在 TC ingress 和 TC egress，而不是 XDP，也不是读 `/proc/net/dev`。
- 每个包经过 hook 时，程序读取 `skb->ifindex` 和 `skb->len`。
- BPF map 使用 `BPF_MAP_TYPE_HASH`，key 是网卡 `ifindex`，value 是收发字节数和包数。
- 已有网卡统计项更新时使用 `__sync_fetch_and_add` 做原子累加。
- 用户态 `NetEbpfMonitor` 负责加载 eBPF skeleton、创建 TC hook、attach ingress/egress 程序、遍历 BPF map，并把累计值差分成速率。

需要补一句边界：

- 当前项目有两套路由。启用并成功构建 eBPF 依赖时走 `NetEbpfMonitor`；否则编译普通 `NetMonitor`，读取 `/proc/net/dev`。

## 2. 总体链路

```text
网络包进入或离开协议栈
    -> TC ingress / TC egress hook
    -> eBPF 程序读取 skb->ifindex 和 skb->len
    -> 以 ifindex 为 key 查找 net_stats_map
    -> 首次出现则初始化 map value
    -> 已存在则原子累加 bytes / packets
    -> 用户态 NetEbpfMonitor 遍历 BPF map
    -> 和上一轮采样做差分
    -> 填充 MonitorInfo.net_info
    -> worker 后续 Push 给 Manager
```

这条链路和 `/proc/net/dev` 的差别是：`/proc/net/dev` 是用户态按周期读取内核导出的累计文本；eBPF TC hook 是包经过内核网络路径时直接执行统计逻辑。

## 3. eBPF 内核侧：TC Hook 统计包

eBPF 程序入口在 [worker/src/ebpf/net_stats.bpf.c](../../worker/src/ebpf/net_stats.bpf.c)。

文件开头注释已经说明它是基于 TC Hook 的网络流量统计程序，挂载到 ingress / egress 两个方向。

### 3.1 统计结构

统计结构定义在 [net_stats.bpf.c:29](../../worker/src/ebpf/net_stats.bpf.c#L29)：

```c
struct net_stats {
    __u64 rcv_bytes;
    __u64 rcv_packets;
    __u64 snd_bytes;
    __u64 snd_packets;
};
```

字段含义：

- `rcv_bytes`：接收字节数
- `rcv_packets`：接收包数
- `snd_bytes`：发送字节数
- `snd_packets`：发送包数

### 3.2 BPF Hash Map

BPF map 定义在 [net_stats.bpf.c:41](../../worker/src/ebpf/net_stats.bpf.c#L41)：

```c
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, struct net_stats);
} net_stats_map SEC(".maps");
```

这里的设计是：

- map 类型：`BPF_MAP_TYPE_HASH`
- key：`__u32 ifindex`
- value：`struct net_stats`
- 最大记录数：64 个网卡

所以这句话“用哈希表存结果”对应的就是 `net_stats_map`。

### 3.3 包经过时更新统计

更新函数在 [net_stats.bpf.c:52](../../worker/src/ebpf/net_stats.bpf.c#L52)：

```c
static __always_inline void update_stats(__u32 ifindex, __u32 len, bool is_rx)
```

它先按 `ifindex` 查 BPF map：

```c
stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
```

如果是第一次看到这个网卡，就初始化一个 `net_stats` 并写入 map：

```c
bpf_map_update_elem(&net_stats_map, &ifindex, &new_stats, BPF_ANY);
```

如果 map 中已经有该网卡统计项，就对累计值做原子累加。

### 3.4 原子操作保证并发安全

原子累加代码在 [net_stats.bpf.c:69](../../worker/src/ebpf/net_stats.bpf.c#L69)：

```c
if (is_rx) {
    __sync_fetch_and_add(&stats->rcv_bytes, len);
    __sync_fetch_and_add(&stats->rcv_packets, 1);
} else {
    __sync_fetch_and_add(&stats->snd_bytes, len);
    __sync_fetch_and_add(&stats->snd_packets, 1);
}
```

网络包可能在多个 CPU 上并发经过 TC hook。多个 CPU 可能同时更新同一个网卡的统计值，因此这里没有直接 `stats->rcv_bytes += len`，而是使用 `__sync_fetch_and_add`。

这就是“原子操作保证并发安全”的代码证据。

### 3.5 Ingress 收包路径

Ingress hook 在 [net_stats.bpf.c:86](../../worker/src/ebpf/net_stats.bpf.c#L86)：

```c
SEC("tc/ingress")
int tc_ingress(struct __sk_buff *skb)
```

它读取包所在网卡和包长：

```c
__u32 ifindex = skb->ifindex;
__u32 len = skb->len;
```

然后按接收方向更新统计：

```c
update_stats(ifindex, len, true);
```

`TC_ACT_OK` 表示统计完以后继续正常放行，不改变包处理结果。

### 3.6 Egress 发包路径

Egress hook 在 [net_stats.bpf.c:107](../../worker/src/ebpf/net_stats.bpf.c#L107)：

```c
SEC("tc/egress")
int tc_egress(struct __sk_buff *skb)
```

它同样读取：

```c
__u32 ifindex = skb->ifindex;
__u32 len = skb->len;
```

然后按发送方向更新统计：

```c
update_stats(ifindex, len, false);
```

因此当前 eBPF 层同时覆盖收包和发包两个方向。

## 4. 用户态：加载、挂载和读取 BPF Map

用户态实现入口在 [worker/src/monitor/net_ebpf_monitor.cpp](../../worker/src/monitor/net_ebpf_monitor.cpp)。

### 4.1 打开并加载 eBPF skeleton

加载逻辑在 [net_ebpf_monitor.cpp:98](../../worker/src/monitor/net_ebpf_monitor.cpp#L98)：

```cpp
bool NetEbpfMonitor::InitEbpf()
```

它先打开 skeleton：

```cpp
skel = net_stats_bpf__open();
```

再加载 eBPF 程序：

```cpp
err = net_stats_bpf__load(skel);
```

这些 `net_stats_bpf__open/load/destroy` 函数来自 bpftool 生成的 skeleton 头文件。

### 4.2 获取 BPF Map fd

加载成功后，用户态取出 `net_stats_map` 的 fd，位置在 [net_ebpf_monitor.cpp:116](../../worker/src/monitor/net_ebpf_monitor.cpp#L116)：

```cpp
map_fd_ = bpf_map__fd(skel->maps.net_stats_map);
```

后续 `UpdateOnce()` 就通过这个 fd 遍历 BPF map。

### 4.3 获取 TC ingress / egress 程序 fd

程序 fd 获取位置在 [net_ebpf_monitor.cpp:126](../../worker/src/monitor/net_ebpf_monitor.cpp#L126)：

```cpp
int ingress_fd = bpf_program__fd(skel->progs.tc_ingress);
int egress_fd = bpf_program__fd(skel->progs.tc_egress);
```

这两个程序正好对应 eBPF 文件里的：

- `SEC("tc/ingress") int tc_ingress(...)`
- `SEC("tc/egress") int tc_egress(...)`

### 4.4 为网卡创建 clsact qdisc

代码会枚举 `/sys/class/net` 下的网卡并跳过 `lo`，然后创建 `clsact` qdisc。辅助函数在 [net_ebpf_monitor.cpp:54](../../worker/src/monitor/net_ebpf_monitor.cpp#L54)：

```cpp
static int tc_qdisc_create_clsact(int ifindex)
```

核心命令是：

```cpp
tc qdisc add dev <ifname> clsact
```

TC ingress / egress 程序需要挂在这个 qdisc 上。

### 4.5 Attach ingress 程序

Ingress attach 代码在 [net_ebpf_monitor.cpp:139](../../worker/src/monitor/net_ebpf_monitor.cpp#L139)：

```cpp
LIBBPF_OPTS(bpf_tc_hook, hook,
    .ifindex = static_cast<int>(ifindex),
    .attach_point = BPF_TC_INGRESS,
);
```

随后创建 hook 并 attach：

```cpp
bpf_tc_hook_create(&hook);
bpf_tc_attach(&hook, &opts_in);
```

这里把 `tc_ingress` 程序挂到对应网卡的 ingress 方向。

### 4.6 Attach egress 程序

Egress attach 代码在 [net_ebpf_monitor.cpp:166](../../worker/src/monitor/net_ebpf_monitor.cpp#L166)：

```cpp
hook.attach_point = BPF_TC_EGRESS;
```

然后 attach egress 程序：

```cpp
bpf_tc_attach(&hook, &opts_eg);
```

这里把 `tc_egress` 程序挂到对应网卡的 egress 方向。

## 5. 用户态读取 BPF Map 并计算速率

运行时采集函数是 [net_ebpf_monitor.cpp:239](../../worker/src/monitor/net_ebpf_monitor.cpp#L239)：

```cpp
void NetEbpfMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info)
```

### 5.1 遍历 BPF Map

遍历 map 的代码在 [net_ebpf_monitor.cpp:253](../../worker/src/monitor/net_ebpf_monitor.cpp#L253)：

```cpp
while (bpf_map_get_next_key(map_fd_, &key, &next_key) == 0) {
    if (bpf_map_lookup_elem(map_fd_, &next_key, &stats) == 0) {
        ...
    }
}
```

这里用户态不是读取 `/proc/net/dev`，而是直接通过 BPF map fd 读取 eBPF 程序维护的统计值。

### 5.2 累计值转速率

BPF map 里存的是累计 bytes / packets。用户态用当前值减去上一轮缓存值，再除以时间间隔，得到每秒速率。对应代码在 [net_ebpf_monitor.cpp:269](../../worker/src/monitor/net_ebpf_monitor.cpp#L269)：

```cpp
auto cache_it = cache_.find(next_key);
```

差分计算在 [net_ebpf_monitor.cpp:277](../../worker/src/monitor/net_ebpf_monitor.cpp#L277)：

```cpp
int64_t rcv_diff = stats.rcv_bytes - old.rcv_bytes;
int64_t snd_diff = stats.snd_bytes - old.snd_bytes;
int64_t rcv_pkt_diff = stats.rcv_packets - old.rcv_packets;
int64_t snd_pkt_diff = stats.snd_packets - old.snd_packets;
```

写入 protobuf 的位置在 [net_ebpf_monitor.cpp:289](../../worker/src/monitor/net_ebpf_monitor.cpp#L289)：

```cpp
net_info->set_rcv_rate(rcv_diff * 1000 / old_duration);
net_info->set_send_rate(snd_diff * 1000 / old_duration);
net_info->set_rcv_packets_rate(rcv_pkt_diff * 1000 / old_duration);
net_info->set_send_packets_rate(snd_pkt_diff * 1000 / old_duration);
```

最后更新本地缓存，供下一轮差分使用：

```cpp
cache_[next_key] = {
    stats.rcv_bytes,
    stats.rcv_packets,
    stats.snd_bytes,
    stats.snd_packets,
    now
};
```

## 6. 和 `/proc/net/dev` 路径的关系

普通 procfs 网络采集在 [worker/src/monitor/net_monitor.cpp:25](../../worker/src/monitor/net_monitor.cpp#L25)：

```cpp
static std::vector<NetStat> get_net_stats_from_proc() {
    std::ifstream file("/proc/net/dev");
```

但是 eBPF 路径由编译开关选择。`MetricCollector` 的选择逻辑在 [worker/src/monitor/metric_collector.cpp:14](../../worker/src/monitor/metric_collector.cpp#L14)：

```cpp
#ifdef ENABLE_EBPF
#include "monitor/net_ebpf_monitor.h"
#else
#include "monitor/net_monitor.h"
#endif
```

实例化逻辑在 [worker/src/monitor/metric_collector.cpp:36](../../worker/src/monitor/metric_collector.cpp#L36)：

```cpp
#ifdef ENABLE_EBPF
  monitors_.push_back(std::make_unique<NetEbpfMonitor>());
#else
  monitors_.push_back(std::make_unique<NetMonitor>());
#endif
```

构建开关在 [worker/CMakeLists.txt:16](../../worker/CMakeLists.txt#L16)：

```cmake
option(ENABLE_EBPF "Enable eBPF-based network monitoring" ON)
```

如果 libbpf、libelf、zlib 等依赖存在，CMake 会启用 eBPF 支持，并编译 `NetEbpfMonitor`。如果依赖缺失，会编译普通 `NetMonitor`。

因此严谨表达是：

> 当前项目网络采集支持 eBPF 和 `/proc/net/dev` 两套路径。启用 eBPF 且依赖满足时，网络统计不走 `/proc/net/dev`，而是通过 TC ingress / egress hook 在包经过协议栈时更新 BPF Hash Map；用户态再读 BPF map 做差分速率计算。如果 eBPF 依赖不满足，构建会回退到 `/proc/net/dev` 的普通采集路径。

## 7. 面试表达版本

可以这样讲：

> 网络流量这块我没有只依赖 `/proc/net/dev`。项目里有一条 eBPF 增强路径：在 `net_stats.bpf.c` 里定义了 TC ingress 和 TC egress 两个 eBPF 程序，分别挂在收包和发包路径上。每个包经过时读取 `skb->ifindex` 和 `skb->len`，用 ifindex 作为 key，把每张网卡的收发字节数和包数累加到 `BPF_MAP_TYPE_HASH` 类型的 `net_stats_map` 里。因为多个 CPU 可能同时处理同一张网卡的包，所以更新已有统计项时用 `__sync_fetch_and_add` 做原子累加。用户态的 `NetEbpfMonitor` 负责加载 skeleton、通过 libbpf attach TC hook，然后周期性遍历 BPF map，和上一轮采样做差分，算出 bytes/sec 和 packets/sec，再填到 `MonitorInfo.net_info` 里。需要说明的是，代码里也保留了 `/proc/net/dev` 路径，eBPF 依赖不满足时会回退到普通 NetMonitor。

## 8. 代码索引

| 作用 | 代码位置 |
| --- | --- |
| eBPF 网络统计程序入口 | [net_stats.bpf.c](../../worker/src/ebpf/net_stats.bpf.c) |
| `net_stats` 统计结构 | [net_stats.bpf.c:29](../../worker/src/ebpf/net_stats.bpf.c#L29) |
| BPF Hash Map `net_stats_map` | [net_stats.bpf.c:41](../../worker/src/ebpf/net_stats.bpf.c#L41) |
| 更新统计函数 `update_stats` | [net_stats.bpf.c:52](../../worker/src/ebpf/net_stats.bpf.c#L52) |
| 原子累加 `__sync_fetch_and_add` | [net_stats.bpf.c:69](../../worker/src/ebpf/net_stats.bpf.c#L69) |
| TC ingress hook | [net_stats.bpf.c:86](../../worker/src/ebpf/net_stats.bpf.c#L86) |
| TC egress hook | [net_stats.bpf.c:107](../../worker/src/ebpf/net_stats.bpf.c#L107) |
| 用户态 eBPF monitor 入口 | [net_ebpf_monitor.cpp](../../worker/src/monitor/net_ebpf_monitor.cpp) |
| 打开和加载 eBPF skeleton | [net_ebpf_monitor.cpp:98](../../worker/src/monitor/net_ebpf_monitor.cpp#L98) |
| 获取 BPF map fd | [net_ebpf_monitor.cpp:116](../../worker/src/monitor/net_ebpf_monitor.cpp#L116) |
| 获取 ingress / egress 程序 fd | [net_ebpf_monitor.cpp:126](../../worker/src/monitor/net_ebpf_monitor.cpp#L126) |
| 创建 TC hook 并 attach ingress | [net_ebpf_monitor.cpp:139](../../worker/src/monitor/net_ebpf_monitor.cpp#L139) |
| attach egress | [net_ebpf_monitor.cpp:166](../../worker/src/monitor/net_ebpf_monitor.cpp#L166) |
| 遍历 BPF map | [net_ebpf_monitor.cpp:253](../../worker/src/monitor/net_ebpf_monitor.cpp#L253) |
| 差分计算速率并填充 `NetInfo` | [net_ebpf_monitor.cpp:277](../../worker/src/monitor/net_ebpf_monitor.cpp#L277) |
| eBPF / procfs 网络 monitor 编译选择 | [metric_collector.cpp:36](../../worker/src/monitor/metric_collector.cpp#L36) |
| eBPF 构建开关 | [worker/CMakeLists.txt:16](../../worker/CMakeLists.txt#L16) |
| 普通 `/proc/net/dev` 路径 | [net_monitor.cpp:25](../../worker/src/monitor/net_monitor.cpp#L25) |

