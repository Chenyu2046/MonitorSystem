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

1. manager 内存态：`GrpcServerImpl::host_data_` 和 `HostManager::host_scores_`；二者都按认证 hostname 建键，并在 60 秒无新数据后回收
2. MySQL 持久化：`server_performance`、`server_net_detail`、`server_softirq_detail`、`server_mem_detail`、`server_disk_detail`
3. gRPC 返回：仅当 MySQL 事务已成功提交时 `GrpcServerImpl::SetMonitorInfo` 返回 `grpc::Status::OK`，worker 侧 `PushOnce` 才返回 `true`；失败、过载和提交结果未知均返回非 OK 状态

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

- `GrpcServerImpl::host_data_`：以认证 hostname 为 key 缓存最近一次原始上报，并按 60 秒 TTL 回收
- `HostManager::host_scores_`：以认证 hostname 为 key 缓存评分和时间戳
- `HostManager` 成员 `last_perf_samples_`：保存 CPU/内存/负载/网络变化率基线
- `last_net_samples_`、`last_softirq_samples_`、`last_mem_samples_`、`last_disk_samples_`：保存各详情表变化率基线；所有主机基线共享 256 上限并按 TTL 回收
- MySQL：插入主性能表和四张详情表

## 5. 错误路径

| 错误点 | 当前处理方式 | 风险 |
|---|---|---|
| `worker/src/main.cpp` 中 interval 参数非法 | 捕获异常并打印 usage 后退出 | 不会因 `std::stoi` 异常崩溃 |
| interval 小于等于 0 或含非数字尾缀 | 拒绝并打印 usage | 配置错误不会静默改变采样周期 |
| gRPC channel 创建 | `CreateChannel` 不立即报错 | manager 地址错误会延迟到 RPC 调用才暴露 |
| `MetricCollector::CollectAll(nullptr)` | 直接 return | 静默失败，调用方不知道未采集 |
| `/dev/cpu_stat_monitor` 不存在 | `CpuStatMonitor::UpdateOnce` 直接 return | CPU stat 缺失但 worker 仍继续上报 |
| `/dev/cpu_softirq_monitor` 不存在 | `CpuSoftIrqMonitor::UpdateOnce` 静默 return | softirq 数据缺失不明显 |
| `/proc/*` 读取失败 | 多数路径返回空数据或不填字段 | manager 会用默认 0 参与评分，可能误判健康 |
| eBPF 初始化或连续读取失败 | `NetEbpfMonitor` 打日志并委托 `NetMonitor` | 网络指标降级为 `/proc/net/dev` 路径 |
| gRPC `SetMonitorInfo` 失败 | worker 打印 `status.error_message()` 并返回 false | 没有重连退避、队列缓存或数据补偿 |
| manager 收到空 request | 返回 `INVALID_ARGUMENT` | 正常 |
| manager 收到空 hostname | 返回 `INVALID_ARGUMENT` 或 `OnDataReceived` 内 return | 该批数据丢弃 |
| MySQL 初始化、连接、事务或插入失败 | 回滚并返回非 OK gRPC 状态 | Worker 记录 Push 失败，下一采样周期重试 |
| MySQL commit 结果未知 | 返回 gRPC `UNKNOWN` | 不把不确定的持久化结果伪装为成功 |
| `BuildAndStart()` 失败 | 检查空 server 并退出 | 不会空指针调用 `Wait()` |

## 6. 风险分析

并发：

`GrpcServerImpl::host_data_` 与 `HostManager::host_scores_` 都有 mutex 保护。`HostManager` 的变化率基线是实例成员，写入和 TTL 清理由 `ingest_mtx_` 串行化；并发采样最多等待 200ms 获取事务锁，超时才返回 `RESOURCE_EXHAUSTED`，避免 gRPC 工作线程和内存无界等待慢数据库。

生命周期：

Worker 捕获 `SIGINT`/`SIGTERM`，主循环调用 `MonitorPusher::Stop()` 并等待推送线程退出；在途 RPC 有 5 秒 deadline。`manager` 里 `QueryServiceImpl` 仍持有 `QueryManager*` 裸指针，当前由 `main` 的阻塞生命周期保证有效；未来改为异步关闭时需要重新审查。

资源：

CPU/softirq 采集中的 `open + mmap` 成功路径会 `munmap + close`，基本闭环。MySQL 写入复用单连接并设置连接/读/写超时；事务锁覆盖变化率基线与五表写入的一致性边界。eBPF 路径会 attach TC hook，析构时 detach，但默认不删除 `clsact` qdisc，可能有轻微运行环境残留。

异常处理：

Worker 参数解析会拒绝非法间隔；Manager 检查 gRPC server 是否成功启动。MySQL 插入逐条检查，失败回滚并映射为非 OK gRPC 状态；输入遥测的 NaN/Inf 会在 RPC 信任边界拒绝，避免污染 SQL 与评分。

## 7. 面试版解释

这个项目最核心的链路是 worker 主动 push 监控数据到 manager。Worker 每次创建 `MonitorInfo` 并经 mTLS 上报；Manager 要求请求 hostname 同时位于 allowlist 和客户端证书 SAN，随后以该认证 hostname 作为缓存、状态与持久化键。`HostManager` 串行化“变化率基线 + 有界 MySQL 事务”，成功才更新缓存；失败、过载和 commit 未知均以非 OK gRPC 状态反馈给 Worker。主要残余风险是 Linux 环境的实际 eBPF/MySQL 集成验证，而非上述链路的静态错误语义。
