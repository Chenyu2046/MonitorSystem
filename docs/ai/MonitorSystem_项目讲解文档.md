# MonitorSystem 项目讲解文档

> 适用场景：C++ 后端 / Linux 系统编程 / 基础架构实习面试。  
> 项目仓库：`https://github.com/yu20120707/MonitorSystem`  
> 文档定位：这不是 README 摘要，而是基于主要源码链路梳理后的项目讲解稿，重点讲清楚 **数据流、模块职责、底层采集机制、工程风险和面试表达方式**。

---

## 0. 一句话定位

**MonitorSystem 是一个基于 Worker-Agent + Manager 架构的 Linux 分布式服务器性能监控系统。**

Worker 部署在被监控服务器上，通过 `/proc`、字符设备 `mmap`、内核模块和 eBPF TC Hook 采集 CPU、内存、磁盘、网络和软中断等指标；随后将指标封装为 Protobuf 的 `MonitorInfo`，周期性通过 gRPC 主动 Push 到 Manager。Manager 接收后维护主机状态，计算健康评分和变化率，将原始指标与明细数据写入 MySQL，并对外提供多类 gRPC 查询接口。

更适合面试表达的版本：

> 这个项目不是简单的业务 CRUD，而是一个偏 Linux 系统方向的监控 Agent 项目。它的核心价值在于实现了从 **内核态指标采集 → 用户态聚合 → Protobuf 序列化 → gRPC 分布式上报 → Manager 评分与持久化 → 查询服务** 的完整链路。

---

## 1. 项目真实定位与边界

### 1.1 应该怎么定位

| 维度 | 准确定位 |
|---|---|
| 项目类型 | Linux 分布式性能监控系统 / 监控 Agent 原型 |
| 技术方向 | C++ 后端、Linux 系统编程、gRPC、Protobuf、MySQL、eBPF、内核模块 |
| 核心链路 | Worker 采集指标，Manager 接收、评分、写库、查询 |
| 面试亮点 | 指标采集机制、内核态/用户态数据传递、gRPC Push、健康评分、查询服务 |
| 项目成熟度 | 系统方向简历项目骨架，中等工程成熟度，不是生产级 APM/Prometheus 替代品 |

### 1.2 不应该怎么吹

不要说：

- “实现了企业级 APM 平台”
- “完全基于 eBPF 实现全链路监控”
- “具备生产级高可用告警体系”
- “可以替代 Prometheus”
- “内置智能异常检测/机器学习诊断”

更准确的说法是：

> 项目重点在 Linux 底层指标采集和分布式上报链路，已经实现了 CPU、内存、磁盘、网络和软中断等核心指标的采集、上报、存储和查询；但在生产级安全、可靠性、时序存储、告警收敛、服务发现等方面仍有明显可优化空间。

---

## 2. 总体架构

### 2.1 架构图

```text
+-----------------------------+             gRPC Push              +-----------------------------+
|            Worker           |  --------------------------------> |            Manager          |
|       被监控服务器 Agent      |           MonitorInfo              |          管理中心节点          |
|                             |                                     |                             |
|  MetricCollector            |                                     |  GrpcServerImpl             |
|    |-- HostMonitor           |                                     |    |-- SetMonitorInfo         |
|    |-- CpuLoadMonitor        |                                     |                             |
|    |-- CpuStatMonitor        |                                     |  HostManager                |
|    |-- CpuSoftIrqMonitor     |                                     |    |-- OnDataReceived         |
|    |-- MemMonitor            |                                     |    |-- CalcScore              |
|    |-- DiskMonitor           |                                     |    |-- WriteToMysql           |
|    |-- NetMonitor            |                                     |                             |
|    |-- NetEbpfMonitor        |                                     |  QueryManager               |
|                             |                                     |  QueryServiceImpl           |
|  MonitorPusher              |                                     |    |-- QueryPerformance       |
|    |-- PushLoop              |                                     |    |-- QueryTrend             |
|    |-- PushOnce              |                                     |    |-- QueryAnomaly           |
|                             |                                     |    |-- QueryScoreRank         |
+-----------------------------+                                     +-----------------------------+
        |                                                                      |
        |                                                                      |
        v                                                                      v
/proc, /dev + mmap, eBPF map                                             MySQL tables
```

### 2.2 分层说明

```text
MonitorSystem
├── proto/
│   ├── monitor_info.proto        # Worker -> Manager 的总监控数据结构与 MonitorInfoManager 服务
│   ├── cpu_load.proto            # CPU load 指标结构
│   ├── cpu_stat.proto            # CPU 使用率 / user/system/idle/iowait 等结构
│   ├── cpu_softirq.proto         # 软中断指标结构
│   ├── mem_info.proto            # 内存指标结构
│   ├── disk_info.proto           # 磁盘指标结构
│   ├── net_info.proto            # 网络指标结构
│   └── query_api.proto           # Manager 对外查询服务定义
│
├── worker/
│   ├── src/main.cpp              # Worker 启动入口
│   ├── src/rpc/                  # MonitorPusher / gRPC 上报逻辑
│   ├── src/monitor/              # 各类指标采集模块
│   ├── src/kmod/                 # CPU stat / softirq 内核模块
│   └── src/ebpf/                 # eBPF 网络统计程序
│
└── manager/
    ├── src/main.cpp              # Manager 启动入口
    ├── src/rpc/                  # gRPC 接收服务 + 查询服务
    ├── src/host_manager.cpp      # 主机状态管理、健康评分、MySQL 写入
    ├── src/query_manager.cpp     # MySQL 查询封装
    └── sql/                      # 数据库初始化脚本
```

---

## 3. 核心数据流

### 3.1 主链路

```text
Worker 启动
  -> 创建 MonitorPusher
  -> MonitorPusher 创建 gRPC stub
  -> 创建 MetricCollector
  -> 周期性执行 PushLoop
  -> 每轮调用 MetricCollector::CollectAll
  -> 各 Monitor 采集本机指标
  -> 填充 Protobuf MonitorInfo
  -> 调用 gRPC SetMonitorInfo
  -> Manager 接收 MonitorInfo
  -> GrpcServerImpl 校验 hostname 并缓存最新数据
  -> callback 触发 HostManager::OnDataReceived
  -> HostManager 计算健康分、变化率
  -> 写入 MySQL 主表和明细表
  -> QueryService 对外提供查询接口
```

### 3.2 为什么是 Push 模型

项目采用的是 **Worker 主动 Push**，而不是 Manager 主动 Pull。

| 模型 | 特点 | 优点 | 缺点 |
|---|---|---|---|
| Push | Worker 主动上报 | Manager 不需要主动探测每台机器；穿透某些网络边界更简单；Worker 可控制采样周期 | Manager 需要处理上报风暴；离线判断更复杂；需要处理失败重试和背压 |
| Pull | Manager 主动拉取 | 中心端节奏可控；天然适合 Prometheus 模型 | Manager 需要知道所有 Worker 地址；跨网络访问复杂；被监控节点需要暴露服务 |

本项目的 Worker 默认按固定间隔上报，Manager 只负责接收和处理。因此面试中应该强调：

> 这个项目是 Push-based monitor agent，不是 Prometheus 那种典型 Pull-based scraper。

---

## 4. Worker 端详细讲解

## 4.1 Worker 启动逻辑

Worker 的入口逻辑可以概括为：

```text
main(argc, argv)
  -> 解析 manager_address 和 interval_seconds
  -> 创建 MonitorPusher(manager_address, interval_seconds)
  -> 调用 pusher.Start()
  -> 后台线程周期性 Push
```

默认 Manager 地址通常是：

```text
localhost:50051
```

默认上报间隔通常是：

```text
10 seconds
```

### 4.1.1 面试讲法

> Worker 入口比较轻，核心不是 main 函数，而是 `MonitorPusher`。Worker 启动后会建立到 Manager 的 gRPC channel，并在后台循环中周期性调用 `MetricCollector` 汇总所有指标，然后通过 `SetMonitorInfo` 接口推送到 Manager。

---

## 4.2 MonitorPusher：Worker 上报器

`MonitorPusher` 的职责是把本机指标从采集端推送到 Manager。

核心职责：

1. 创建 gRPC Channel；
2. 创建 `MonitorInfoManager::Stub`；
3. 创建 `MetricCollector`；
4. 周期性采集指标；
5. 调用 `SetMonitorInfo` 上报；
6. 处理上报失败日志。

### 4.2.1 数据流

```text
MonitorPusher::Start
  -> 启动后台线程
  -> PushLoop
  -> while running:
       MonitorInfo info
       collector.CollectAll(&info)
       stub.SetMonitorInfo(info)
       sleep(interval)
```

### 4.2.2 工程风险

当前上报逻辑偏基础：

- 没有本地落盘缓冲；
- 没有指数退避重试；
- 没有 gRPC deadline；
- 没有失败数据补偿；
- Manager 不可用时数据可能直接丢失；
- 没有背压机制，Manager 压力大时 Worker 不会自适应降频。

可以作为优化点讲：

> 后续可以在 Worker 侧增加 bounded queue、本地 WAL、失败重试、deadline、限流和动态采样，避免 Manager 短暂不可用导致监控数据完全丢失。

---

## 4.3 MetricCollector：指标聚合器

`MetricCollector` 是 Worker 端最核心的调度模块。

它本身不负责具体采集，而是统一持有多个 Monitor：

```text
MetricCollector
  |-- HostMonitor
  |-- CpuLoadMonitor
  |-- CpuStatMonitor
  |-- CpuSoftIrqMonitor
  |-- MemMonitor
  |-- DiskMonitor
  |-- NetMonitor / NetEbpfMonitor
```

每轮采样时，它会：

```text
1. 获取 hostname
2. 写入 MonitorInfo.hostname
3. 遍历 monitors_
4. 调用 monitor->UpdateOnce(&monitor_info)
5. 最终返回完整 MonitorInfo
```

### 4.3.1 面试讲法

> 我把 Worker 端采集逻辑做成了统一的 `MetricCollector`。它相当于采集调度器，每轮采样时依次调用 CPU、内存、磁盘、网络、软中断等模块，把结果写进同一个 Protobuf。这样可以减少 RPC 次数，也保证一轮上报内的指标来自相近时间窗口。

---

# 5. 各指标采集机制

---

## 5.1 CPU 使用率采集：内核模块 + 字符设备 + mmap + 差分计算

这是项目中最值得讲的底层点之一。

### 5.1.1 为什么不能直接拿到 CPU 使用率

Linux 内核中的 CPU 统计值通常是累计时间，不是百分比。

比如 CPU 时间大致包括：

```text
user
nice
system
idle
iowait
irq
softirq
steal
```

这些字段本质是从系统启动到当前时刻累计的 CPU 时间。因此要得到使用率，必须做两次采样差分。

### 5.1.2 项目中的采集链路

```text
内核态：cpu_stat_collector.c
  -> 周期性读取 kcpustat_cpu(cpu).cpustat[]
  -> 将每个 CPU 的累计时间写入共享结构
  -> 通过字符设备 /dev/cpu_stat_monitor 暴露
  -> 支持 mmap

用户态：CpuStatMonitor
  -> open('/dev/cpu_stat_monitor')
  -> mmap 共享区域
  -> 读取当前 CPU 累计时间
  -> 与上一次采样值做 delta
  -> 计算 user/system/idle/iowait/irq/softirq 百分比
  -> 写入 Protobuf
```

### 5.1.3 使用率计算公式

```text
prev_total = prev_user + prev_nice + prev_system + prev_idle + prev_iowait + ...
curr_total = curr_user + curr_nice + curr_system + curr_idle + curr_iowait + ...

total_delta = curr_total - prev_total
idle_delta  = curr_idle  - prev_idle
busy_delta  = total_delta - idle_delta

cpu_usage = busy_delta / total_delta * 100
```

如果要更细粒度，也可以计算：

```text
user_percent    = delta_user    / total_delta * 100
system_percent  = delta_system  / total_delta * 100
iowait_percent  = delta_iowait  / total_delta * 100
softirq_percent = delta_softirq / total_delta * 100
```

### 5.1.4 为什么用 mmap

使用 `mmap` 的原因：

| 方式 | 特点 |
|---|---|
| read | 每次系统调用都要从内核拷贝数据到用户态 |
| ioctl | 适合控制命令，但不适合频繁大批量读统计数据 |
| mmap | 用户态直接映射内核维护的共享区域，减少频繁 read/copy 开销 |

面试表达：

> CPU stat 是高频只读数据，使用字符设备 + mmap 可以减少用户态频繁 read 带来的系统调用和拷贝开销。内核模块定期刷新共享统计区，用户态只需要读取映射内存并做差分计算。

---

## 5.2 CPU Load 采集：优先字符设备，fallback 到 /proc/loadavg

CPU load 模块设计了双路径：

```text
优先路径：/dev/cpu_load_monitor + mmap
fallback：/proc/loadavg
```

但是需要注意：当前仓库的 `worker/src/kmod` 目录里可以看到 CPU stat 和 softirq 的内核模块，而没有明显对应的 `cpu_load_collector.c`。因此更稳妥的说法是：

> CPU load 模块代码上做了字符设备路径和 `/proc/loadavg` fallback，但当前仓库里 load 字符设备对应的内核模块不完整，所以实际可运行路径更可能是 `/proc/loadavg`。

### 5.2.1 load average 的含义

Linux load average 表示一段时间内系统的平均负载，通常包括：

- 正在运行的任务；
- 等待 CPU 的可运行任务；
- 不可中断睡眠状态任务，常见于 I/O 等待。

因此：

> Load average 不等于 CPU 使用率。CPU 使用率高通常说明 CPU 忙；load 高可能是 CPU 忙，也可能是大量任务卡在磁盘 I/O、网络 I/O 或内核不可中断等待上。

### 5.2.2 面试区分

| 指标 | 含义 | 典型问题 |
|---|---|---|
| CPU usage | CPU 时间被 busy 占用的比例 | CPU 计算压力 |
| Load average | 可运行队列 + 不可中断任务压力 | CPU 饱和、I/O 阻塞、任务堆积 |
| iowait | CPU 等待 I/O 完成的时间占比 | 磁盘/存储瓶颈 |

---

## 5.3 内存采集：读取 /proc/meminfo

内存模块读取：

```text
/proc/meminfo
```

常见字段包括：

```text
MemTotal
MemFree
MemAvailable
Buffers
Cached
Dirty
Writeback
AnonPages
Mapped
SReclaimable
```

### 5.3.1 使用 MemAvailable 而不是 MemFree

Linux 会尽可能把空闲内存用于 page cache 和 buffer，因此 `MemFree` 通常很低，但这不代表系统真的没内存。

更合理的可用内存判断是：

```text
MemAvailable
```

项目里的内存使用率逻辑可以概括为：

```text
used_percent = (MemTotal - MemAvailable) / MemTotal * 100
```

### 5.3.2 面试讲法

> 内存模块读取 `/proc/meminfo`，但判断内存压力时不直接用 `MemFree`，而是用 `MemAvailable`。因为 Linux 会把空闲内存用于 page cache，必要时这些缓存可以回收，所以 `MemAvailable` 更接近真实可分配内存。

---

## 5.4 磁盘采集：读取 /proc/diskstats 并做差分

磁盘模块读取：

```text
/proc/diskstats
```

它会跳过：

```text
loop*
ram*
```

重点解析字段：

```text
reads_completed
sectors_read
read_time_ms
writes_completed
sectors_written
write_time_ms
io_time_ms
```

### 5.4.1 磁盘吞吐计算

`/proc/diskstats` 中的 sector 通常按 512 bytes 计算。

```text
read_bytes_per_sec  = delta(sectors_read)    * 512 / delta_time
write_bytes_per_sec = delta(sectors_written) * 512 / delta_time
```

### 5.4.2 IOPS 计算

```text
read_iops  = delta(reads_completed)  / delta_time
write_iops = delta(writes_completed) / delta_time
```

### 5.4.3 平均延迟计算

```text
avg_read_latency_ms  = delta(read_time_ms)  / delta(reads_completed)
avg_write_latency_ms = delta(write_time_ms) / delta(writes_completed)
```

### 5.4.4 util 计算

```text
util_percent = delta(io_time_ms) / (delta_time_ms) * 100
```

或者等价写法：

```text
util_percent = delta(io_time_ms) / (delta_time_seconds * 1000) * 100
```

### 5.4.5 面试讲法

> 磁盘模块读取 `/proc/diskstats`，这些字段大多是累计值，所以也必须基于前后两次采样做差分。通过 sector delta 可以算吞吐，通过 read/write completed delta 可以算 IOPS，通过 read/write time delta 可以估算平均 I/O 延迟，通过 io_time 可以估算磁盘繁忙程度。

---

## 5.5 网络采集：/proc/net/dev fallback + eBPF TC Hook 增强路径

网络模块有两条路径。

### 5.5.1 普通路径：/proc/net/dev

普通 `NetMonitor` 读取：

```text
/proc/net/dev
```

解析网卡维度的累计值：

```text
rx_bytes
rx_packets
rx_errs
rx_drop
tx_bytes
tx_packets
tx_errs
tx_drop
```

然后通过差分计算：

```text
rx_bytes_per_sec = delta(rx_bytes) / delta_time
tx_bytes_per_sec = delta(tx_bytes) / delta_time
rx_packets_per_sec = delta(rx_packets) / delta_time
tx_packets_per_sec = delta(tx_packets) / delta_time
```

### 5.5.2 eBPF 路径：TC ingress/egress Hook

增强版 `NetEbpfMonitor` 使用 eBPF。

核心链路：

```text
eBPF 程序 net_stats.bpf.c
  -> 挂载到 TC ingress / egress
  -> 每个 skb 经过时读取 skb->ifindex 和 skb->len
  -> 按网卡 ifindex 更新 BPF map
  -> 用户态 NetEbpfMonitor 读取 BPF map
  -> 计算 bps / pps
  -> 写入 Protobuf
```

### 5.5.3 为什么 eBPF 更有价值

| 方式 | 数据来源 | 特点 |
|---|---|---|
| `/proc/net/dev` | 内核累计统计导出 | 简单稳定，适合基础网卡流量统计 |
| eBPF TC Hook | 包经过内核网络路径时执行 | 更贴近包处理链路，扩展性强，可继续做连接级、协议级、延迟/重传分析 |

面试表达：

> `/proc/net/dev` 是从用户态周期性读取网卡累计统计值，属于轮询模型；eBPF TC Hook 是把统计逻辑挂在内核网络路径上，在包经过 ingress/egress 时直接更新 BPF map。当前项目主要统计网卡吞吐和 PPS，但这个架构可以继续扩展到 TCP RTT、重传、丢包、连接状态等更细指标。

### 5.5.4 注意边界

不能说：

> 项目已经实现 TCP 连接级别诊断。

更准确：

> 当前 eBPF 主要做网卡维度流量统计，粒度是 interface-level，不是完整 TCP flow-level observability。

---

## 5.6 软中断采集：内核模块 + mmap + 每秒速率

软中断模块是另一个系统方向亮点。

### 5.6.1 内核态采集

内核模块周期性读取类似：

```text
kstat_softirqs_cpu(NET_RX_SOFTIRQ, cpu)
kstat_softirqs_cpu(NET_TX_SOFTIRQ, cpu)
kstat_softirqs_cpu(BLOCK_SOFTIRQ, cpu)
kstat_softirqs_cpu(TIMER_SOFTIRQ, cpu)
kstat_softirqs_cpu(RCU_SOFTIRQ, cpu)
```

然后通过：

```text
/dev/cpu_softirq_monitor
mmap
```

暴露给用户态。

### 5.6.2 用户态速率化

软中断计数也是累计值，所以用户态需要做差分：

```text
softirq_rate = delta(softirq_count) / delta_time
```

### 5.6.3 为什么软中断和网络性能有关

Linux 网络收包链路中，网卡中断之后通常会进入 NAPI poll，很多包处理工作会放在软中断上下文中执行，尤其是 `NET_RX_SOFTIRQ`。

如果出现：

```text
NET_RX_SOFTIRQ 很高
rx_packets_per_sec 很高
CPU softirq_percent 很高
```

可能说明：

- 网络包 PPS 很高；
- 协议栈处理压力大；
- 单核软中断热点；
- 网卡队列/RSS/RPS 配置不合理；
- 上层应用消费速度跟不上。

面试表达：

> 我把软中断作为网络性能分析的辅助指标。单看网卡吞吐只能知道流量大小，但结合 `NET_RX_SOFTIRQ`、CPU softirq 时间占比和 PPS，可以判断压力是否集中在内核网络包处理路径上。

---

# 6. Protobuf 与 gRPC 设计

## 6.1 MonitorInfo 数据结构

`MonitorInfo` 是 Worker 上报给 Manager 的总数据结构，里面聚合多类指标。

可以理解为：

```text
MonitorInfo
  |-- hostname
  |-- host info
  |-- cpu load
  |-- cpu stat
  |-- cpu softirq
  |-- mem info
  |-- disk info
  |-- net info
```

### 6.1.1 为什么用 Protobuf

| 方案 | 特点 |
|---|---|
| JSON | 易读，但体积较大，解析开销较高，字段类型约束弱 |
| Protobuf | 二进制编码，体积小，schema 明确，跨语言支持好，适合 RPC |

面试表达：

> 监控指标上报频率较高，字段也比较固定，因此用 Protobuf 比 JSON 更合适。它能通过 `.proto` 统一定义数据结构，生成 C++ 类型，减少手写序列化错误，也方便 Manager 和 Worker 独立演进。

---

## 6.2 gRPC 服务

项目里主要有两类 gRPC 服务：

### 6.2.1 Worker -> Manager 数据上报服务

核心接口：

```text
SetMonitorInfo(MonitorInfo) -> Response
```

主链路是 Worker 主动调用该接口。

需要注意：`.proto` 中也存在 `GetMonitorInfo`，但从当前 Worker 主逻辑看，实际主链路是 Push，`GetMonitorInfo` 更像早期 Pull 模型或测试接口残留。

### 6.2.2 Manager 对外查询服务

`QueryService` 提供多类查询接口：

```text
QueryPerformance
QueryTrend
QueryAnomaly
QueryScoreRank
QueryLatestScore
QueryNetDetail
QueryDiskDetail
QueryMemDetail
QuerySoftIrqDetail
```

### 6.2.3 面试讲法

> gRPC 分两类：第一类是 Worker 到 Manager 的数据上报接口，核心是 `SetMonitorInfo`；第二类是 Manager 对外暴露的查询接口，比如历史性能、趋势、异常、评分排行和各类明细查询。这样采集链路和查询链路在接口层是分离的。

---

# 7. Manager 端详细讲解

---

## 7.1 Manager 启动逻辑

Manager 启动后会创建：

```text
GrpcServerImpl
HostManager
QueryManager
QueryServiceImpl
```

然后注册两个方向的服务：

```text
1. 监控数据接收服务
2. 查询服务
```

### 7.1.1 数据接收 callback

Manager 端通常会给 gRPC 接收服务设置一个 callback：

```text
SetMonitorInfo 收到 MonitorInfo
  -> 调用 HostManager::OnDataReceived(info)
```

这样 RPC 层只负责网络协议处理，业务处理交给 `HostManager`。

---

## 7.2 GrpcServerImpl：接收 Worker 上报

`GrpcServerImpl::SetMonitorInfo` 的职责：

1. 校验 request 是否为空；
2. 提取 hostname；
3. 将最新数据写入内存 map；
4. 触发 callback；
5. 返回响应。

数据流：

```text
Worker gRPC SetMonitorInfo
  -> GrpcServerImpl::SetMonitorInfo
  -> host_data_[hostname] = request
  -> callback(request)
  -> HostManager::OnDataReceived
```

### 7.2.1 为什么先缓存最新数据

`host_data_` 可以理解为 Manager 内存态的“每台主机最新快照”。

用途：

- 快速查看当前状态；
- 避免每次查询最新数据都访问数据库；
- 为后续在线状态判断、调度选择、实时面板提供基础。

当前实现还比较基础，没有完整的在线/离线判定逻辑，但这个设计方向是合理的。

---

## 7.3 HostManager：Manager 的核心业务模块

`HostManager` 的职责主要包括：

```text
1. 接收 MonitorInfo
2. 提取主机标识
3. 提取 CPU / MEM / DISK / NET / SOFTIRQ 指标
4. 计算健康评分
5. 计算变化率
6. 维护 host_scores_
7. 写入 MySQL
```

### 7.3.1 健康评分模型

项目中的健康评分是一个静态加权模型。

大致逻辑：

```text
score = 100
        - cpu_usage_penalty
        - mem_usage_penalty
        - load_penalty
        - disk_penalty
        - net_penalty
```

典型权重：

```text
CPU    35%
MEM    30%
LOAD   15%
DISK   15%
NET     5%
```

最终分数限制在：

```text
0 ~ 100
```

### 7.3.2 这个评分怎么讲

可以讲：

> Manager 收到每台机器的指标后，会把 CPU、内存、load、磁盘 util、网络带宽占用等归一化成压力项，再按静态权重计算健康分。这个分数不是复杂算法，优点是可解释、实现简单，可以用于节点排序、异常筛选和后续调度策略。

不能讲：

> 使用机器学习实现智能健康评分。

### 7.3.3 评分模型的问题

当前静态权重存在问题：

| 问题 | 原因 |
|---|---|
| 不同业务权重不同 | CPU 密集型、IO 密集型、网络密集型服务的瓶颈不同 |
| 不同机器规格不同 | 8 核机器和 64 核机器 load 可比性不同 |
| 网络带宽上限不同 | 1Gbps 和 10Gbps 网卡不能用同一阈值 |
| 磁盘类型不同 | HDD、SATA SSD、NVMe 的 util 和延迟含义不同 |

优化方向：

```text
1. 按机器规格做归一化
2. 按业务类型配置权重模板
3. 引入动态阈值
4. 引入历史基线
5. 将健康分拆成 CPU/MEM/IO/NET 子分数
```

---

## 7.4 MySQL 持久化

Manager 会把数据写入 MySQL。

主要表可以理解为：

```text
server_performance      主性能汇总表
server_net_detail       网络接口明细
server_disk_detail      磁盘设备明细
server_mem_detail       内存明细
server_softirq_detail   软中断明细
```

### 7.4.1 写库链路

```text
HostManager::OnDataReceived
  -> CalcScore
  -> 提取各类指标
  -> 构造 SQL
  -> INSERT server_performance
  -> INSERT detail tables
```

### 7.4.2 当前写库问题

当前写库方式偏 demo：

| 问题 | 影响 |
|---|---|
| MySQL 配置硬编码 | 部署不灵活，密码暴露风险 |
| 每次写入连接数据库 | 高并发下连接开销大 |
| SQL 字符串拼接 | 可能有 SQL 注入/特殊字符破坏 SQL 风险 |
| 无 prepared statement | 性能和安全性较差 |
| 无批量写入 | 多 Worker 高频上报时写入放大 |
| 无异步队列 | RPC 接收线程可能被 DB 写入拖慢 |
| 无失败重试 | DB 短暂不可用时数据丢失 |

### 7.4.3 优化方向

建议按优先级优化：

```text
P0: 配置文件 / 环境变量替代硬编码
P0: prepared statement 替代字符串拼接
P1: MySQL 连接池
P1: 接收线程和写库线程解耦，使用异步队列
P1: 批量写入
P2: 写失败重试 + 本地缓冲
P2: MySQL 换成更适合时序数据的 Prometheus / ClickHouse / InfluxDB
```

---

## 7.5 QueryManager 与 QueryService

### 7.5.1 QueryService 职责

QueryService 是 Manager 对外查询入口。

接口包括：

| 接口 | 作用 |
|---|---|
| QueryPerformance | 查询某台机器一段时间内的性能数据 |
| QueryTrend | 查询趋势和变化率 |
| QueryAnomaly | 查询异常数据 |
| QueryScoreRank | 查询健康评分排行 |
| QueryLatestScore | 查询最新健康分 |
| QueryNetDetail | 查询网络接口明细 |
| QueryDiskDetail | 查询磁盘设备明细 |
| QueryMemDetail | 查询内存明细 |
| QuerySoftIrqDetail | 查询软中断明细 |

### 7.5.2 查询链路

```text
外部客户端
  -> QueryServiceImpl
  -> QueryManager
  -> MySQL SELECT
  -> Protobuf Response
  -> 返回客户端
```

### 7.5.3 面试讲法

> Manager 不只是接收和存储数据，还抽象了查询服务。查询层通过 QueryService 暴露 gRPC API，内部由 QueryManager 访问 MySQL。这样采集写入链路和查询读取链路相对分离，后续可以把查询层替换成 HTTP API 或接入可视化面板。

---

# 8. 这个项目的技术亮点

---

## 8.1 亮点一：从内核态到用户态的 CPU/软中断采集链路

这个点区别于普通“读 `/proc` 文件”的监控 demo。

关键机制：

```text
内核模块
  -> 读取 kcpustat / kstat_softirqs
  -> 字符设备
  -> mmap
  -> 用户态差分计算
  -> Protobuf 上报
```

面试价值：

- 能讲 Linux 内核统计结构；
- 能讲字符设备；
- 能讲 mmap；
- 能讲累计值差分；
- 能讲 CPU usage、iowait、softirq 的含义。

---

## 8.2 亮点二：eBPF TC Hook 网络统计

关键机制：

```text
TC ingress/egress
  -> eBPF program
  -> 读取 skb->ifindex / skb->len
  -> 更新 BPF map
  -> 用户态 libbpf 读取
  -> 计算吞吐和 PPS
```

面试价值：

- 能讲 eBPF 的运行位置；
- 能讲 TC 和 XDP 的区别；
- 能讲 BPF map；
- 能讲用户态和内核态通过 map 交换数据；
- 能讲为什么 eBPF 比纯 `/proc` 更有扩展空间。

---

## 8.3 亮点三：Worker-Agent + Manager 的分布式架构

它不是单机脚本，而是分布式 Agent 架构。

关键点：

```text
Worker 负责采集
Manager 负责接收、评分、持久化、查询
Protobuf 定义数据结构
gRPC 负责跨机器通信
```

面试价值：

- 能讲 Push/Pull 模型；
- 能讲 RPC 接口设计；
- 能讲 Manager 压力和背压；
- 能讲多 Worker 上报时的数据一致性和可靠性。

---

## 8.4 亮点四：Manager 健康评分与查询服务

Manager 不是只落库，还做了健康评分和查询 API。

关键点：

```text
CPU / MEM / LOAD / DISK / NET
  -> 归一化
  -> 静态权重
  -> 健康分
  -> 排行 / 异常查询 / 趋势查询
```

面试价值：

- 能讲监控数据如何从“指标”变成“决策依据”；
- 能讲评分模型的边界；
- 能讲后续如何改成动态阈值和历史基线。

---

# 9. 项目风险与短板

---

## 9.1 安全性不足

截至 2026-07 的生产加固版本，默认使用 mTLS，Manager 会校验客户端证书并按 SAN
allowlist 区分 Worker 上报与查询客户端；数据库连接信息由运行时环境变量注入。仅设置
`MONITOR_ALLOW_INSECURE_GRPC=1` 的本机联调才会启用明文通道。

风险：

```text
1. 部署侧错误设置本机明文逃生变量会关闭 TLS
2. CA、证书轮换和吊销仍需要部署平台负责
3. Worker SAN 与主机名映射需要由证书签发流程持续维护
4. 请求大小、速率限制和审计日志仍待后续补齐
```

优化：

```text
1. 在 CI/部署单元禁止 `MONITOR_ALLOW_INSECURE_GRPC=1`
2. 自动化证书轮换和吊销
3. 请求大小、速率限制和审计日志
4. 防重放标识与时钟偏差监控
```

---

## 9.2 可靠性不足

Worker Push 失败后没有完整补偿。

风险：

```text
Manager 重启 -> 数据丢失
网络抖动 -> 数据丢失
MySQL 短暂不可用 -> 写库失败
Manager 高压 -> Worker 仍然固定频率上报
```

优化：

```text
Worker bounded queue
本地 WAL
失败重试
指数退避
gRPC deadline
Manager 反压信号
动态采样频率
```

---

## 9.3 存储模型不适合大规模时序数据

MySQL 能做 demo，但监控数据天然是时序数据。

MySQL 问题：

```text
1. 高频 INSERT 压力大
2. 时间范围查询容易变慢
3. 明细表增长快
4. 聚合统计成本高
5. 保留策略和降采样不方便
```

优化：

```text
Prometheus
InfluxDB
VictoriaMetrics
ClickHouse
TimescaleDB
```

如果保持 MySQL，也应该加：

```text
时间索引
主机名索引
分区表
批量写入
冷热数据归档
```

---

## 9.4 eBPF 指标深度不够

当前 eBPF 更偏网卡维度吞吐统计。

还没有实现：

```text
TCP RTT
TCP retransmit
TCP connect latency
accept backlog
socket queue
丢包原因
五元组流量统计
协议级统计
```

优化方向：

```text
tracepoint: tcp:tcp_retransmit_skb
kprobe/tcp_v4_connect
tracepoint: skb:kfree_skb
sockops
TC flow-level classification
```

---

## 9.5 工程配置偏硬编码

问题：

```text
Manager 地址默认硬编码
MySQL 用户名/密码硬编码
采样周期配置不够灵活
没有统一配置文件
```

优化：

```text
YAML / TOML 配置
环境变量
命令行参数
配置热更新
配置校验
```

---

# 10. 面试讲解模板

---

## 10.1 30 秒版本

> 这是一个基于 C++/gRPC/Protobuf 的 Linux 分布式性能监控系统。Worker 部署在被监控服务器上，通过 `/proc`、字符设备 `mmap`、内核模块和 eBPF 采集 CPU、内存、磁盘、网络和软中断指标，封装为 Protobuf 后周期性 Push 到 Manager。Manager 接收数据后维护主机状态，计算健康评分和变化率，写入 MySQL，并提供历史性能、趋势、异常、评分排行和明细查询等 gRPC 接口。

---

## 10.2 1 分钟版本

> 这个项目采用 Worker-Agent + Manager 架构。Worker 端负责本机指标采集，CPU stat 和 softirq 使用内核模块周期性读取内核统计结构，并通过字符设备和 `mmap` 暴露给用户态；内存和磁盘分别读取 `/proc/meminfo`、`/proc/diskstats`；网络模块支持 `/proc/net/dev` 差分统计，也支持 eBPF TC Hook 在 ingress/egress 路径统计收发流量。所有指标由 `MetricCollector` 聚合到 `MonitorInfo`，通过 gRPC `SetMonitorInfo` 上报。Manager 端收到数据后由 `HostManager` 计算健康分、变化率并落库，同时通过 `QueryService` 提供查询接口。

---

## 10.3 2 分钟深入版本

> 项目的核心链路分为采集、上报、处理、查询四层。采集层在 Worker 端实现，CPU 使用率不是直接读一个百分比，而是采集 user/system/idle/iowait 等累计时间后做两次采样差分；软中断同样通过内核模块读取每 CPU 的 softirq 计数，再由用户态计算每秒速率。网络监控有两套实现，基础路径读 `/proc/net/dev`，增强路径用 eBPF 挂到 TC ingress/egress，通过 BPF map 暴露每个网卡的 bytes 和 packets。上报层使用 Protobuf 定义统一的 `MonitorInfo`，用 gRPC Push 到 Manager。处理层由 Manager 的 `HostManager` 完成，收到数据后计算 CPU、内存、load、磁盘和网络维度的健康评分，并写入 MySQL 主表和明细表。查询层通过 `QueryService` 暴露历史性能、趋势、异常、评分排行以及网络/磁盘/内存/软中断明细查询。项目目前更偏系统方向原型，后续可以补连接池、异步写库、TLS/mTLS、失败重试、时序数据库和更细粒度 eBPF 网络诊断指标。

---

# 11. 简历项目描述

## 11.1 项目名称

**基于 C++/gRPC/eBPF 的 Linux 分布式性能监控系统**

## 11.2 项目简介

基于 Worker-Agent + Manager 架构实现 Linux 分布式服务器性能监控系统。Worker 部署在被监控服务器上，采集 CPU、内存、磁盘、网络、软中断等指标，并通过 Protobuf/gRPC 周期性推送到 Manager；Manager 负责指标接收、健康评分、变化率计算、MySQL 持久化和查询服务，对外提供性能查询、趋势分析、异常查询和评分排行等接口。

## 11.3 简历 bullet

1. **设计并实现 Worker-Agent + Manager 分布式监控架构**，Worker 端通过 `MetricCollector` 聚合 CPU、内存、磁盘、网络、软中断等多类指标，封装为 Protobuf `MonitorInfo` 后通过 gRPC 周期性 Push 到 Manager。

2. **实现 CPU 与软中断内核态采集链路**，通过内核模块读取 `kcpustat` 与 `kstat_softirqs` 等内核统计数据，基于字符设备和 `mmap` 暴露给用户态，并通过前后两次采样差分计算 CPU 使用率、iowait 占比和软中断速率。

3. **实现网络性能监控模块**，支持 `/proc/net/dev` 轮询差分统计，同时基于 eBPF TC ingress/egress Hook 按网卡维度统计收发字节数和包数，通过 BPF map 与用户态交互，计算网络吞吐和 PPS。

4. **实现 Manager 端状态管理与健康评分机制**，对 CPU、内存、load、磁盘 util、网络带宽等指标进行归一化加权，计算服务器健康分和变化率，用于节点状态评估、异常筛选和评分排行。

5. **实现监控数据持久化与查询服务**，将性能汇总数据和网络、磁盘、内存、软中断明细写入 MySQL，并基于 gRPC QueryService 提供历史性能、趋势分析、异常查询、最新评分和明细查询接口。

---

# 12. 面试高频追问与回答

---

## Q1：CPU 使用率怎么计算？

CPU 使用率不是内核直接给的百分比，而是通过两次采样的累计 CPU 时间差分计算。

```text
total_delta = curr_total - prev_total
idle_delta  = curr_idle  - prev_idle
usage       = 1 - idle_delta / total_delta
```

其中 total 包括 user、nice、system、idle、iowait、irq、softirq、steal 等时间。项目里 CPU stat 通过内核模块采集累计时间，再由用户态做差分。

---

## Q2：Load average 和 CPU 使用率有什么区别？

CPU 使用率表示 CPU 时间被 busy 消耗的比例；load average 表示系统运行队列和不可中断任务的平均压力。

所以：

```text
CPU usage 高 -> CPU 忙
load 高      -> 任务排队或 I/O 阻塞严重
```

load 高但 CPU 不高，常见原因是磁盘 I/O、网络 I/O 或锁等待导致大量任务处于不可中断状态。

---

## Q3：为什么内存使用率看 MemAvailable 而不是 MemFree？

Linux 会把空闲内存用于 page cache 和 buffer。`MemFree` 低不代表系统没有可用内存，因为 cache 可以被回收。`MemAvailable` 会估算当前系统还能分配给应用的内存，因此更适合作为内存压力判断依据。

---

## Q4：为什么用 mmap 传递 CPU/软中断数据？

CPU 和软中断属于周期性读取的只读统计数据。如果每次都用 read/ioctl，用户态需要频繁系统调用并发生数据拷贝。使用字符设备 + mmap 后，内核模块维护共享统计区域，用户态直接读取映射内存，可以降低频繁采样的系统调用和拷贝开销。

---

## Q5：eBPF 相比 /proc/net/dev 好在哪里？

`/proc/net/dev` 是用户态周期性读取内核导出的累计网卡统计，属于轮询模型。eBPF TC Hook 可以把统计逻辑挂到 ingress/egress 包处理路径上，包经过时直接更新 BPF map，更接近真实网络处理链路，也方便后续扩展到 TCP RTT、重传、丢包、连接级统计等指标。

---

## Q6：这个项目为什么不用 Prometheus？

Prometheus 是成熟的时序监控系统，生产上更完善。这个项目重点不是替代 Prometheus，而是自己实现 Linux 指标采集、Agent 上报、Manager 接收处理和查询链路。MySQL 用于简化落库和查询展示。后续如果要生产化，可以把存储层替换成 Prometheus、VictoriaMetrics、InfluxDB 或 ClickHouse。

---

## Q7：Push 模型有什么问题？

Push 模型下 Worker 主动上报，Manager 不需要主动探测所有机器，但问题是 Manager 需要承受多 Worker 同时上报的压力。如果 Manager 不可用，Worker 侧需要重试、本地缓冲和背压控制。当前项目这部分还比较基础，可以优化为 bounded queue + WAL + retry + deadline + 动态采样。

---

## Q8：健康评分公式合理吗？

当前健康评分是静态加权模型，优点是简单、可解释，适合快速判断机器状态。但它不够自适应，不同业务类型和机器规格下，CPU、内存、磁盘、网络的权重应该不同。后续可以按机器规格归一化，按业务类型配置权重模板，并结合历史基线做动态阈值。

---

## Q9：这个项目最大工程短板是什么？

主要短板有：

```text
1. mTLS 与 SAN 授权已落地，但证书轮换/吊销和部署侧禁用明文逃生仍待完善
2. MySQL 凭据已改为运行时环境变量注入
3. 写库是同步字符串拼接 SQL
4. 没有连接池、批量写入和 prepared statement
5. Worker 上报失败没有本地缓冲和重试补偿
6. MySQL 不适合大规模时序监控数据
7. eBPF 当前只做网卡维度流量统计，诊断深度还不够
```

---

## Q10：如果继续优化，你会怎么做？

按优先级：

```text
P0: 配置文件化，去掉硬编码 MySQL 密码和 Manager 地址
P0: gRPC 增加 deadline，避免请求无限阻塞
P0: MySQL prepared statement，避免字符串拼接 SQL
P1: Manager 写库异步化，引入队列和连接池
P1: Worker 增加失败重试、本地缓冲和指数退避
P1: 证书自动轮换、吊销和部署侧禁止明文逃生
P2: 存储层替换为时序数据库
P2: eBPF 扩展 TCP RTT、重传、丢包、连接维度指标
P2: 增加告警规则、异常收敛和可视化面板
```

---

# 13. 推荐学习顺序

如果要快速掌握这个项目，建议按下面顺序学。

## 13.1 第一阶段：先掌握主链路

目标：能讲清楚 Worker 到 Manager 的完整数据流。

学习文件：

```text
worker/src/main.cpp
worker/src/rpc/monitor_pusher.cpp
worker/src/monitor/metric_collector.cpp
proto/monitor_info.proto
manager/src/main.cpp
manager/src/rpc/grpc_server.cpp
manager/src/host_manager.cpp
```

掌握问题：

```text
1. Worker 怎么启动？
2. MonitorPusher 怎么周期上报？
3. MetricCollector 怎么聚合指标？
4. MonitorInfo 里有什么字段？
5. Manager 怎么接收 SetMonitorInfo？
6. HostManager 收到后做了什么？
```

---

## 13.2 第二阶段：掌握各指标机制

目标：能把每类指标的数据源和计算方式讲清楚。

学习文件：

```text
worker/src/monitor/cpu_stat_monitor.cpp
worker/src/kmod/cpu_stat_collector.c
worker/src/monitor/cpu_softirq_monitor.cpp
worker/src/kmod/softirq_collector.c
worker/src/monitor/mem_monitor.cpp
worker/src/monitor/disk_monitor.cpp
worker/src/monitor/net_monitor.cpp
worker/src/monitor/net_ebpf_monitor.cpp
worker/src/ebpf/net_stats.bpf.c
```

掌握问题：

```text
1. CPU 使用率为什么要差分？
2. mmap 在这里解决什么问题？
3. softirq 和网络收包有什么关系？
4. /proc/meminfo 里 MemAvailable 为什么重要？
5. /proc/diskstats 怎么算吞吐、IOPS、延迟、util？
6. /proc/net/dev 和 eBPF 网络统计有什么区别？
```

---

## 13.3 第三阶段：掌握 Manager 查询与存储

目标：能讲清楚数据落库和查询接口。

学习文件：

```text
manager/src/host_manager.cpp
manager/src/query_manager.cpp
manager/src/rpc/query_service.cpp
proto/query_api.proto
manager/sql/init_server_performance.sql
```

掌握问题：

```text
1. 健康分怎么算？
2. MySQL 写了哪些表？
3. 查询接口有哪些？
4. 当前 SQL 写法有什么风险？
5. 为什么监控数据更适合时序数据库？
```

---

## 13.4 第四阶段：准备优化方案

目标：让项目从“能跑”提升到“面试能打”。

优先准备：

```text
1. gRPC deadline / retry
2. Worker 本地缓冲
3. Manager 异步写库队列
4. MySQL 连接池
5. prepared statement
6. TLS/mTLS
7. Prometheus/ClickHouse 存储替换
8. eBPF TCP 指标扩展
9. 告警规则引擎
```

---

# 14. 最终评价

这个项目在 C++ 校招里属于 **中等偏上系统方向项目**。

它的优势是：

```text
1. 不是普通 CRUD
2. 有 Linux 指标语义
3. 有内核模块和 mmap
4. 有 eBPF
5. 有 gRPC/Protobuf 分布式通信
6. 有 Manager 状态管理和查询服务
7. 可以扩展到基础架构监控/调度/诊断方向
```

它的短板是：

```text
1. 工程化一般
2. 可靠性不足
3. 安全性不足
4. 存储模型不够专业
5. eBPF 深度还停留在基础流量统计
6. 部分代码存在 demo/残留接口痕迹
```

因此最准确的简历定位是：

> **基于 C++/gRPC/eBPF 的 Linux 分布式性能监控系统原型。重点展示 Linux 底层指标采集、Worker-Agent 分布式上报、Manager 健康评分和查询服务能力。**

如果面试目标是 C++ 后端 / 基础架构 / Linux 系统方向，这个项目值得放；但一定要准备好工程短板和优化方案，主动把它讲成“系统方向项目 + 可生产化演进路线”，而不是硬吹成成熟监控平台。
