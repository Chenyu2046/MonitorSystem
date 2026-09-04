# KernScope

> 面向 Linux 主机的分布式性能监控、健康评分与自适应诊断系统。

KernScope 由部署在被监控主机上的 **Worker** 和部署在管理端的 **Manager** 组成。Worker 负责采集主机与内核指标，Manager 负责接收、按主机有序处理、计算健康结果、持久化数据，并通过 gRPC 提供历史指标与诊断查询。

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus" alt="C++17" />
  <img src="https://img.shields.io/badge/Linux-eBPF-FCC624?logo=linux&logoColor=black" alt="Linux eBPF" />
  <img src="https://img.shields.io/badge/gRPC-HTTP%2F2-244c5a" alt="gRPC" />
  <img src="https://img.shields.io/badge/MySQL-8.0-4479A1?logo=mysql&logoColor=white" alt="MySQL 8.0" />
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT License" />
</p>

## 项目定位与概览

KernScope 解决的是一条完整的主机观测链路，而不是单独的指标采集器：

~~~text
被监控主机
  Worker
    ├─ CPU / 内存 / 磁盘 / 网络 / SoftIRQ / 主机信息采集
    ├─ eBPF 网络观测与按需诊断 Probe
    ├─ 异常检测与诊断状态机
    └─ MonitorInfo ── gRPC ──▶ Manager
                              ├─ 校验与按主机分片
                              ├─ 资源评分与健康评分
                              ├─ 证据汇总、根因分析、Incident
                              ├─ 异步持久化到 MySQL
                              └─ QueryService 查询历史指标与诊断结果
~~~

实现边界以当前仓库中的 C++、Proto、SQL 和构建配置为准；README 中的“已支持”只描述代码中已经存在的路径，不把规划能力当作现有能力。

## 架构入口与工作流（Workflow）

### 真实总体架构图

这张图展示 Worker、Manager、诊断、评分、持久化和查询之间的真实运行时关系。节点点击后的详情面板只展示该节点产生或消费的数据结构、字段含义和下一跳，不展示源码位置。

<p align="center">
  <a href="docs/ai/monitor-system.index.html">
    <img src="docs/ai/monitor-system.architecture.svg" alt="KernScope 真实运行时总体架构图" width="100%" />
  </a>
</p>

> GitHub README 使用上面的 SVG 作为静态预览。需要点击节点、查看消息字段和进入下一级流程时，请打开 [交互式架构入口](docs/ai/monitor-system.index.html)。


### 三条核心数据工作流

| 工作流（Workflow） | 关注的问题 | 入口 |
| --- | --- | --- |
| 采集与上传 | 指标如何采集、如何形成 MonitorInfo、如何进入发送队列并通过 gRPC 上传 | [采集与上传 Workflow](docs/ai/monitor-system.workflow.collection-upload.html) |
| 健康评分与诊断 | Manager 如何计算资源分、健康分、异常信号，以及何时形成诊断证据与 Incident | [健康评分与诊断 Workflow](docs/ai/monitor-system.workflow.health-scoring.html) |
| 持久化与查询 | 数据如何进入持久化队列、落到 MySQL，以及 QueryService 如何返回历史数据和诊断结果 | [持久化与查询 Workflow](docs/ai/monitor-system.workflow.persistence-query.html) |

如果只想快速理解项目，建议按“总体架构 → 采集与上传 → 健康评分与诊断 → 持久化与查询”的顺序阅读。工作流中的字段面板以当前 Proto 和 Manager/Worker 的真实处理逻辑为依据。

## 核心能力

| 能力 | 当前实现 | 结果边界 |
| --- | --- | --- |
| 基础主机指标 | CPU Load、CPU Stat、SoftIRQ、内存、磁盘、网络、主机信息 | 指标先在 Worker 聚合为 MonitorInfo |
| 网络观测 | eBPF TC ingress/egress；关闭 eBPF 时使用 /proc/net/dev | eBPF 网络数据通过累计计数器差分得到速率 |
| 自适应诊断 | 基于阈值/规则的异常检测、连续样本状态机、按需 Probe/Profile | 不是机器学习模型；Unavailable 不会被伪造为有效数据 |
| 健康评分 | Manager 侧资源评分与 HealthScoreEngine 滚动窗口计算 | 评分结果包含有效性、置信度、状态和主要信号 |
| 根因与事件 | EvidenceBuilder、RootCauseEngine、IncidentStore | 只有收到诊断快照时才进入证据/根因/事件链路 |
| 有序并发处理 | 按主机 hash 到固定 shard；同一主机 FIFO，不同主机并行 | 队列同时受条目数和序列化字节数限制 |
| 异步持久化 | 处理线程与 PersistenceWorker 解耦 | 接收成功、处理完成、进入持久化队列、数据库落盘是不同状态 |
| 查询服务 | 性能、趋势、异常、排名、最新评分、资源详情、Incident 查询 | 普通历史查询依赖 MySQL；Incident 查询支持内存回退 |

## 端到端数据链路

### 1. Worker：采集、诊断、上传

Worker 默认从 localhost:50051 连接 Manager，基础推送间隔为 10 秒；命令行参数可以覆盖 Manager 地址和间隔。

~~~text
各类 Monitor
  → MetricCollector::CollectAll
  → 填充 sample_sequence / sample_timestamp_ms / sample_session_id
  → AnomalyDetector::Evaluate
  → 合并 Manager 上一次返回的 MonitorFeedback
  → ObservabilityStateMachine::Update
  → ProbeController::Apply
  → CollectSnapshot / FillDiagnosticProto
  → MonitorSendQueue
  → gRPC GrpcManager::SetMonitorInfo
~~~

关键语义：

- 首次采样可能处于 NotReady，因为部分速率需要与上一采样点做差分；计数器回退也会使本次采样无效。
- Worker 的诊断状态依次为 NORMAL → SUSPECT → DIAGNOSTIC → PROFILING → COOLDOWN。状态转移受连续异常/恢复样本和时间窗口约束，不是单个瞬时值直接切换。
- MonitorSendQueue 与发送线程解耦；发送失败只对可重试的 gRPC 瞬时错误进行重试，每次尝试使用独立的 RPC deadline。
- Manager 返回 MonitorFeedback 后，Worker 可将远端健康结果与本地诊断状态合并；没有有效 sample identity 时不会生成有效反馈。

### 2. Manager：接收、评分、诊断、排队

Manager 对 SetMonitorInfo 的处理分为“快速接收”和“后台处理”两段：

~~~text
SetMonitorInfo(MonitorInfo)
  → 基础校验 / 识别 host
  → HostShardExecutor（同 host 保序）
  → ProcessOne
      ├─ CPU、网络和资源派生指标
      ├─ resource score
      ├─ HealthScoreEngine：滚动历史窗口健康评分
      ├─ 若包含 diagnostic：EvidenceBuilder
      │                         → RootCauseEngine
      │                         → IncidentStore
      └─ PersistenceTask
  → MonitorFeedback（health_valid / anomaly score / result version）
~~~

ProcessOne 完成并成功把 PersistenceTask 放入持久化队列后，Manager 可以向 Worker 返回处理反馈。此时不应把“已接收”或“已处理”理解成“已经提交 MySQL”；数据库写入由独立的持久化线程继续完成。

### 3. 持久化与查询：最终数据如何被消费

~~~text
PersistenceTask
  → PersistenceWorker
      ├─ 普通指标 → server_performance
      │             server_net_detail / server_softirq_detail
      │             server_mem_detail / server_disk_detail
      └─ 诊断事件 → diagnostic_incident
                    diagnostic_evidence
                    diagnostic_root_cause

QueryService
  → QueryManager / MySQL：历史指标、趋势、评分、资源详情
  → IncidentStore 内存回退：Incident、Incident Detail、Active Diagnosis
~~~

诊断查询只有在 QueryManager 未初始化，或诊断持久化被标记为 degraded 时，才回退到 Manager 内存中的 IncidentStore。普通历史指标、评分和资源详情查询不提供该内存回退。

## 数据契约：节点之间实际传递什么

### Worker → Manager：MonitorInfo

MonitorInfo 是一次主机采样的聚合消息，定义在 [proto/monitor_info.proto](proto/monitor_info.proto)。它包含以下几类数据：

| 字段 | 结构 | 含义 |
| --- | --- | --- |
| name | string | 主机标识 |
| host_info | message | 主机基础信息 |
| cpu_load | message | Load 指标 |
| cpu_stat | repeated message | 按 CPU 的 busy、user、system、idle、iowait、IRQ、SoftIRQ 等比例 |
| soft_irq | repeated message | 按 CPU 的 SoftIRQ 类型速率 |
| mem_info | message | 内存使用信息 |
| net_info | repeated message | 网卡收发字节/包速率及错误、丢包等信息 |
| disk_info | repeated message | 磁盘指标 |
| diagnostic | message | 当前诊断状态、信号、Probe/Profile 快照等；没有诊断快照时不进入 Manager 的证据链路 |
| sample_sequence | uint64 | Worker 侧采样序号 |
| sample_timestamp_ms | int64 | 采样时间戳 |
| sample_session_id | string | 采样会话标识，用于关联异步处理与反馈 |

### Manager → Worker：MonitorFeedback

GrpcManager::SetMonitorInfo 返回 [MonitorFeedback](proto/monitor_info.proto)：

| 字段 | 含义 |
| --- | --- |
| host_name | Manager 处理结果对应的主机 |
| health_valid | 本次健康结果是否有效 |
| node_anomaly_score | Manager 计算的节点异常分 |
| result_timestamp_ms | 结果时间戳 |
| result_version | 结果版本 |
| remote_trigger_score | 可选的远端触发分，用于 Worker 侧诊断决策 |

### Manager 内部：PersistenceTask

进入持久化队列的不是原始 RPC 指针，而是包含主机标识、MonitorInfo、资源评分、健康结果、网络/CPU/内存派生速率和可选 IncidentRecord 的任务对象。这样可以让 RPC 线程尽快返回，同时保留后续 MySQL 写入所需的数据快照。

## 自适应诊断与健康评分

### 诊断状态机

~~~text
NORMAL（正常观测）
   ↓ 连续异常样本
SUSPECT（怀疑异常，提升观测）
   ↓ 满足诊断触发条件
DIAGNOSTIC（采集诊断快照）
   ↓ 需要进一步剖析
PROFILING（有界 On-CPU / Off-CPU profiling）
   ↓ 恢复或超时
COOLDOWN（冷却，避免立即重复触发）
   ↓
NORMAL
~~~

当前 Probe 类型包括 TCP、Block I/O、Scheduler、On-CPU、Off-CPU。Probe 的“请求、可用、已 attach、快照可用”是独立状态；缺少内核能力时，系统会记录不可用状态，而不是填充看似有效的结果。Profile 会限制持续时间和样本规模，采样结果不被直接解释为精确 CPU 百分比。

### 健康评分

健康评分由 Manager 侧完成，输入包括资源派生指标、历史窗口和异常信号。当前实现使用滚动窗口、MAD 异常检测、EWMA 和 sigma 阈值等统计规则生成健康结果；输出至少区分：

- resource_score：当前资源状态的派生评分；
- health_score：结合历史窗口后的健康评分；
- anomaly_score：异常程度；
- confidence / health_valid：结果置信度与有效性；
- health_state、health_model_state、health_top_signals：状态和主要信号。

项目当前没有把健康评分描述为训练得到的 AI 模型，也没有把一次异常样本直接等同于根因结论。

## 存储模型与查询接口

初始化脚本 [manager/sql/init_server_performance.sql](manager/sql/init_server_performance.sql) 创建 monitor_db 以及以下表：

| 表 | 用途 |
| --- | --- |
| server_performance | 主机级资源分、健康分、异常分、置信度和状态 |
| server_net_detail | 网卡级网络详情 |
| server_softirq_detail | CPU/SoftIRQ 详情 |
| server_mem_detail | 内存详情 |
| server_disk_detail | 磁盘详情 |
| diagnostic_incident | 诊断事件主记录 |
| diagnostic_evidence | 事件关联的观测证据 |
| diagnostic_root_cause | 根因分析结果 |

QueryService 当前提供：

~~~text
QueryPerformance     QueryTrend          QueryAnomaly
QueryScoreRank       QueryLatestScore    QueryNetDetail
QueryDiskDetail      QueryMemDetail      QuerySoftIrqDetail
GetIncidents         GetIncidentDetail   GetActiveDiagnosis
~~~

## 代码结构

~~~text
proto/                 Protobuf / gRPC 服务与数据契约
worker/                被监控主机侧：采集、诊断、队列、推送
  src/monitor/         CPU、内存、网络、磁盘、主机信息采集器
  src/diagnostics/     异常检测、状态机、Probe、Profile、快照
  src/rpc/             Worker 发送队列与 gRPC 推送
  src/ebpf/            TC 网络统计及诊断 eBPF 程序
  src/kmod/            CPU Stat / SoftIRQ 内核模块
manager/               管理端：接收、分片、评分、持久化、查询
  src/health/          健康评分与滚动窗口
  src/diagnostics/     证据、根因、Incident 与诊断持久化
  src/rpc/             Push 与 Query gRPC 服务
  sql/                 MySQL 初始化脚本
benchmark/             gRPC 压测、运行脚本与证据归档
docs/ai/               交互式架构图和三条数据 Workflow
~~~

## 构建

### 获取代码

```bash
git clone https://github.com/yu20120707/MonitorSystem.git
cd MonitorSystem
```

### 环境要求

- Linux；CMake 3.10.2 或更高版本；支持 C++17 的 GCC 或 Clang。
- 基础依赖：Protocol Buffers、gRPC、对应的 protobuf/gRPC C++ 插件。
- 开启 eBPF 时：libbpf、libelf、libz 及对应头文件；eBPF 程序构建还会调用 clang、bpftool 和 make。仓库默认包含 worker/src/ebpf/vmlinux.h，也可以通过 Makefile 的 VMLINUX_H 覆盖；运行时还需要支持 BPF/TC 的 Linux 内核和相应权限。
- 开启 MySQL 时：通过 pkg-config 可发现的 mysqlclient。
- 编译内核模块还需要当前内核的 headers 和 kernel build tools。

### 最小构建：关闭可选的 eBPF 与 MySQL

下面的配置适合先验证 C++、Proto、gRPC 和单元测试；它不会生成 eBPF 网络实现，也不会提供 MySQL 历史持久化能力。

~~~bash
cmake -S . -B build \
  -DENABLE_EBPF=OFF \
  -DENABLE_MYSQL=OFF \
  -DBUILD_MANAGER=ON \
  -DBUILD_BENCHMARK=ON \
  -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
~~~

### 启用 MySQL

确认 mysqlclient 和 pkg-config 可用后：

~~~bash
cmake -S . -B build-mysql \
  -DENABLE_EBPF=OFF \
  -DENABLE_MYSQL=ON \
  -DBUILD_MANAGER=ON \
  -DBUILD_BENCHMARK=ON
cmake --build build-mysql -j"$(nproc)"
~~~

### 启用 eBPF 与内核模块

~~~bash
cmake -S . -B build-ebpf \
  -DENABLE_EBPF=ON \
  -DENABLE_MYSQL=OFF \
  -DBUILD_MANAGER=ON
cmake --build build-ebpf -j"$(nproc)"
cmake --build build-ebpf --target ebpf_programs
cmake --build build-ebpf --target kernel_modules
~~~

加载 CPU Stat 和 SoftIRQ 模块：

~~~bash
sudo bash worker/scripts/load_modules.sh load
sudo bash worker/scripts/load_modules.sh status
~~~

ENABLE_EBPF=ON 或 ENABLE_MYSQL=ON 在依赖缺失时会让 CMake 配置失败，而不是静默生成一个功能不完整的目标。

## 本地运行

### 启动 Manager

Manager 默认监听 0.0.0.0:50051，也可以通过第一个命令行参数覆盖：

~~~bash
./build/manager/manager 0.0.0.0:50051
~~~

启用 MySQL 的 Manager 需要先初始化数据库：

~~~bash
# 使用具备建库/建表权限的管理员账号导入 schema
mysql -h 127.0.0.1 -u root -p < manager/sql/init_server_performance.sql

# 为本机 Manager 创建应用账号；远程 Manager 请将 host 替换为受控来源地址
mysql -h 127.0.0.1 -u root -p <<'SQL'
CREATE USER IF NOT EXISTS 'monitor'@'127.0.0.1' IDENTIFIED BY 'your-password';
ALTER USER 'monitor'@'127.0.0.1' IDENTIFIED BY 'your-password';
GRANT ALL PRIVILEGES ON monitor_db.* TO 'monitor'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL

export MONITOR_MYSQL_HOST=127.0.0.1
export MONITOR_MYSQL_USER=monitor
export MONITOR_MYSQL_PASSWORD='your-password'
export MONITOR_MYSQL_DATABASE=monitor_db
./build-mysql/manager/manager 0.0.0.0:50051
~~~

### 启动 Worker

Worker 命令格式为 `<manager_address> [interval_seconds]`：

~~~bash
sudo ./build/worker/worker 192.168.1.100:50051 10
~~~

当前 Worker 启动时会无条件初始化 CPU Stat 和 SoftIRQ 采集器，因此启动前必须加载对应内核模块，并确保 /dev/cpu_stat_monitor 与 /dev/cpu_softirq_monitor 可访问。使用 eBPF 网络实现时，还需要满足目标内核、权限和 eBPF 工具链要求。

### 运行 benchmark compose 环境

仓库提供了独立的 MySQL + Manager + load generator benchmark 环境：

~~~bash
docker compose -f docker-compose.benchmark.yml up --build
~~~

该 Compose 文件使用 mysql:8.0，并将 [manager/sql/init_server_performance.sql](manager/sql/init_server_performance.sql) 作为初始化脚本挂载。它是可复现实验环境，不代表生产部署配置。

## 验证与性能证据

项目把“代码验证”和“性能观测”分开记录，避免把一次实验结果当成系统的通用容量承诺。

### 已归档的 Manager 分片链路证据

在 [10 Host Sharded Manager smoke evidence](benchmark/evidence/manager-sharded-10host-20260818/README.md) 中，实验 commit 为 7ff8d142e5699e68c51f8402adeb17cd00414d94，运行环境为 mysql:8.0 linux/amd64、Docker Engine 29.6.2、Docker Compose v5.3.1；10 个 Host、30 秒、每 Host 1 秒一个样本的运行记录显示：

- accepted = processed = persistence_tasks = persisted = 300；
- queue_full = 0，persistence_rejected = 0；
- accepted 延迟 P50/P95/P99 为 1451 / 2171 / 7096 us；
- 该证据还包含 benchmark 镜像信息、完整 CSV 和 manager 统计文件；该 commit 早于当前 HEAD，不能自动视为当前 HEAD 的新一轮压测结果。

这里的 persisted = 300 是该次实验在等待 drain 完成后的最终检查结果；在一般运行语义中，Manager 返回接收/处理反馈并不等价于数据库事务已经完成。

### eBPF 观测证据的阅读边界

历史 eBPF benchmark 和测量方法见 [benchmark/MEASUREMENT_METHODOLOGY_2026-08-13.md](benchmark/MEASUREMENT_METHODOLOGY_2026-08-13.md)。当前归档的 [Global Hash vs Per-CPU Hash A/B 报告](benchmark/evidence/linux-ebpf-map-update-ab-20260825/FINAL_REPORT.md) 明确记录了 Ubuntu 24.04.4、Linux 6.8.0-138-generic、4 vCPU、VirtualBox NAT、enp0s3、180 个有效窗口及其限制；对应的 [可复用指标摘要](benchmark/evidence/linux-ebpf-map-update-ab-20260825/RESUME_METRICS.md) 未将结果表述为稳定的生产性能收益。部分历史吞吐数据来自 WSL2 veth 等隔离环境，只能说明对应实验配置下的观测结果，不能直接外推为物理网卡或生产集群容量。

## 配置与运行边界

当前代码中可直接影响运行行为的常用配置包括：

| 配置 | 作用 |
| --- | --- |
| MONITOR_MYSQL_HOST | MySQL 地址 |
| MONITOR_MYSQL_USER | MySQL 用户 |
| MONITOR_MYSQL_PASSWORD | MySQL 密码 |
| MONITOR_MYSQL_DATABASE | MySQL 数据库名 |
| MONITOR_VERBOSE_METRICS | Manager/Worker 的详细指标日志开关 |
| KERNSCOPE_DIAGNOSTIC_LOG | 诊断日志开关 |
| KERNSCOPE_EBPF_OBJECT_DIR | eBPF 对象文件目录覆盖 |
| MONITOR_PERF_LOG / MONITOR_PERF_TRACE | 性能日志与 trace 开关 |
| KERNSCOPE_MANAGER_SHARDS | Manager 主机分片数量 |
| KERNSCOPE_SHARD_QUEUE_CAPACITY / KERNSCOPE_SHARD_QUEUE_MAX_BYTES | 主机分片队列的条目数/字节上限 |
| KERNSCOPE_PERSIST_QUEUE_CAPACITY / KERNSCOPE_PERSIST_QUEUE_MAX_BYTES | 持久化队列的条目数/字节上限 |
| KERNSCOPE_NORMAL_INTERVAL_MS / KERNSCOPE_SUSPECT_INTERVAL_MS / KERNSCOPE_DIAGNOSTIC_INTERVAL_MS | Worker 不同观测状态下的采样间隔 |
| KERNSCOPE_PROFILING_DURATION_SEC / KERNSCOPE_PROFILE_SAMPLE_HZ / KERNSCOPE_COOLDOWN_SEC | profiling 时长、采样频率和冷却时间 |
| KERNSCOPE_MYSQL_CONNECT_TIMEOUT_SEC / KERNSCOPE_MYSQL_READ_TIMEOUT_SEC / KERNSCOPE_MYSQL_WRITE_TIMEOUT_SEC | MySQL 连接、读、写超时 |

需要特别注意：

1. Manager 当前通过 gRPC insecure credentials 启动，README 不把它描述为已具备 TLS、认证和授权能力；生产环境需要在部署层补齐网络访问控制与安全传输方案。
2. eBPF、内核模块、MySQL 均是 Linux/环境相关能力；关闭对应编译开关可以完成部分本地验证，但不能代表完整生产链路已经可用。
3. 普通历史指标和评分查询依赖 MySQL；只有 Incident 类查询在特定降级条件下使用内存状态。

## 相关文档

- [交互式总体架构入口](docs/ai/monitor-system.index.html)
- [端到端核心链路说明](docs/ai/core-flow-trace.md)
- [MySQL 数据层说明](docs/ai/mysql-data-layer.md)
- [仓库上手与代码证据](docs/ai/repo-onboarding.md)
- [项目定位与产品说明](PRODUCT.md)
- [Benchmark 测量方法](benchmark/MEASUREMENT_METHODOLOGY_2026-08-13.md)

## Roadmap

以下方向仍属于规划或待完善范围，不作为当前能力承诺：

- [ ] Worker 本地 WAL 与断点续传
- [ ] Manager 高可用与多实例协调
- [ ] 完整的生产级身份认证、TLS 和权限模型
- [ ] 更丰富的诊断 Probe 与可观测性运营界面

## License

[MIT License](LICENSE)
