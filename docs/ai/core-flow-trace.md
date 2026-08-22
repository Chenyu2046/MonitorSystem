# Core Flow Trace: Worker Adaptive Observability 与 Manager 入库链路

## 1. 链路总览

`worker main` -> `MonitorPusher::Start` -> `MonitorPusher::PushLoop` -> `MonitorPusher::PushOnce` -> `MetricCollector::CollectAll` -> `AnomalyDetector::Evaluate` -> `ObservabilityStateMachine::Update` -> `ProbeController::Apply` -> `MonitorInfo.diagnostic` -> `MonitorSendQueue` -> `MonitorPusher::SendLoop` -> `GrpcManager::SetMonitorInfo` -> `GrpcServerImpl::SetMonitorInfo` -> `HostManager::OnDataReceived` -> `EvidenceBuilder` -> `RootCauseEngine` -> `IncidentStore` / `HostManager::CalcScore` -> `MySQL`

## 2. 逐层调用表

| 顺序 | 函数 | 文件 | 职责 | 输入 | 输出 |
|---|---|---|---|---|---|
| 1 | `main` | `worker/src/main.cpp` | 解析 manager 地址和推送间隔，创建推送器 | `argv[1]` manager 地址，`argv[2]` interval | `MonitorPusher` 对象 |
| 2 | `MonitorPusher::MonitorPusher` | `worker/src/rpc/monitor_pusher.cpp` | 创建 gRPC channel/stub 和 `MetricCollector` | `manager_address`、`interval_seconds` | `stub_`、`collector_` |
| 3 | `MonitorPusher::Start` | `worker/src/rpc/monitor_pusher.cpp` | 启动采集线程和发送线程 | 无业务输入 | `thread_`、`sender_thread_` 开始运行 |
| 4 | `MonitorPusher::PushLoop` | `worker/src/rpc/monitor_pusher.cpp` | 周期调用 `PushOnce`，按当前状态等待下一轮 | `running_` 状态 | 多次采集入队 |
| 5 | `MonitorPusher::PushOnce` | `worker/src/rpc/monitor_pusher.cpp` | 创建 `MonitorInfo`，编排采集和诊断控制后写入有界队列 | 空 `MonitorInfo` | 入队成功/丢弃 |
| 6 | `MonitorSendQueue::Push` | `worker/src/rpc/monitor_send_queue.cpp` | 按 item/byte 上限入队，队满时优先保留诊断数据 | `MonitorInfo` | 队列 item |
| 7 | `MonitorPusher::SendLoop` | `worker/src/rpc/monitor_pusher.cpp` | 顺序取出队列数据并发送 | 队列 item | 发送结果 |
| 8 | `MonitorPusher::SendWithRetry` | `worker/src/rpc/monitor_pusher.cpp` | 为每次 unary RPC 设置 deadline，只对指定状态指数退避重试 | `MonitorInfo` | gRPC `Status` |
| 9 | `MetricCollector::CollectAll` | `worker/src/monitor/metric_collector.cpp` | 设置主机名，逐个 monitor 填充 protobuf | `MonitorInfo*` | 填满 CPU/Load/内存/磁盘/网络/主机信息 |
| 10 | `MonitorInter::UpdateOnce` 系列 | `worker/src/monitor/*.cpp` | 从系统采集具体指标 | `MonitorInfo*` | protobuf 子字段 |
| 11 | `AnomalyDetector::Evaluate` | `worker/src/diagnostics/anomaly_detector.cpp` | 根据现有基础指标生成信号和整体异常分数 | `MonitorInfo` | `AnomalyResult` |
| 12 | `ObservabilityStateMachine::Update` | `worker/src/diagnostics/observability_state.cpp` | 以连续样本和恢复条件更新观测状态 | `AnomalyResult` | `ObservabilityState` |
| 13 | `ProbeController::Apply` | `worker/src/diagnostics/probe_controller.cpp` | 按状态加载/卸载独立 eBPF 对象；对象、内核或 BTF 不可用时记录状态并降级 | `ObservabilityState` | Probe status |
| 14 | `GrpcManager::Stub::SetMonitorInfo` | 由 `proto/monitor_info.proto` 生成 | 通过 gRPC 把 `MonitorInfo` 发给 manager；每次调用有 deadline | `MonitorInfo` | `google.protobuf.Empty` 或错误状态 |
| 15 | `GrpcServerImpl::SetMonitorInfo` | `manager/src/rpc/grpc_server.cpp` | 校验请求、缓存原始数据、触发回调 | `MonitorInfo* request` | gRPC `Status` |
| 16 | `HostManager::OnDataReceived` | `manager/src/host_manager.cpp` | 构造 host 标识，计算评分/变化率，并把诊断快照交给证据与根因模块 | `const MonitorInfo& info` | `host_scores_`、IncidentStore |
| 17 | `EvidenceBuilder::Build` | `manager/src/diagnostics/evidence_builder.cpp` | 将基础指标、诊断信号和 Top-N profile 转为带来源的 Evidence | `MonitorInfo` | `vector<Evidence>` |
| 18 | `RootCauseEngine::Evaluate` | `manager/src/diagnostics/root_cause_engine.cpp` | 使用多证据规则计算可解释 confidence，不把单阈值伪装成根因 | `vector<Evidence>` | `vector<RootCause>` |
| 19 | `IncidentStore::Observe` | `manager/src/diagnostics/incident_store.cpp` | 更新活动 Incident，恢复后进入有界历史缓存，保留原始 Evidence | `Evidence`、`RootCause` | active/history Incident |
| 20 | `HostManager::CalcScore` / `WriteToMysql` | `manager/src/host_manager.cpp` | 保留原健康分与主表/详情表写入语义 | `MonitorInfo`、变化率 | MySQL rows |

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
- `MonitorPusher::sender_thread_`：保存顺序发送线程
- `MonitorPusher::send_queue_`：按 item/byte 上限缓存待发送 `MonitorInfo`；诊断数据优先于普通周期数据
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
| gRPC `SetMonitorInfo` 失败 | 每次调用有 deadline；`UNAVAILABLE`、`DEADLINE_EXCEEDED`、`RESOURCE_EXHAUSTED` 按指数退避有限重试 | 非重试状态直接丢弃；队列满时可能丢弃普通数据；WAL/补传未实现 |
| manager 收到空 request | 返回 `INVALID_ARGUMENT` | 正常 |
| manager 收到空 hostname | 返回 `INVALID_ARGUMENT` 或 `OnDataReceived` 内 return | 该批数据丢弃 |
| `mysql_init` 失败 | 打印错误并 return | gRPC 仍返回 OK，worker 认为 Push 成功 |
| `mysql_real_connect` 失败 | 打印错误、关闭连接、return | 基础数据和诊断数据保留内存回退；上游仍按现有 unary 成功语义返回 |
| `mysql_query` 插入失败 | 当前基本未检查返回值 | 写库失败可能被吞掉，日志不可见 |
| `BuildAndStart()` 失败 | 检查空指针并返回失败 | 不进入服务等待循环 |

## 6. 风险分析

并发：

`GrpcServerImpl::host_data_` 和 `HostManager::host_scores_` 都有 mutex 保护，这是正向设计。但 `HostManager::OnDataReceived` 里更新的多个文件级 `static std::map` 没有锁，比如 `last_perf_samples`、`last_net_samples` 等。如果 gRPC server 并发处理多个 worker 上报，这些变化率缓存存在数据竞争风险。

生命周期：

`worker` 的 `MonitorPusher` 是 `main` 栈对象，SIGINT/SIGTERM 会打断主循环并进入析构/显式 Stop，发送线程和采集线程随后 join。`manager` 用独立 Wait 线程承载 gRPC `Wait()`，收到退出信号后先 `Shutdown()`，再 join server、停止 HostManager 和关闭 QueryManager；服务对象仍由 main 栈持有，生命周期成立。

资源：

CPU/softirq 采集中的 `open + mmap` 成功路径会 `munmap + close`，基本闭环。MySQL 写入路径每次上报都新建连接，性能和连接数压力较大。eBPF 路径会 attach TC hook，析构时 detach，但默认不删除 `clsact` qdisc，可能有轻微运行环境残留。

异常处理：

worker 参数解析、线程创建、gRPC server 创建都没有异常/失败兜底。MySQL 插入错误缺少统一检查。更关键的是 manager 写库失败不会反馈到 worker，因为 `GrpcServerImpl::SetMonitorInfo` 回调执行完仍返回 `OK`，这会让上游误以为端到端成功。

## 7. Adaptive Observability and eBPF Diagnostics

`MonitorPusher::PushOnce()` 在基础指标采集完成后执行五步控制逻辑：

1. `AnomalyDetector::Evaluate()` 从当前 `MonitorInfo` 读取 CPU、IOWait、Load、Memory、Disk、Network PPS 和 Network SoftIRQ，生成每个信号的归一化分数及整体分数。
2. `ObservabilityStateMachine::Update()` 使用连续样本确认、进入/退出阈值和恢复计数，在 `NORMAL`、`SUSPECT`、`DIAGNOSTIC`、`PROFILING`、`COOLDOWN` 之间转换。
3. `ProbeController::Apply()` 根据状态加载/卸载 TCP、Block I/O、Scheduler 三类独立 eBPF 对象；每个对象单独记录 `requested`、`available`、`attached` 和错误码，加载失败不会终止基础指标 Worker。
4. `PROFILING` 状态创建单一 `ProfileSession`，按异常信号选择 On-CPU 或 Off-CPU，并由硬超时和 RAII 清理资源。
5. `ProbeController::CollectSnapshot()` 读取已 attach 对象的有界 map，并聚合 Per-CPU 值；Worker 将异常信号和 Top-N profile ID 写入新增 `MonitorInfo.diagnostic` field 10，旧 field 1~9 语义保持不变。

Phase 4 在 `MonitorInfo.diagnostic` field 10 中追加异常信号和 Top-N profile ID。Manager 侧保留最多 256 条已结束 Incident，并通过独立 DiagnosticPersistence 写入 `diagnostic_incident`/`diagnostic_evidence`；QueryService 优先读取 MySQL，连接不可用时使用内存 IncidentStore。旧 field 1~9、旧查询和基础 MySQL 表不变。

Phase 5 在不改变 unary RPC 协议的前提下，将 Worker 发送拆为采集线程和顺序发送线程。`MonitorSendQueue` 同时限制 item 数和序列化字节数；队满时优先移除普通基础指标，诊断/Profiling 快照优先保留。`SendWithRetry` 为每次 RPC 设置 deadline，只重试 `UNAVAILABLE`、`DEADLINE_EXCEEDED` 和 `RESOURCE_EXHAUSTED`，使用带 jitter 的指数退避；Worker/Manager 均有 signal-driven shutdown。WAL、ACK 序号和断线补传仍未实现。

状态机当前采样周期为：`NORMAL` 使用配置的基础 interval，`SUSPECT` 使用 suspect interval，`DIAGNOSTIC`、`PROFILING` 和 `COOLDOWN` 使用 diagnostic interval。`MonitorPusher::WaitForNextSample()` 按当前状态等待，因此不会再固定使用启动参数作为所有状态的周期。

## 8. 面试版解释

这个项目最核心的链路是 worker 主动 push 监控数据到 manager。worker 启动时从命令行拿到 manager 地址和推送间隔，创建 `MonitorPusher`，后台线程周期执行 `PushOnce`。每次 push 会创建一个 `MonitorInfo` protobuf，由 `MetricCollector` 依次调用 CPU、内存、磁盘、网络、软中断、主机信息这些 monitor，从 `/proc`、内核模块 `/dev` 设备或 eBPF map 里采集数据并填充 protobuf。然后 worker 通过 gRPC 调用 `GrpcManager::SetMonitorInfo` 发给 manager。manager 的 `GrpcServerImpl::SetMonitorInfo` 先校验 hostname 并缓存原始数据，再通过回调进入 `HostManager::OnDataReceived`。这里会生成唯一主机名，基于 CPU、内存、负载、磁盘、网络算健康评分，维护内存里的最新主机状态，同时计算变化率并写入 MySQL 的主表和详情表。风险点主要是 MySQL 写失败不会反馈给 worker，变化率的静态 map 没有锁，以及 worker 缺少优雅退出和参数异常处理。
