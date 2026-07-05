# 内核态到用户态 mmap 数据通路

## 1. 这条链路解决的问题

传统监控 Agent 通常周期性读取 `/proc` 文件，例如 `/proc/stat`、`/proc/softirqs`、`/proc/loadavg`。这种方式实现简单，但每次采样都要经过一次文件读取路径：用户态发起系统调用，内核组织文本格式数据，再拷贝到用户态，用户态还要解析文本。

本项目里 CPU 状态和软中断统计没有完全走 `/proc` 文本读取路径，而是单独实现了内核模块。内核模块在内核空间维护一块共享统计区域，并通过字符设备的 `mmap` 回调把这块区域映射到 worker 进程地址空间。用户态采集时只需要读映射后的结构体数组，就像读普通内存变量一样，避免了每次采样都走 `read` 系统调用和内核到用户态的数据拷贝。

当前仓库里明确实现这条链路的指标有两类：

- CPU 状态统计：`/dev/cpu_stat_monitor`
- CPU 软中断统计：`/dev/cpu_softirq_monitor`

## 2. 总体数据流

```text
Linux 内核统计结构
    |
    |  内核模块定时读取
    v
内核模块共享内存区
    |
    |  字符设备 file_operations.mmap
    |  remap_pfn_range 建立页映射
    v
用户态 worker 进程虚拟地址
    |
    |  直接按结构体数组读取
    v
CpuStatMonitor / CpuSoftIrqMonitor
    |
    |  差分计算、填充 protobuf
    v
MetricCollector::CollectAll
    |
    v
MonitorInfo 上报给 Manager
```

这条链路的关键不是“内核直接给出 CPU 使用率”，而是“内核模块把累计统计值放进共享区域，用户态读出前后两次累计值后计算差分”。

## 3. CPU 状态统计链路

### 3.1 内核态数据来源

对应源码：

- [worker/src/kmod/cpu_stat_collector.c](../../worker/src/kmod/cpu_stat_collector.c)

内核模块定义了和用户态共享的结构体 `struct cpu_stat`，字段包括：

- `user`
- `nice`
- `system`
- `idle`
- `iowait`
- `irq`
- `softirq`
- `steal`
- `guest`
- `guest_nice`

共享内存指针是：

```c
static struct cpu_stat *cpu_stat_data;
```

模块通过 `update_cpu_stats()` 读取每个 CPU 的内核累计统计：

```c
kcs = &kcpustat_cpu(cpu);
cpu_stat_data[idx].user = nsec_to_jiffies(kcs->cpustat[CPUTIME_USER]);
cpu_stat_data[idx].system = nsec_to_jiffies(kcs->cpustat[CPUTIME_SYSTEM]);
cpu_stat_data[idx].irq = nsec_to_jiffies(kcs->cpustat[CPUTIME_IRQ]);
cpu_stat_data[idx].softirq = nsec_to_jiffies(kcs->cpustat[CPUTIME_SOFTIRQ]);
```

`idle` 和 `iowait` 额外走了 `get_cpu_idle_time_us()`、`get_cpu_iowait_time_us()`，用于兼容 tickless 场景下更精确的空闲时间和 I/O 等待时间统计。

### 3.2 内核态共享内存分配

模块初始化时按页对齐分配共享区：

```c
data_size = PAGE_ALIGN(sizeof(struct cpu_stat) * MAX_CPUS);
cpu_stat_data = (struct cpu_stat *)__get_free_pages(
    GFP_KERNEL | __GFP_ZERO,
    get_order(data_size)
);
```

这里使用页级分配，而不是普通小对象分配，原因是后续要把这块内存映射给用户态。`mmap` 映射以页为基本单位，所以数据区需要页对齐。

### 3.3 定时刷新

CPU 统计模块使用高精度定时器周期性刷新共享区：

```c
timer_interval = ktime_set(1, 0);
hrtimer_init(&update_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
update_timer.function = timer_callback;
hrtimer_start(&update_timer, timer_interval, HRTIMER_MODE_REL);
```

定时器回调里调用：

```c
update_cpu_stats();
```

因此，用户态看到的是内核模块按固定周期刷新后的最新累计统计快照。

### 3.4 字符设备和 mmap 暴露

CPU 模块注册字符设备，并创建设备节点，对应代码见 [cpu_stat_collector.c:268](../../worker/src/kmod/cpu_stat_collector.c#L268) 和 [cpu_stat_collector.c:291](../../worker/src/kmod/cpu_stat_collector.c#L291)：

```c
cdev_init(&cpu_stat_cdev, &cpu_stat_fops);
cdev_add(&cpu_stat_cdev, dev_num, 1);
device_create(cpu_stat_class, NULL, dev_num, NULL, DEVICE_NAME);
```

`DEVICE_NAME` 是：

```c
#define DEVICE_NAME "cpu_stat_monitor"
```

所以用户态设备路径是：

```text
/dev/cpu_stat_monitor
```

真正把内核内存映射到用户态的是 [`cpu_stat_mmap()`](../../worker/src/kmod/cpu_stat_collector.c#L195)：

```c
pfn = page_to_pfn(virt_to_page(cpu_stat_data));
ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
```

并且这个回调挂到了字符设备的文件操作表：

```c
static const struct file_operations cpu_stat_fops = {
    .owner   = THIS_MODULE,
    .open    = cpu_stat_open,
    .release = cpu_stat_release,
    .mmap    = cpu_stat_mmap,
};
```

这就是“内核模块直接把统计数据映射到用户进程地址空间”的核心代码。

### 3.5 用户态读取

对应源码：

- [worker/src/monitor/cpu_stat_monitor.cpp](../../worker/src/monitor/cpu_stat_monitor.cpp)

用户态采集类是 [`CpuStatMonitor::UpdateOnce()`](../../worker/src/monitor/cpu_stat_monitor.cpp#L11)。

它先打开字符设备：

```cpp
int fd = open("/dev/cpu_stat_monitor", O_RDONLY);
```

然后把设备映射到当前进程地址空间：

```cpp
void* addr = mmap(nullptr, stat_size, PROT_READ, MAP_SHARED, fd, 0);
```

映射成功后，用户态直接把返回地址转换成结构体数组：

```cpp
struct cpu_stat* stats = static_cast<struct cpu_stat*>(addr);
```

后续读取 `stats[i].user`、`stats[i].system`、`stats[i].idle` 等字段时，代码层面就是普通内存读取，不再每个字段发起系统调用，也不需要从内核复制一份缓冲区。

### 3.6 用户态差分计算 CPU 使用率

内核模块提供的是累计时间，不是百分比。`CpuStatMonitor` 在用户态缓存上一次采样：

```cpp
CpuStat& cached = cpu_stat_map_[stats[i].cpu_name];
```

下一次采样时，使用新旧累计值计算差分：

```cpp
float cpu_percent = (new_cpu_busy_time - old_cpu_busy_time) /
                    (new_cpu_total_time - old_cpu_total_time) * 100.00;
```

因此 CPU 使用率计算链路是：

```text
内核累计时间 -> mmap 暴露 -> 用户态读取两次快照 -> 差分计算百分比
```

## 4. 软中断统计链路

### 4.1 内核态数据来源

对应源码：

- [worker/src/kmod/softirq_collector.c](../../worker/src/kmod/softirq_collector.c)

软中断模块定义共享结构体 `struct softirq_stat`，字段包括：

- `hi`
- `timer`
- `net_tx`
- `net_rx`
- `block`
- `irq_poll`
- `tasklet`
- `sched`
- `hrtimer`
- `rcu`

共享内存指针是：

```c
static struct softirq_stat *softirq_data;
```

模块通过 [`update_softirq_stats()`](../../worker/src/kmod/softirq_collector.c#L67) 读取每个 CPU 的软中断累计计数：

```c
softirq_data[idx].net_tx = kstat_softirqs_cpu(NET_TX_SOFTIRQ, cpu);
softirq_data[idx].net_rx = kstat_softirqs_cpu(NET_RX_SOFTIRQ, cpu);
softirq_data[idx].timer  = kstat_softirqs_cpu(TIMER_SOFTIRQ, cpu);
softirq_data[idx].rcu    = kstat_softirqs_cpu(RCU_SOFTIRQ, cpu);
```

这里的数据来源是内核软中断统计，而不是用户态解析 `/proc/softirqs` 文本。

### 4.2 mmap 暴露方式

软中断模块同样按页对齐分配共享区：

```c
data_size = PAGE_ALIGN(sizeof(struct softirq_stat) * MAX_CPUS);
softirq_data = (struct softirq_stat *)__get_free_pages(
    GFP_KERNEL | __GFP_ZERO,
    get_order(data_size)
);
```

字符设备名是：

```c
#define DEVICE_NAME "cpu_softirq_monitor"
```

所以用户态设备路径是：

```text
/dev/cpu_softirq_monitor
```

映射核心代码在 [`softirq_mmap()`](../../worker/src/kmod/softirq_collector.c#L135)：

```c
pfn = page_to_pfn(virt_to_page(softirq_data));
ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
```

并挂到：

```c
static const struct file_operations softirq_fops = {
    .owner   = THIS_MODULE,
    .open    = softirq_open,
    .release = softirq_release,
    .mmap    = softirq_mmap,
};
```

### 4.3 用户态读取和速率计算

对应源码：

- [worker/src/monitor/cpu_softirq_monitor.cpp](../../worker/src/monitor/cpu_softirq_monitor.cpp)

用户态先打开设备：

```cpp
int fd = open("/dev/cpu_softirq_monitor", O_RDONLY);
```

然后执行 `mmap`：

```cpp
void* addr = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
```

映射成功后直接按 `struct softirq_stat` 数组读取：

```cpp
struct softirq_stat* stats = static_cast<struct softirq_stat*>(addr);
```

软中断计数也是累计值。用户态保存上一轮采样，并根据时间差计算每秒软中断次数：

```cpp
softirq_msg->set_net_rx(
    static_cast<int64_t>((stats[i].net_rx - it->second.net_rx) / seconds));
```

所以软中断链路是：

```text
内核软中断累计计数 -> mmap 暴露 -> 用户态读取两次快照 -> 按秒计算速率
```

## 5. 聚合到 worker 采集流程

对应源码：

- [worker/src/monitor/metric_collector.cpp](../../worker/src/monitor/metric_collector.cpp)

[`MetricCollector`](../../worker/src/monitor/metric_collector.cpp#L22) 初始化时会把 CPU 状态和软中断监控器加入采集列表：

```cpp
monitors_.push_back(std::make_unique<CpuStatMonitor>());
monitors_.push_back(std::make_unique<CpuSoftIrqMonitor>());
```

每次采集时，`CollectAll()` 统一调用各个 monitor 的 `UpdateOnce()`：

```cpp
for (auto& monitor : monitors_) {
  monitor->UpdateOnce(monitor_info);
}
```

因此完整运行链路是：

```text
worker 定时 Push
    -> MetricCollector::CollectAll
    -> CpuStatMonitor::UpdateOnce
       -> open /dev/cpu_stat_monitor
       -> mmap
       -> 直接读取 cpu_stat 数组
       -> 差分计算 CPU 使用率
       -> 填充 MonitorInfo.cpu_stat
    -> CpuSoftIrqMonitor::UpdateOnce
       -> open /dev/cpu_softirq_monitor
       -> mmap
       -> 直接读取 softirq_stat 数组
       -> 差分计算软中断速率
       -> 填充 MonitorInfo.soft_irq
    -> gRPC 上报给 Manager
```

## 6. 和读取 `/proc` 的区别

| 方案 | 数据通路 | 特点 |
| --- | --- | --- |
| 读取 `/proc` | 用户态 `read` -> 内核生成文本 -> 拷贝到用户态 -> 用户态解析 | 简单、通用，但每次采样都有系统调用、拷贝和文本解析成本 |
| 字符设备 + `mmap` | 内核模块维护共享区 -> 用户态映射同一块内存 -> 用户态直接读结构体 | 更适合高频只读统计，减少重复系统调用和数据拷贝 |

需要注意的是，`mmap` 并不是完全没有系统调用。建立映射时仍然需要一次 `open` 和一次 `mmap`。这里强调的“无需系统调用、无需数据拷贝”，指的是映射建立后读取统计数据的热路径：用户态读字段时不再为每次采样数据走 `read` 调用，也不再需要内核把数据复制到用户态缓冲区。

## 7. 面试表达版本

可以这样讲：

> 传统监控 Agent 经常周期性读 `/proc`，每次采样都要经过系统调用、内核组织文本数据、拷贝到用户态，再由用户态解析。我们在 CPU stat 和 softirq 两条链路上做了内核模块，模块定时从 `kcpustat_cpu` 和 `kstat_softirqs_cpu` 读取内核累计统计，写入页对齐的共享内存区。然后通过字符设备实现 `mmap`，用 `remap_pfn_range` 把这块内核内存映射到 worker 进程。用户态的 `CpuStatMonitor` 和 `CpuSoftIrqMonitor` 打开 `/dev/cpu_stat_monitor`、`/dev/cpu_softirq_monitor` 后执行 `mmap`，后续直接按结构体数组读取数据，再做两次采样差分，计算 CPU 使用率和软中断速率。这样高频采样时避免了反复 `read /proc` 带来的系统调用、数据拷贝和文本解析开销。

## 8. 代码索引

| 作用 | 代码位置 |
| --- | --- |
| CPU 共享结构体和共享内存指针 | [cpu_stat_collector.c:46](../../worker/src/kmod/cpu_stat_collector.c#L46) |
| CPU 内核统计读取，采集 `kcpustat_cpu` | [cpu_stat_collector.c:124](../../worker/src/kmod/cpu_stat_collector.c#L124) |
| CPU 字符设备 `mmap` 实现，调用 `remap_pfn_range` | [cpu_stat_collector.c:195](../../worker/src/kmod/cpu_stat_collector.c#L195) |
| CPU 用户态 monitor，打开 `/dev/cpu_stat_monitor` 并读取映射区 | [cpu_stat_monitor.cpp:11](../../worker/src/monitor/cpu_stat_monitor.cpp#L11) |
| 软中断共享结构体和共享内存指针 | [softirq_collector.c:35](../../worker/src/kmod/softirq_collector.c#L35) |
| 软中断内核统计读取，采集 `kstat_softirqs_cpu` | [softirq_collector.c:67](../../worker/src/kmod/softirq_collector.c#L67) |
| 软中断字符设备 `mmap` 实现，调用 `remap_pfn_range` | [softirq_collector.c:135](../../worker/src/kmod/softirq_collector.c#L135) |
| 软中断用户态 monitor，打开 `/dev/cpu_softirq_monitor` 并读取映射区 | [cpu_softirq_monitor.cpp:21](../../worker/src/monitor/cpu_softirq_monitor.cpp#L21) |
| worker 采集聚合入口 | [metric_collector.cpp:22](../../worker/src/monitor/metric_collector.cpp#L22) |
| 内核模块加载脚本 | [load_modules.sh](../../worker/scripts/load_modules.sh) |
