# Core Flow Trace: Worker Push 监控数据上报与 Manager 入库链路

## 1. 链路总览

`worker main` -> `MonitorPusher::Start` -> `MonitorPusher::PushLoop` -> `MonitorPusher::PushOnce` -> `MetricCollector::CollectAll` -> `MonitorInter::UpdateOnce` -> `GrpcManager::SetMonitorInfo` -> `GrpcServerImpl::SetMonitorInfo` -> `HostManager::OnDataReceived` -> `HostManager::CalcScore` -> `HostManager::WriteToMysql` -> `MySQL`

## 2. 逐层调用表

| 顺序 | 函数 | 文件 | 职责 | 输入 | 输出 |
|---|---|---|---|---|---|
| 1 | `main` | `worker/src/main.cpp` | 解析 manager 地址和推送间隔，创建推送器 | `argv[1]` manager 地址，`argv[2]` interval | `MonitorPusher` 对象 |
| 2 | `MonitorPusher::MonitorPusher` | `worker/src/rpc/monitor_pusher.cpp` | 创建 gRPC channel/stub 和 `MetricCollector` | `manager_address`、`interval_seconds` | `stub_`、`collector_` |
| 3 | `MonitorPusher::Start` | `worker/src/rpc/monitor_pusher.cpp` | 启动后台推送线程 | 无业务输入 | `thread_` 开始运行 |
| 4 | `MonitorPusher::PushLoop` | `worker/src/rpc/monitor_pusher.cpp` | 周期调用 `PushOnce`，按 interval sleep | `running_` 状态 | 多次 Push 调用 |
| 5 | `MonitorPusher::PushOnce` | `worker/src/rpc/monitor_pusher.cpp` | 创建 `MonitorInfo`，采集指标，发起 gRPC Push | 空 `MonitorInfo` | gRPC `Status`，bool 成功/失败 |
| 6 | `MetricCollector::CollectAll` | `worker/src/monitor/metric_collector.cpp` | 设置主机名，逐个 monitor 填充 protobuf | `MonitorInfo*` | 填满 CPU/Load/内存/磁盘/网络/主机信息 |
| 7 | `MonitorInter::UpdateOnce` 系列 | `worker/src/monitor/*.cpp` | 从系统采集具体指标 | `MonitorInfo*` | protobuf 子字段 |
| 8 | `GrpcManager::Stub::SetMonitorInfo` | 由 `proto/monitor_info.proto` 生成 | 通过 gRPC 把 `MonitorInfo` 发给 manager | `MonitorInfo` | `google.protobuf.Empty` 或错误状态 |
| 9 | `GrpcServerImpl::SetMonitorInfo` | `manager/src/rpc/grpc_server.cpp` | 校验请求、缓存原始数据、触发回调 | `MonitorInfo* request` | gRPC `Status` |
| 10 | `HostManager::OnDataReceived` | `manager/src/host_manager.cpp` | 构造 host 标识，计算评分和变化率，更新内存态，写库 | `const MonitorInfo& info` | `host_scores_` 更新，MySQL 写入 |
| 11 | `HostManager::CalcScore` | `manager/src/host_manager.cpp` | 按 CPU/内存/负载/磁盘/网络权重算健康分 | `MonitorInfo` | `double score` |
| 12 | `HostManager::WriteToMysql` | `manager/src/host_manager.cpp` | 写主表和详情表 | `host_name`、`HostScore`、变化率 | MySQL rows |

## 3. 数据流

原始输入：

`worker` 进程启动参数：`manager_address` 和 `interval_seconds`。真正的业务数据来自本机 Linux 运行时状态，包括 `/proc/loadavg`、`/proc/meminfo`、`/proc/diskstats`、`/proc/net/dev`、`/dev/cpu_stat_monitor`、`/dev/cpu_softirq_monitor`，以及启用 eBPF 时的 BPF map。

中间结构：

核心中间结构是 `monitor::proto::MonitorInfo`，定义在 `proto/monitor_info.proto`。`MetricCollector::CollectAll` 会先 `set_name(hostname_)`，再让各 monitor 写入 `cpu_load`、`cpu_stat`、`soft_irq`、`mem_info`、`net_info`、`disk_info`、`host_info`。

进入 manager 后，`GrpcServerImpl::SetMonitorInfo` 会把原始 `MonitorInfo` 缓存在 `host_data_`。随后 `HostManager::OnDataReceived` 把它转换成 `HostScore{info, score, timestamp}`，并计算 `PerfSample` 变化率。

最终输出：

1. manager 内存态：`GrpcServerImpl::host_data_` 和 `HostManager::host_scores_`
2. MySQL 持久化：`server_performance`、`server_net_detail`、`server_softirq_detail`、`server_mem_detail`、`server_disk_detail`
3. gRPC 返回：`GrpcServerImpl::SetMonitorInfo` 返回 `grpc::Status::OK`，worker 侧 `PushOnce` 返回 `true`

## 4. 状态变化

哪些对象/成员变量/DB/缓存/文件被修改：

`worker` 侧：

- `MonitorPusher::running_`：`Start()` 置为 `true`，`Stop()` 置为 `false`
- `MonitorPusher::thread_`：保存后台 push 线程
- `MetricCollector::monitors_`：构造时保存多个 `unique_ptr<MonitorInter>`
- `CpuStatMonitor::cpu_stat_map_`：保存上次 CPU tick，用于计算 CPU 百分比
- `CpuSoftIrqMonitor::cpu_softirqs_`：保存上次软中断计数和时间点
- `NetMonitor::last_net_info_` 或 `NetEbpfMonitor::cache_`：保存网络计数器快照
- `DiskMonitor` 文件级 `last_samples` / `last_time`：保存磁盘计数器快照

`manager` 侧：

- `GrpcServerImpl::host_data_`：以 hostname 为 key 缓存最近一次原始上报
- `HostManager::host_scores_`：以 `hostname_ip` 或 hostname 为 key 缓存评分和时间戳
- `last_perf_samples`：文件级静态 map，用于计算 CPU/内存/负载/网络变化率
- `last_net_samples`、`last_softirq_samples`、`last_mem_samples`、`last_disk_samples`：文件级静态 map，用于各详情表变化率
- MySQL：插入主性能表和四张详情表

## 5. 错误路径

| 错误点 | 当前处理方式 | 风险 |
|---|---|---|
| `worker/src/main.cpp` 中 `std::stoi(argv[2])` 参数非法 | 未捕获异常 | 非数字 interval 会直接终止进程 |
| interval 小于等于 0 | 回退默认 10 秒 | 可接受，但没有日志说明输入被修正 |
| gRPC channel 创建 | `CreateChannel` 不立即报错 | manager 地址错误会延迟到 RPC 调用才暴露 |
| `MetricCollector::CollectAll(nullptr)` | 直接 return | 静默失败，调用方不知道未采集 |
| `/dev/cpu_stat_monitor` 不存在 | `CpuStatMonitor::UpdateOnce` 直接 return | CPU stat 缺失但 worker 仍继续上报 |
| `/dev/cpu_softirq_monitor` 不存在 | `CpuSoftIrqMonitor::UpdateOnce` 静默 return | softirq 数据缺失不明显 |
| `/proc/*` 读取失败 | 多数路径返回空数据或不填字段 | manager 会用默认 0 参与评分，可能误判健康 |
| eBPF 初始化失败 | `NetEbpfMonitor` 打日志，`loaded_ = false` | 如果编译期启用 eBPF，运行期失败后该 monitor 不自动退到 `/proc/net/dev` |
| gRPC `SetMonitorInfo` 失败 | worker 打印 `status.error_message()` 并返回 false | 没有重连退避、队列缓存或数据补偿 |
| manager 收到空 request | 返回 `INVALID_ARGUMENT` | 正常 |
| manager 收到空 hostname | 返回 `INVALID_ARGUMENT` 或 `OnDataReceived` 内 return | 该批数据丢弃 |
| `mysql_init` 失败 | 打印错误并 return | gRPC 仍返回 OK，worker 认为 Push 成功 |
| `mysql_real_connect` 失败 | 打印错误、关闭连接、return | 数据只进入内存态，不落库；上游不知道 |
| `mysql_query` 插入失败 | 当前基本未检查返回值 | 写库失败可能被吞掉，日志不可见 |
| `BuildAndStart()` 失败 | 未检查 `server` 是否为空 | 可能空指针调用 `Wait()` |

## 6. 风险分析

并发：

`GrpcServerImpl::host_data_` 和 `HostManager::host_scores_` 都有 mutex 保护，这是正向设计。但 `HostManager::OnDataReceived` 里更新的多个文件级 `static std::map` 没有锁，比如 `last_perf_samples`、`last_net_samples` 等。如果 gRPC server 并发处理多个 worker 上报，这些变化率缓存存在数据竞争风险。

生命周期：

`worker` 的 `MonitorPusher` 是 `main` 栈对象，但主线程无限 sleep，正常析构路径基本不可达。`MonitorPusher::~MonitorPusher` 和 `MetricCollector::~MetricCollector` 设计了停止逻辑，但当前没有 signal handler 驱动优雅退出。`manager` 里 `QueryServiceImpl` 持有 `QueryManager*` 裸指针，当前因为都在 `main` 栈上且 `server->Wait()` 阻塞，所以生命周期成立；未来如果改异步关闭，需要重看。

资源：

CPU/softirq 采集中的 `open + mmap` 成功路径会 `munmap + close`，基本闭环。MySQL 写入路径每次上报都新建连接，性能和连接数压力较大。eBPF 路径会 attach TC hook，析构时 detach，但默认不删除 `clsact` qdisc，可能有轻微运行环境残留。

异常处理：

worker 参数解析、线程创建、gRPC server 创建都没有异常/失败兜底。MySQL 插入错误缺少统一检查。更关键的是 manager 写库失败不会反馈到 worker，因为 `GrpcServerImpl::SetMonitorInfo` 回调执行完仍返回 `OK`，这会让上游误以为端到端成功。

## 7. 面试版解释

这个项目最核心的链路是 worker 主动 push 监控数据到 manager。worker 启动时从命令行拿到 manager 地址和推送间隔，创建 `MonitorPusher`，后台线程周期执行 `PushOnce`。每次 push 会创建一个 `MonitorInfo` protobuf，由 `MetricCollector` 依次调用 CPU、内存、磁盘、网络、软中断、主机信息这些 monitor，从 `/proc`、内核模块 `/dev` 设备或 eBPF map 里采集数据并填充 protobuf。然后 worker 通过 gRPC 调用 `GrpcManager::SetMonitorInfo` 发给 manager。manager 的 `GrpcServerImpl::SetMonitorInfo` 先校验 hostname 并缓存原始数据，再通过回调进入 `HostManager::OnDataReceived`。这里会生成唯一主机名，基于 CPU、内存、负载、磁盘、网络算健康评分，维护内存里的最新主机状态，同时计算变化率并写入 MySQL 的主表和详情表。风险点主要是 MySQL 写失败不会反馈给 worker，变化率的静态 map 没有锁，以及 worker 缺少优雅退出和参数异常处理。
