# KernScope 架构说明

## 定位

KernScope 是 **eBPF 驱动的 Linux 内核性能诊断系统**。它以现有的 Worker-Agent + Manager 监控系统为基础，沿着下面的主线增量演进：

```text
低开销持续观测
       ↓
异常检测
       ↓
自适应观测
       ↓
eBPF 深度诊断 / On-CPU / Off-CPU Profiling
       ↓
多源证据关联
       ↓
可解释根因定位
```

Phase 0 只调整项目定位和架构文档。Phase 1 增加了 Worker 侧的自适应观测控制，Phase 2 增加了受限 eBPF 诊断 Probe，Phase 3 增加了限时 Profiling，Phase 4 增加了 Manager 侧 Evidence/RCA/Incident 内存闭环；尚未完成的后续能力明确标记为目标。

## 当前运行时基线

当前主链路仍为：

```text
worker main
  → MonitorPusher::PushLoop
  → MetricCollector::CollectAll
  → MonitorInter::UpdateOnce
  → MonitorInfo
  → gRPC GrpcManager::SetMonitorInfo
  → GrpcServerImpl::SetMonitorInfo
  → HostManager::OnDataReceived
  → 健康评分 / 内存状态
  → MySQL
```

Worker 当前保留以下基础能力：

- CPU Load、CPU Stat、Memory、Disk、SoftIRQ 和 HostInfo。
- `/proc/net/dev` 网络 fallback。
- TC ingress/egress + `BPF_MAP_TYPE_PERCPU_HASH` 的网络字节/报文聚合。
- `MetricCollector` 作为统一采集入口。

Manager 当前保留主机状态、健康评分、MySQL 持久化、历史查询、最优节点选择和现有 Query API。已有 `SetMonitorInfo` unary Push 链路仍不重构为 streaming。

## 目标架构边界

后续 Phase 将在现有基础上增加以下边界：

| 层次 | 目标职责 | 当前状态 |
|---|---|---|
| Base Metrics | 采集 CPU、内存、磁盘、网络、SoftIRQ | 已有并保留 |
| Adaptive Observability | 根据异常趋势调整采样周期和观测深度 | Phase 1 已实现状态机、异常评分和 Probe 规划 |
| eBPF Diagnostics | TCP、Block I/O、Scheduler 聚合诊断 | Phase 2 已实现独立对象加载、attach、map 聚合和不可用降级；Linux/BTF 功能验证待执行 |
| Profiling | On-CPU、Off-CPU、Stack、Symbolizer | Phase 3 已加入有界 Stack Map、ProfileSession、采样聚合和地址/module+offset 回退；Linux perf/BPF 功能验证待执行 |
| Evidence / RCA | Manager 侧证据构建、规则诊断、Incident | Phase 4 已实现多证据规则、可解释 confidence 和有界 IncidentStore |
| Persistence / Query | 新增诊断数据的存储与查询 | Phase 4 已追加 diagnostic field 10 和三类内存查询 RPC；MySQL 诊断表待后续阶段 |

设计约束：

1. 不修改既有 Proto field number，不破坏老 Worker/Manager 兼容性。
2. 不重写已有 TC eBPF 网络数据面；新 Probe 与其独立。
3. 诊断 Probe 或 Profiling 不可用时，基础监控继续运行。
4. 诊断数据优先在内核 Map 聚合，限制 Map、Top-N 和 Profiling 时长。
5. 现有 benchmark 脚本、原始结果和测量口径保留；未重新运行不得改写性能数字。

## Phase 路线

```text
Phase 0  项目定位与架构文档
   ↓
Phase 1  Adaptive Observability：状态机、异常检测、动态采样
   ↓
Phase 2  eBPF Diagnostics：TCP / Block I/O / Scheduler
   ↓
Phase 3  Profiling：On-CPU / Off-CPU / Symbolizer
   ↓
Phase 4  Evidence / Root Cause / Incident / Query
   ↓
Phase 5  RPC 可靠性、优雅退出、Sanitizer、Benchmark 回归
```

每个 Phase 独立完成 Design、Implementation、Unit Test、Integration Test、Build 和 Regression，再进入下一阶段。后续文档和简历只描述已经实现并验证的能力。

## 事实来源

- [当前 Worker → Manager 核心流程](core-flow-trace.md)
- [Worker Push 链路源码](../../worker/src/rpc/monitor_pusher.cpp)
- [统一采集入口](../../worker/src/monitor/metric_collector.cpp)
- [基础网络 eBPF 程序](../../worker/src/ebpf/net_stats.bpf.c)
- [MonitorInfo 协议](../../proto/monitor_info.proto)

Phase 主设计基线为本次工作区提供的 `KernScope_Optimization_Technical_Spec.md`；本仓库文档只记录与当前代码一致的边界，不复制外部规格全文。
