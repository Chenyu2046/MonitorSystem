# Worker 与 Manager 指标清单和处理链路

## 1. 文档范围

本文说明当前项目从 Linux 内核和 procfs 采集数据，到 Worker 计算，再到 Manager 接收、评分、诊断和持久化的完整指标链路。

本文只描述当前代码和 protobuf 中已经存在的字段，不把内核原始字段、Worker 派生字段和 Manager 评分字段混为一谈。

## 2. 总体数据流

```text
Linux 内核
  ├─ /proc/loadavg
  ├─ /proc/meminfo
  ├─ /proc/diskstats
  ├─ /proc/net/dev
  ├─ CPU/SoftIRQ 内核模块 + 字符设备 mmap
  └─ eBPF TC ingress/egress + BPF map
          ↓
Worker Monitor
  ├─ 读取原始快照或累计计数
  ├─ 前后采样差分
  ├─ 计算百分比、速率、IOPS、延迟和 util
  └─ 计算本地异常信号和诊断状态
          ↓
MonitorInfo protobuf
          ↓ gRPC SetMonitorInfo
Manager
  ├─ 校验、去重、过期样本过滤
  ├─ CPU/网络/磁盘主机级聚合
  ├─ Resource Score 资源余量评分
  ├─ Health Score 健康评分和历史异常检测
  ├─ 诊断证据和根因分析
  └─ 异步持久化并提供查询接口
```

Worker 的统一采集入口是 `MetricCollector::CollectAll()`；每个 Monitor 都把结果写入同一轮 `MonitorInfo`。随后 `MonitorPusher` 补充采样序号、时间戳、Worker 异常状态和诊断快照，再通过 gRPC 上报。

相关代码：

- [metric_collector.cpp](../../worker/src/monitor/metric_collector.cpp)
- [monitor_pusher.cpp](../../worker/src/rpc/monitor_pusher.cpp)
- [monitor_info.proto](../../proto/monitor_info.proto)

## 3. Worker 采集和计算的指标

### 3.1 CPU Load Average

来源：`/proc/loadavg`。

| 字段 | 含义 | 单位 | Worker 是否计算 |
| --- | --- | --- | --- |
| `load_avg_1` | 最近 1 分钟系统平均负载 | 任务数 | 直接读取 |
| `load_avg_3` | 最近 5 分钟系统平均负载 | 任务数 | 直接读取，字段名历史遗留 |
| `load_avg_15` | 最近 15 分钟系统平均负载 | 任务数 | 直接读取 |

Load Average 表示处于运行态或不可中断睡眠态的任务数量，不等同于 CPU 使用率。

### 3.2 CPU 使用率

CPU 状态由内核模块维护累计计数，Worker 通过 `/dev/cpu_stat_monitor` mmap 读取。内核原始字段为：

```text
user, nice, system, idle, iowait,
irq, softirq, steal, guest, guest_nice
```

Worker 保存上一轮快照，用当前值减去上一值，再计算本轮时间占比：

```text
total = user + nice + system + idle + iowait + irq + softirq + steal
busy  = user + nice + system + irq + softirq + steal
```

当前 `CpuStat` 对 Manager 暴露的字段如下：

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `cpu_name` | CPU 核名称，例如 cpu0 | - |
| `cpu_percent` | 忙碌时间百分比，包含 steal | percent |
| `usr_percent` | 用户态任务占用 | percent |
| `system_percent` | 内核态任务占用 | percent |
| `nice_percent` | nice 任务占用 | percent |
| `idle_percent` | 空闲时间 | percent |
| `io_wait_percent` | 等待 IO | percent |
| `irq_percent` | 硬中断处理 | percent |
| `soft_irq_percent` | 软中断处理 | percent |
| `sample_valid` | 本轮样本是否有效 | bool |

注意：内核原始结构中的 `steal`、`guest`、`guest_nice` 当前没有作为独立 protobuf 字段发送；其中 `steal`参与了 Worker 的 `cpu_percent` 计算。

相关代码：

- [cpu_stat_collector.c](../../worker/src/kmod/cpu_stat_collector.c)
- [cpu_stat_monitor.cpp](../../worker/src/monitor/cpu_stat_monitor.cpp)
- [cpu_stat.proto](../../proto/cpu_stat.proto)
- [内核态到用户态-mmap-数据通路.md](内核态到用户态-mmap-数据通路.md)

### 3.3 SoftIRQ

内核模块按 CPU 核维护累计 SoftIRQ 计数，Worker 通过 `/dev/cpu_softirq_monitor` mmap 读取，并计算：

```text
events_per_sec = (current_count - previous_count) / sample_interval
```

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `hi` | 高优先级软中断 | events/s |
| `timer` | 定时器软中断 | events/s |
| `net_tx` | 网络发送软中断 | events/s |
| `net_rx` | 网络接收软中断 | events/s |
| `block` | 块设备 IO 软中断 | events/s |
| `irq_poll` | IRQ 轮询软中断 | events/s |
| `tasklet` | tasklet 软中断 | events/s |
| `sched` | 调度相关软中断 | events/s |
| `hrtimer` | 高精度定时器软中断 | events/s |
| `rcu` | RCU 回调软中断 | events/s |

相关代码：

- [softirq_collector.c](../../worker/src/kmod/softirq_collector.c)
- [cpu_softirq_monitor.cpp](../../worker/src/monitor/cpu_softirq_monitor.cpp)
- [cpu_softirq.proto](../../proto/cpu_softirq.proto)

### 3.4 内存

来源：`/proc/meminfo`。容量字段在 Worker 中统一转换为 GiB。

| 字段 | 对应含义 | 单位 |
| --- | --- | --- |
| `total` | `MemTotal`，总内存 | GiB |
| `free` | `MemFree`，完全空闲内存 | GiB |
| `avail` | `MemAvailable`，应用可使用内存估计值 | GiB |
| `buffers` | 块设备等内核缓冲区 | GiB |
| `cached` | 文件页缓存 | GiB |
| `swap_cached` | Swap cache | GiB |
| `active` / `inactive` | 活跃/非活跃内存 | GiB |
| `active_anon` / `inactive_anon` | 活跃/非活跃匿名页 | GiB |
| `active_file` / `inactive_file` | 活跃/非活跃文件页 | GiB |
| `dirty` | 已修改但未写回的页 | GiB |
| `writeback` | 正在写回的页 | GiB |
| `anon_pages` | 匿名页 | GiB |
| `mapped` | 被映射的页 | GiB |
| `kReclaimable` | 内核可回收内存 | GiB |
| `sReclaimable` | 可回收 slab | GiB |
| `sUnreclaim` | 不可回收 slab | GiB |
| `used_percent` | `(total - avail) / total * 100` | percent |

`free` 和 `avail` 不相等。`free`只表示当前完全未使用的内存，`avail`还考虑了可回收缓存，判断主机内存压力时通常更关注 `avail`。

当前协议是主机级内存指标，不支持进程级内存归因。

相关代码：[mem_monitor.cpp](../../worker/src/monitor/mem_monitor.cpp)、[mem_info.proto](../../proto/mem_info.proto)。

### 3.5 磁盘 IO

来源：`/proc/diskstats`，Worker 会过滤 `loop` 和 `ram` 设备。

原始累计字段：

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `reads` / `writes` | 完成的读/写请求数 | requests |
| `sectors_read` / `sectors_written` | 读/写扇区数 | sectors |
| `read_time_ms` / `write_time_ms` | 累计读/写耗时 | ms |
| `io_in_progress` | 当前正在处理的 IO 数 | requests |
| `io_time_ms` | 设备累计忙碌时间 | ms |
| `weighted_io_time_ms` | 按并发请求数加权的 IO 时间 | ms |

Worker 根据前后两次累计值计算：

| 字段 | 计算方式 | 单位 |
| --- | --- | --- |
| `read_bytes_per_sec` | 读取扇区差值 × 512 / 时间 | bytes/s |
| `write_bytes_per_sec` | 写入扇区差值 × 512 / 时间 | bytes/s |
| `read_iops` / `write_iops` | 读/写请求差值 / 时间 | requests/s |
| `avg_read_latency_ms` | 读耗时差值 / 读请求差值 | ms/request |
| `avg_write_latency_ms` | 写耗时差值 / 写请求差值 | ms/request |
| `util_percent` | 设备忙碌时间增量 / 采样时间 | percent |

相关代码：[disk_monitor.cpp](../../worker/src/monitor/disk_monitor.cpp)、[disk_info.proto](../../proto/disk_info.proto)。

### 3.6 网络

启用 eBPF 且依赖满足时，Worker 在 TC ingress/egress 路径上统计网卡的累计字节数和包数；用户态读取 BPF map 后按网卡聚合各 CPU 的 per-CPU 值，再和上一轮做差分。

如果 eBPF 不可用，则回退到 `/proc/net/dev` 的 `NetMonitor`。两条路径对外提供相同的 `NetInfo` 字段。

| 字段 | 含义 | 单位/语义 |
| --- | --- | --- |
| `name` | 网卡名称 | - |
| `send_rate` | 发送字节速率 | KiB/s |
| `rcv_rate` | 接收字节速率 | KiB/s |
| `send_packets_rate` | 发送包速率 | packets/s |
| `rcv_packets_rate` | 接收包速率 | packets/s |
| `err_in` / `err_out` | 接收/发送错误 | 累计计数 |
| `drop_in` / `drop_out` | 接收/发送丢弃 | 累计计数 |

错误和丢弃在 Worker 上报时仍是累计值，Manager 会结合上一轮累计值计算每秒变化。

相关代码：

- [net_stats.bpf.c](../../worker/src/ebpf/net_stats.bpf.c)
- [net_ebpf_monitor.cpp](../../worker/src/monitor/net_ebpf_monitor.cpp)
- [net_monitor.cpp](../../worker/src/monitor/net_monitor.cpp)
- [net_info.proto](../../proto/net_info.proto)

### 3.7 Worker 本地异常和诊断指标

Worker 会用基础指标做本地异常判断，主要关注：

```text
最大逐核 CPU 使用率
最大逐核 iowait
最大逐核 softirq
Load Average 和 CPU 核数
内存使用率
最大磁盘 util 和读写延迟
网卡包速率总和
最大网络 SoftIRQ 速率
```

Worker 计算并上报的诊断结果包括：

```text
anomaly_score
DiagnosticSignal
ObservabilityState
ProbeRuntimeStatus
OnCPU profile
OffCPU profile
```

Worker 的 `anomaly_score` 用于决定是否进入诊断或 profiling，不等同于 Manager 的 `health_score`。

## 4. Manager 收到后的处理

### 4.1 接收和校验

Manager 的 `SetMonitorInfo` 收到完整 `MonitorInfo` 后，将消息提交到按主机分片的处理队列。`ProcessOne` 依次执行：

1. 检查主机标识、`sample_valid` 和数值范围。
2. 根据 `sample_session_id`、`sample_sequence` 和时间戳过滤重复或过期样本。
3. 计算主机级概览和评分。
4. 保存内存快路径数据并提交异步持久化任务。

gRPC 返回成功只表示消息被 Manager 接收/接受，不表示 MySQL 已经写入完成。

相关代码：[grpc_server.cpp](../../manager/src/rpc/grpc_server.cpp)、[host_manager.cpp](../../manager/src/host_manager.cpp)。

### 4.2 主机级聚合

CPU：对有效 CPU 核求平均，得到主机 CPU 概览；同时保留最忙 CPU 核及其占用率，异常分析可以使用最大值。

网络：对所有有效网卡求和，得到总收发吞吐和总包速率；同时保留峰值接收网卡和峰值发送网卡。

磁盘：保留逐设备明细，在主机资源评分中主要使用最大磁盘 util、最大读写延迟和磁盘 IOPS。

SoftIRQ：保留逐 CPU 核明细，健康分析重点使用 `net_rx + net_tx` 的峰值速率。

### 4.3 Resource Score

Resource Score 表示主机剩余资源余量，默认权重为：

| 领域 | 权重 |
| --- | ---: |
| CPU | 0.35 |
| Memory | 0.30 |
| Load | 0.15 |
| Disk | 0.15 |
| Network | 0.05 |

各项资源使用越高，资源分数越低，最终归一化到 0~100。兼容字段 `score` 继续表示 Resource Score。

### 4.4 Health Score

Health Score 表示当前是否存在异常，不是简单的资源剩余量。Manager 当前分析 15 类指标：

| 领域 | 指标 |
| --- | --- |
| CPU | 平均 CPU、峰值 CPU、峰值 iowait、峰值 SoftIRQ、Load/CPU 核数 |
| Memory | 内存使用率 |
| Disk | 磁盘 util、磁盘延迟、磁盘 IOPS |
| Network | 包速率、接收吞吐、发送吞吐、丢弃速率、错误速率、网络 SoftIRQ 速率 |

Manager 对指标历史窗口执行静态阈值、EWMA、MAD 和多次采样投票，然后按领域取严重异常并计算整体异常分数。

默认健康领域权重为：

```text
CPU 0.30，Memory 0.20，Disk 0.25，Network 0.15，Scheduler 0.10
```

最终公式为：

```text
health_score = 100 * (1 - anomaly_score)
```

因此需要区分：

```text
resource_score：资源还有多少余量
health_score：当前指标是否表现异常
Worker anomaly_score：是否触发本地诊断策略
```

相关代码：[health_score_engine.cpp](../../manager/src/health/health_score_engine.cpp)、[health_score_engine.h](../../manager/include/health/health_score_engine.h)。

### 4.5 变化率

Manager 缓存每个主机的上一轮概览，计算相对变化率：

```text
rate = (current - previous) / previous
```

变化率用于趋势查询、异常辅助判断和持久化。第一次没有上一轮有效样本时，变化率没有真实基线。

### 4.6 诊断和根因分析

当 `MonitorInfo.diagnostic` 存在时，Manager 继续处理 Worker 的信号、探针状态和 profiling 快照，构建证据并交给 RootCauseEngine，必要时保存 Incident。

典型诊断证据包括：

```text
TCP 重传
Block IO 平均延迟
调度切换和唤醒
OnCPU/OffCPU Top 栈
探针是否请求、可用、已挂载和快照是否成功
```

### 4.7 持久化和查询

Manager 异步保存：

```text
原始 MonitorInfo
CPU/内存/磁盘/网络主机概览
逐网卡明细
逐磁盘明细
内存分类明细
SoftIRQ 明细
Resource Score 和 Health Score
异常率、置信度、状态、Top Signals
变化率
```

查询侧主要分为：

| 查询视图 | 主要内容 |
| --- | --- |
| `PerformanceRecord` | 主机级 CPU、Load、内存、磁盘、网络、评分和变化率 |
| `ServerScoreSummary` | 最新主机状态、关键指标和评分摘要 |
| `NetDetailRecord` | 逐网卡速率、错误、丢弃和变化率 |
| `DiskDetailRecord` | 逐磁盘吞吐、IOPS、延迟、util 和变化率 |
| `MemDetailRecord` | 内存分类容量和变化率 |
| `SoftIrqDetailRecord` | 逐 CPU 核 SoftIRQ 速率和变化率 |
| `Incident` | 异常 episode、证据、根因和严重级别 |

查询协议定义见 [query_api.proto](../../proto/query_api.proto)。

## 5. 面试版总结

> 我们把指标处理分成 Worker 和 Manager 两层。Worker 负责靠近数据源采集，并把内核累计值转换成可用的业务指标：CPU 计算 user、system、idle、iowait、irq、softirq 等百分比，SoftIRQ 计算 events/s，内存计算 used_percent，磁盘计算吞吐、IOPS、平均延迟和 util，网络通过 eBPF map 或 `/proc/net/dev` 计算网卡速率和包速率。Worker 还根据这些指标做一次本地异常判断，决定是否启动诊断探针。
>
> Worker 通过 `MonitorInfo` 把逐核 CPU、逐核 SoftIRQ、逐网卡网络、逐磁盘 IO 和主机级内存发送给 Manager。Manager 先校验和去重，然后把逐实体数据聚合成主机平均值、峰值和总量，再分别计算 Resource Score 和 Health Score。Resource Score 关注资源余量，Health Score 结合历史窗口、静态阈值、EWMA、MAD 和投票判断异常。最后 Manager 计算变化率，处理诊断证据和根因，并异步写入 MySQL，查询接口再提供主机摘要和设备明细。

## 6. 关键边界

1. `MonitorInfo` 是 Worker 到 Manager 的完整指标消息；`MonitorFeedback` 只是 Manager 返回给 Worker 的健康反馈，不包含完整指标。
2. CPU、SoftIRQ、网络速率和磁盘派生值主要由 Worker 计算；Manager 重新做主机级聚合和健康评分。
3. 网络错误/丢弃在 Worker 上报时是累计值，Manager 才按历史样本计算每秒变化。
4. `load_avg_3` 实际是 5 分钟 Load Average，不能按字段名解释成 3 分钟。
5. 当前 CPU protobuf 不单独暴露 `steal`、`guest`、`guest_nice`，不能在 Manager 查询接口中声称这些字段已经完整对外提供。
6. Manager 接收成功不等于 MySQL 写入完成，持久化在异步队列之后执行。
