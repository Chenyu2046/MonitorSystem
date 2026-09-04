(function () {
  'use strict';

  // This is a presentation-only layer. It reads the stable node ids emitted by
  // Archify and replaces source-oriented node detail with source-grounded data
  // shape descriptions. It never renders source paths or code locations.
  var field = function (name, type, meaning) {
    return { name: name, type: type, meaning: meaning };
  };
  var model = function (kind, summary, fields, input, output, nested) {
    return {
      kind: kind,
      summary: summary,
      fields: fields || [],
      input: input || '',
      output: output || '',
      nested: nested || []
    };
  };

  var DATA = {
    'linux-runtime': model('主机数据源', '提供 Worker 读取的 Linux 原始计数、快照和速率输入。', [
      field('procfs', '路径集合', '/proc/loadavg、/proc/meminfo、/proc/net/dev、/proc/diskstats'),
      field('mmap device', '共享页', '逐 CPU 累计 CPU 计数'),
      field('TC eBPF map', 'per-CPU map', '网卡收发字节和包计数，用户态聚合 CPU 槽位')
    ], 'Linux 内核状态与累计计数', 'CPU/load、内存、网络、磁盘、SoftIRQ 采样输入'),
    'worker-agent': model('进程数据', 'Worker 周期性构造并发送一轮 MonitorInfo。', [
      field('sample', 'MonitorInfo', '基础资源字段加可选 DiagnosticSnapshot'),
      field('identity', 'host_info + sample_*', '主机身份、采样序号、采样时间、会话标识')
    ], 'MetricCollector 和诊断控制结果', 'MonitorSendQueue 中的 MonitorInfo'),
    'metric-collector': model('采集聚合', '按 monitors_ 顺序调用各类监控器，并把 host、resource 字段和采集状态写入同一个 MonitorInfo 消息。', [
      field('monitors_', '监控器列表', 'CPU/load、cpu_stat、softirq、mem、net、disk、host'),
      field('host_info', 'HostInfo', 'hostname 和 ip_address'),
      field('resource fields', 'nested messages', 'CPU/load、memory、network、disk、SoftIRQ'),
      field('collect status', 'CollectStatus', '本轮采集是否成功；失败状态决定是否继续上报')
    ], 'procfs、mmap 设备、TC eBPF', 'MonitorInfo 基础字段'),
    'diag-control': model('诊断控制数据', '在基础样本上计算异常并按状态决定是否采集诊断快照。', [
      field('overall anomaly', 'double', '各类信号归一化后的整体异常分数'),
      field('state', 'ObservabilityState', 'NORMAL、SUSPECT、DIAGNOSTIC、PROFILING、COOLDOWN'),
      field('snapshot', 'DiagnosticSnapshot', '信号、Probe 状态和 profiling 栈')
    ], 'MonitorInfo 基础指标', '写回 MonitorInfo.diagnostic'),
    'send-queue': model('有界消息队列', '在采集线程和发送线程之间传递 MonitorInfo，并实施数量和字节背压。', [
      field('item', 'MonitorInfo', '队列中的一轮采样消息'),
      field('capacity', 'items + bytes', '同时限制元素数量和序列化字节数'),
      field('priority', 'diagnostic / ordinary', '队满时优先保留诊断样本')
    ], 'MonitorInfo', 'Pop 后交给发送线程'),
    'manager-rpc': model('gRPC 消息边界', '现有 unary RPC 的请求、反馈和接收状态边界。', [
      field('request', 'MonitorInfo', 'Worker 上传的一轮采样'),
      field('response', 'MonitorFeedback', 'Manager 生成的健康反馈，可为空或部分有效'),
      field('status', 'gRPC Status', 'OK、INVALID_ARGUMENT、RESOURCE_EXHAUSTED、UNAVAILABLE')
    ], 'Worker MonitorInfo', 'Manager 接收结果和 MonitorFeedback'),
    'host-manager': model('主机处理结果', '按 host 串行处理样本，同时生成评分、诊断和持久化任务。', [
      field('resource score', 'double', '兼容资源余量分数，通常映射到 0~100'),
      field('health result', 'HealthResult', '健康分数、异常分数、域分数、Top Signals'),
      field('persistence task', 'PersistenceTask', '快照、变化率和可选 Incident')
    ], 'Manager 接收的 MonitorInfo', '缓存、MonitorFeedback、PersistenceTask'),
    'persistence-queue': model('持久化任务队列', '把评分完成后的 PersistenceTask 与数据库写入解耦。', [
      field('task', 'PersistenceTask', 'HostScore、变化率、可选 IncidentRecord'),
      field('backpressure', 'items + bytes', '按任务估算大小限制队列占用')
    ], 'HostManager 计算结果', 'PersistenceWorker 串行消费'),
    'diag-state': model('诊断状态数据', '维护主机 active incident、关闭历史和可追溯证据。', [
      field('incident', 'IncidentRecord', 'id、host、severity、state、active、时间范围'),
      field('root_causes', 'RootCause[]', '根因类型、置信度、evidence ids、摘要'),
      field('evidence', 'Evidence[]', '来源、目标、值、单位、严重度、时间、细节')
    ], 'HealthResult 和诊断证据', '内存查询或诊断持久化'),
    'mysql': model('持久化数据', '保存普通性能、逐实体详情和诊断 incident 及其证据。', [
      field('server_performance', '主表', '主机概览、变化率、resource/health score、Top Signals'),
      field('server_*_detail', '详情表', '网卡、磁盘、内存、SoftIRQ 的逐实体历史'),
      field('diagnostic_*', '诊断表', 'incident、root cause、evidence 及其时间和关联 id')
    ], 'PersistenceTask', 'SQL 行和查询结果'),
    'query-client': model('查询数据', '请求历史、评分、详情或 incident 视图的外部调用方。', [
      field('request', 'Query*Request', 'server、时间范围、分页、排序或阈值'),
      field('response', 'Query*Response', 'records、summary、detail 或 incidents 加分页统计')
    ], '查询条件', 'QueryService gRPC response'),
    'query-service': model('查询响应', '把 QueryService 请求转换为统一 protobuf 响应；内存 fallback 仅用于 incident 相关读取。', [
      field('history', 'PerformanceRecord[]', '主机性能和变化率时间序列'),
      field('detail', 'Net/Disk/Mem/SoftIrqDetailRecord[]', '逐网卡、磁盘、内存分类、SoftIRQ 记录'),
      field('diagnosis', 'Incident[]', '事件、根因和 evidence；可从 QueryManager 或 IncidentStore 读取')
    ], 'Query*Request', 'Query*Response'),

    'linux-proc': model('procfs 输入', '来自 Linux procfs 的主机级或累计指标输入。', [
      field('/proc/loadavg', '1/5/15 load', 'load_avg_1、load_avg_3（历史名，实际 5 分钟）、load_avg_15'),
      field('/proc/meminfo', '分类内存', 'total、free、avail、buffers、cached 等 GiB 字段'),
      field('/proc/net/dev', '网卡累计值', '字节、包、错误、丢弃累计计数'),
      field('/proc/diskstats', '磁盘累计值', '读写请求、扇区、耗时和 util 所需计数')
    ], 'Linux procfs', '对应监控器的采样输入'),
    'linux-kernel': model('内核输入', '提供逐 CPU CPU/SoftIRQ 数据和 TC eBPF 网络计数。', [
      field('cpu_stat', 'per-CPU counter', 'mmap 共享页中的 CPU 累计计数'),
      field('softirq', 'per-CPU rate', '各 SoftIRQ 类别按相邻计数差分为 events/s'),
      field('net_stats_map', 'BPF_MAP_TYPE_PERCPU_HASH', '按 CPU 槽位读取并聚合的收发字节/包计数')
    ], 'Linux kernel / TC hooks', 'Worker 监控器可消费的内核数据'),
    'worker-main': model('生命周期数据', 'Worker 启动参数、信号控制和退出状态。', [
      field('argv', 'command arguments', '进程启动参数'),
      field('signal', 'process signal', '触发停止或生命周期变化'),
      field('threads', 'thread state', '采集/发送线程的启动和停止')
    ], '进程启动环境', 'MonitorPusher 生命周期'),
    'pusher-start': model('发送器状态', '初始化监控器、打开输入并启动采集和发送线程。', [
      field('collector state', 'initialized/opened', '监控器和内核输入是否可用'),
      field('thread state', '2 worker threads', 'PushLoop 与 SendLoop 的运行状态')
    ], 'Worker 生命周期', '周期采样线程和发送线程'),
    'push-loop': model('采样调度', '按周期调用 PushOnce，并根据配置调整下一轮间隔。', [
      field('interval', 'duration', '采样周期或 adaptive interval'),
      field('sample', 'one MonitorInfo', '本轮采集和诊断装配后的消息')
    ], '调度时钟', 'PushOnce 产生的一轮 MonitorInfo'),
    'worker-metric-readers': model('监控器集合', 'MetricCollector 持有的各类 reader 和 monitor。', [
      field('CPU/load', 'CpuStat[] + CpuLoad', '逐核 CPU 分解和主机负载'),
      field('memory', 'MemInfo', '主机内存分类和 used_percent'),
      field('network', 'NetInfo[]', '逐网卡吞吐、包速率、错误和丢弃'),
      field('disk/SoftIRQ', 'DiskInfo[] + SoftIrq[]', '逐磁盘派生 IO 和逐核 events/s')
    ], 'procfs、mmap、eBPF', '写入同一个 MonitorInfo'),
    'monitor-info': model('protobuf 消息', 'Worker 一轮采样的完整数据载体；诊断快照作为可选字段复用同一消息上报。', [
      field('name', 'string · field 1', '兼容旧版本的主机名'),
      field('host_info', 'HostInfo · field 2', '主机身份：hostname、ip_address'),
      field('soft_irq', 'repeated SoftIrq · field 4', '逐 CPU SoftIRQ 事件速率，单位 events/s'),
      field('cpu_load', 'CpuLoad · field 5', '主机级 1/5/15 分钟负载'),
      field('cpu_stat', 'repeated CpuStat · field 6', '逐 CPU 使用率和 user/system/idle/IOWait 等分解，单位 percent'),
      field('mem_info', 'MemInfo · field 7', '主机内存分类快照，容量 GiB，used_percent 为 percent'),
      field('net_info', 'repeated NetInfo · field 8', '逐网卡速率、包速率以及错误/丢弃累计值'),
      field('disk_info', 'repeated DiskInfo · field 9', '逐磁盘累计 IO 计数和吞吐/IOPS/延迟/util 派生值'),
      field('diagnostic', 'DiagnosticSnapshot · field 10', '可选异常、状态、Probe 和 profiling 快照'),
      field('sample_sequence', 'uint64 · field 20', 'Worker 会话内单调递增的采样序号'),
      field('sample_timestamp_ms', 'int64 · field 21', 'Unix epoch milliseconds 的采样时间'),
      field('sample_session_id', 'string · field 22', 'Worker 生命周期内稳定的采样会话 ID')
    ], 'Linux/内核采样和诊断结果', 'AnomalyDetector、DiagnosticSnapshot、MonitorSendQueue、SetMonitorInfo', [
      { name: 'HostInfo', shape: 'hostname: string; ip_address: string' },
      { name: 'CpuLoad', shape: 'load_avg_1/3/15: float (field 1/2/3，1/5/15 分钟负载); sample_valid: bool (field 10)' },
      { name: 'CpuStat[]', shape: 'cpu_name: string (1); cpu_percent/usr/system/nice/idle/io_wait/irq/soft_irq_percent: float (2-9，percent); sample_valid: bool (20)' },
      { name: 'SoftIrq[]', shape: 'cpu: string (1); hi/timer/net_tx/net_rx/block/irq_poll/tasklet/sched/hrtimer/rcu: float (2-11，events/s); sample_valid: bool (20)' },
      { name: 'MemInfo', shape: 'total/free/avail/buffers/cached/swap_cached/active/inactive/active_anon/inactive_anon/active_file/inactive_file/dirty/writeback/anon_pages/mapped/kReclaimable/sReclaimable/sUnreclaim: float (1-19，GiB); used_percent: float (31，percent); sample_valid: bool (40)' },
      { name: 'NetInfo[]', shape: 'name: string (1); send_rate/rcv_rate: float (2-3，KiB/s); send_packets_rate/rcv_packets_rate: float (4-5，packets/s); err_in/err_out/drop_in/drop_out: uint64 (6-9，累计值); sample_valid: bool (20)' },
      { name: 'DiskInfo[]', shape: 'name: string (1); reads/writes/sectors_read/sectors_written/read_time_ms/write_time_ms/io_in_progress/io_time_ms/weighted_io_time_ms: uint64 (2-10，累计计数); read_bytes_per_sec/write_bytes_per_sec/read_iops/write_iops/avg_read_latency_ms/avg_write_latency_ms/util_percent: double (20-26，派生值); sample_valid: bool (30)' },
      { name: 'DiagnosticSnapshot', shape: 'state: ObservabilityState (1); anomaly_score: double (2); signals: DiagnosticSignal[] (3); oncpu_profiles/offcpu_profiles: ProfileEntry[] (4-5); probe_status: ProbeRuntimeStatus[] (6)' },
      { name: 'DiagnosticSignal[]', shape: 'domain: DiagnosticDomain; metric: string; value: double; unit: string; anomaly_score: double; target: string' },
      { name: 'ProfileEntry[]', shape: 'pid/tid: int32; samples/total_offcpu_ns: uint64; user_stack/kernel_stack: StackFrame[]; user_stack_id/kernel_stack_id: int32' },
      { name: 'ProbeRuntimeStatus[]', shape: 'probe: string; requested/available/attached/snapshot_ok: bool; last_error/snapshot_error: int32' }
    ]),
    'anomaly-detector': model('Worker 异常评估', 'Worker AnomalyDetector 只读取本轮 MonitorInfo，生成信号列表、整体分数和诊断策略建议；不维护跨轮状态。', [
      field('overall_score', 'double 0..1', '所有信号 score 取最大值，避免严重单点被平均稀释'),
      field('signals', 'AnomalySignal[]', '每项包含 domain、metric、原单位 value、0..1 score、triggered'),
      field('should_diagnose', 'bool', '是否建议进入诊断策略'),
      field('should_profile', 'bool', '是否建议进入 profiling 策略')
    ], 'MonitorInfo', 'AnomalyResult → 状态机和 ProbeController'),
    'state-machine': model('状态数据', '用连续异常/恢复样本和时间窗口维护观测状态。', [
      field('state', 'ObservabilityState', 'NORMAL、SUSPECT、DIAGNOSTIC、PROFILING、COOLDOWN'),
      field('sample counters', 'consecutive counts', '异常确认和恢复确认所需的连续样本'),
      field('profile/cooldown', 'time window', 'profiling 和冷却阶段的时序控制')
    ], 'overall_score 和样本时序', 'state + profile decision'),
    'probe-controller': model('Probe 数据', '按诊断状态申请、附着和采集 Probe，并允许失败降级。', [
      field('requested', 'bool', '本轮是否请求该 Probe'),
      field('available/attached', 'bool + bool', '能力是否可用、当前是否已附着'),
      field('snapshot', 'profile entries', 'OnCPU/OffCPU 栈样本或空结果'),
      field('errors', 'last_error/snapshot_error', '附着或快照失败码')
    ], 'state + profile decision', 'ProbeRuntimeStatus 和 ProfileEntry'),
    'diagnostic-proto': model('诊断消息', '写入 MonitorInfo.diagnostic 的 DiagnosticSnapshot 结构。', [
      field('state', 'ObservabilityState', '当前 Worker 观测状态'),
      field('anomaly_score', 'double', '本轮整体异常分数'),
      field('signals', 'DiagnosticSignal[]', 'domain、metric、value、unit、anomaly_score、target'),
      field('oncpu_profiles', 'ProfileEntry[]', 'pid/tid、samples、user/kernel stacks'),
      field('offcpu_profiles', 'ProfileEntry[]', 'pid/tid、total_offcpu_ns、user/kernel stacks'),
      field('probe_status', 'ProbeRuntimeStatus[]', 'requested、available、attached、错误码、snapshot_ok')
    ], 'AnomalyDetector + state + ProbeController', 'MonitorInfo.diagnostic'),
    'send-loop': model('发送线程数据', '从有界队列取出 MonitorInfo，记录排队等待后进入 RPC 调用。', [
      field('queue item', 'MonitorInfo', '待发送的一轮采样'),
      field('queue wait', 'steady duration', '从入队到 Pop 的等待时长'),
      field('ordering', 'sender sequence', '按队列顺序交给发送逻辑')
    ], 'MonitorSendQueue.Pop', 'SendWithRetry 的 RPC 输入'),
    'send-retry': model('发送结果', '在 deadline 内发送 unary RPC，并只对指定可重试状态退避重试。', [
      field('deadline', 'gRPC deadline', '本次 RPC 的时间边界'),
      field('retry codes', 'StatusCode', 'UNAVAILABLE、DEADLINE_EXCEEDED、RESOURCE_EXHAUSTED'),
      field('backoff', 'duration + jitter', '指数退避和随机抖动'),
      field('result', 'Status + MonitorFeedback', '成功反馈或最终失败状态')
    ], 'MonitorInfo', 'GrpcManager.SetMonitorInfo'),
    'grpc-contract': model('RPC 消息', 'Worker 客户端调用的 GrpcManager.SetMonitorInfo 合约。', [
      field('request', 'MonitorInfo', '完整采样消息'),
      field('response', 'MonitorFeedback', 'host_name、health_valid、node_anomaly_score、时间、版本、remote_trigger_score')
    ], 'MonitorInfo', 'MonitorFeedback 或 gRPC Status'),
    'manager-handler': model('接收状态', '校验主机标识、调用 Manager 回调，并把入队结果映射为 gRPC 状态。', [
      field('request', 'MonitorInfo', '必须包含可解析的主机标识'),
      field('callback result', 'DataReceiveResult', 'accepted、queue full、stopping、invalid host'),
      field('status', 'gRPC Status', 'OK 或对应失败码')
    ], 'SetMonitorInfo request', 'HostManager callback result + status'),
    'manager-submit': model('处理追踪', '把带回调的样本提交给 HostShardExecutor，并在 deadline 内等待反馈。', [
      field('completion', 'promise<HostFeedbackResult>', '将处理结果关联回同一个采样'),
      field('host_name', 'string', 'CanonicalHostKey 结果'),
      field('feedback', 'HostFeedbackResult', 'host、node/remote score、时间、版本、valid')
    ], 'host_name + MonitorInfo + deadline', 'accepted/rejected 和可选 HostFeedbackResult'),

    'rpc-handler': model('RPC 接收', 'Manager 的 gRPC handler 入口，负责校验并提交异步处理。', [
      field('request', 'MonitorInfo', 'Worker 上传消息'),
      field('response', 'MonitorFeedback', '处理反馈的可选结果'),
      field('status', 'DataReceiveResult → Status', '入队成功、队满、停止或主机非法')
    ], 'Worker unary RPC', 'HostManager 提交结果'),
    'host-key': model('路由键', '从请求中得到用于主机亲和性和状态索引的规范主机名。', [
      field('input', 'MonitorInfo', '优先 host_info.hostname，兼容旧 name'),
      field('host_name', 'string', '非空规范主机键')
    ], 'MonitorInfo', 'ShardFor、缓存和 PersistenceTask 的 host_name'),
    'submit-tracked': model('追踪提交', '把 MonitorInfo 和完成通知放入按 host 路由的执行器。', [
      field('WorkItem', 'host + info + timestamps', '消息、接收时间、入队时间、completion'),
      field('return', 'DataReceiveResult', '是否进入有界 shard queue')
    ], 'host_name + MonitorInfo', 'Shard queue 或拒绝结果'),
    'shard-select': model('分片键', '使用 host_name 的稳定哈希选择 shard。', [
      field('host_name', 'string', '路由输入'),
      field('shard_id', 'size_t', 'hash(host_name) % shard_count')
    ], 'Canonical host_name', '一个固定 shard'),
    'shard-queue': model('Shard WorkItem', '每个 shard 的有界 FIFO 队列元素。', [
      field('host_name', 'string', '主机亲和性键'),
      field('info', 'MonitorInfo', '待处理采样消息'),
      field('received_at', 'system_clock', '事件接收时间'),
      field('enqueued_at', 'steady_clock', '排队计时起点'),
      field('completion', 'promise pointer', '可选的反馈完成通知')
    ], 'SubmitTracked', 'RunShard FIFO 消费'),
    'run-shard': model('串行处理上下文', '一个 shard 由一个消费者按 FIFO 调用 ProcessOne，保证同一 host 的历史顺序。', [
      field('queue wait', 'microseconds', 'enqueued_at 到取出的排队时间'),
      field('process input', 'MonitorInfo + timestamps', '当前样本和两个时间边界'),
      field('process result', 'HostFeedbackResult', '通过 completion 回传')
    ], 'Shard WorkItem', 'HostManager::ProcessOne'),
    'validate-monitor': model('校验结果', '确认 MonitorInfo 的主机标识和基本数据可进入计算路径。', [
      field('hostname', 'non-empty string', '主机键必须可解析'),
      field('sample fields', 'validated MonitorInfo', '计算使用的字段需通过基本有效性检查'),
      field('valid', 'bool', '决定后续评分与持久化是否有效')
    ], 'MonitorInfo', 'valid/invalid processing branch'),
    'resource-overview': model('资源概览', '从逐核和逐网卡消息构造 Manager 侧主机概览。', [
      field('CpuOverview', 'average + peak', '有效 CPU 核平均值、cpu_count、peak_core_name、peak_cpu_percent'),
      field('NetworkOverview', 'sum + peak', '总收发速率、峰值收发速率、包速率、网卡数量'),
      field('memory/load/disk', 'host scalar', 'used_percent、load_avg_1、最大 disk util')
    ], 'MonitorInfo.cpu_stat/net_info/mem_info/cpu_load/disk_info', '评分输入和 PersistenceTask 概览'),
    'resource-score': model('资源评分', '兼容旧语义的资源余量分数，不等同于 health_score。', [
      field('weights', 'CPU .35 / mem .30 / load .15 / disk .15 / net .05', '固定资源评分权重'),
      field('components', '0..1', '1 - CPU/100、1 - mem/100、load、disk、收发带宽余量'),
      field('score', 'double × 100', '资源余量评分；valid 表示结果有限有效')
    ], 'CpuOverview + load + MemInfo + NetworkOverview + DiskInfo', 'ScoreResult{score, valid}'),
    'perf-sample': model('变化率状态', '保存下一轮变化率计算所需的上一轮主机级基础值。', [
      field('base values', 'PerfSample', 'CPU 分解、load、内存、网络和 score'),
      field('rate', 'float', '(current - previous) / previous；previous 为 0 时返回 0'),
      field('ownership', 'per-host shard state', '同一 host 在同一 shard 中顺序更新')
    ], '当前 MonitorInfo 概览 + 上一轮 PerfSample', 'PersistenceTask 的 *_rate 字段'),
    'health-engine': model('健康评分输入输出', '结合指标历史、静态阈值、EWMA、MAD 和投票生成 HealthResult。', [
      field('observation', 'metric value + event time', '当前 CPU、内存、磁盘、网络、调度指标'),
      field('detector result', 'DetectorResult', '阈值/MAD/EWMA 分数、投票、异常标记、模型状态'),
      field('health result', 'HealthResult', '域分数、anomaly_score、health_score、confidence、Top Signals')
    ], 'MonitorInfo + resource_score + historical windows', 'HealthResult'),
    'rolling-windows': model('历史窗口', '按指标维护有界的时间序列，用于 EWMA、MAD 和模型状态。', [
      field('metrics', 'MetricId[]', 'CPU、IOWait、SoftIRQ、load、memory、disk、network 等'),
      field('samples', 'value + timestamp', '每个指标的历史观测'),
      field('state', 'cold/warming/ready', '由样本数和历史时长决定检测能力')
    ], '当前指标观测', 'MetricDetector 的 history'),
    'metric-detector': model('单指标检测结果', '对一个指标融合静态阈值、EWMA、MAD 并用异常投票确认。', [
      field('threshold_available', 'bool', '是否存在静态阈值'),
      field('threshold_score', 'double', '静态阈值归一化分数'),
      field('mad_score / ewma_score', 'double', '历史偏差分数'),
      field('anomaly_votes', 'int', '参与确认的异常投票数'),
      field('anomaly_score', 'double 0..1', '单指标异常分数'),
      field('anomalous', 'bool', '是否达到共识票数'),
      field('model_state', 'ModelState', 'cold、warming、ready')
    ], 'metric value + RollingWindow', 'DetectorResult'),
    'domain-aggregate': model('域聚合结果', '把单指标检测结果按 CPU、Memory、Disk、Network、Scheduler 聚合。', [
      field('domain score', 'double', '每个域内有效指标的异常分数'),
      field('valid weight', 'double', '只对有效域参与归一化'),
      field('anomaly_score', 'double', 'Σ(domain_score × weight) / Σ(valid_weight)'),
      field('top_signals', 'TopSignal[]', '按异常贡献保留的关键指标')
    ], 'DetectorResult[]', 'HealthResult domain fields'),
    'health-result': model('健康结果', 'Manager 内部健康评分和诊断状态的统一结果对象。', [
      field('health_score', 'double', '100 × (1 - anomaly_score)'),
      field('resource_score', 'double', '兼容资源余量评分'),
      field('anomaly_score', 'double', '整体异常分数'),
      field('remote_trigger_score', 'double', '远程触发信号'),
      field('anomaly_rate_5m', 'double', '5 分钟异常率'),
      field('cpu/memory/disk/network/scheduler_score', 'double', '五个域分数'),
      field('confidence', 'double', '模型可信度'),
      field('state/model_state', 'string + ModelState', 'Worker 状态和模型冷启动状态'),
      field('top_signals', 'TopSignal[]', '指标值和 DetectorResult'),
      field('valid', 'bool', '本轮健康结果是否有效')
    ], 'HealthScoreEngine', 'feedback、host cache、PersistenceTask、diagnostic evidence'),
    'feedback': model('反馈消息', '把同一采样对应的 Manager 评分摘要回传给 Worker。', [
      field('host_name', 'string', '反馈对应的主机'),
      field('health_valid', 'bool', '健康计算是否有效'),
      field('node_anomaly_score', 'double', '节点异常分数'),
      field('remote_trigger_score', 'double optional', '远程触发分数'),
      field('result_timestamp_ms', 'int64', '结果时间'),
      field('result_version', 'uint64', '结果版本')
    ], 'HostFeedbackResult', 'MonitorFeedback'),
    'host-cache': model('主机最新状态', '按 host 保存最近一次 HostScore，供实时查询和后续变化率计算。', [
      field('info', 'MonitorInfo', '最近样本原始消息'),
      field('score', 'double', '资源余量 score 兼容值'),
      field('health', 'HealthResult', '最近健康结果'),
      field('timestamp', 'system_clock', '事件时间'),
      field('score_valid', 'bool', '资源分数有效性')
    ], 'ProcessOne', '最新评分查询或内存 fallback'),
    'evidence-builder': model('证据数据', '把健康结果和原始监控字段转换为可追溯诊断 Evidence。', [
      field('Evidence.id', 'string', '证据唯一标识'),
      field('source/target', 'string', '原始指标边界和受影响对象'),
      field('value/unit', 'double + string', '证据值和单位'),
      field('severity/timestamp/detail', 'double + time + text', '严重度、事件时间和解释细节')
    ], 'MonitorInfo + HealthResult', 'Evidence[]'),
    'root-cause': model('根因结果', '按规则组合 Evidence，得到一个或多个根因候选。', [
      field('type', 'string', '根因类别'),
      field('confidence', 'double', '规则置信度'),
      field('evidence_ids', 'string[]', '关联的证据 id'),
      field('summary', 'string', '根因摘要')
    ], 'Evidence[]', 'RootCause[]'),
    'incident-store': model('Incident 状态', '为每个 host 维护 active episode，并保存关闭历史。', [
      field('id/server_name', 'uint64 + string', 'incident id 和主机'),
      field('severity/state/active', 'string + bool', '严重度、状态、是否仍 active'),
      field('start/end_time', 'time point', 'episode 时间范围'),
      field('root_causes/evidence', 'vectors', '根因和可追溯证据快照')
    ], 'state + Evidence[] + RootCause[]', 'IncidentRecord 或空值'),
    'persistence-task': model('持久化任务', '把一轮主机快照、变化率、健康结果和可选 incident 送入异步写入；字段对应 C++ struct，不是新的 protobuf。', [
      field('host_name', 'string', '主机键'),
      field('host_score', 'HostScore', 'info: MonitorInfo；score: double；score_valid: bool；timestamp: system_clock；health: HealthResult'),
      field('net_in_rate/net_out_rate', 'double', '当前主机收发速率概览'),
      field('CPU rate group', 'float × 11', 'cpu_percent_rate、usr_percent_rate、system_percent_rate、nice_percent_rate、idle_percent_rate、io_wait_percent_rate、irq_percent_rate、soft_irq_percent_rate、steal_percent_rate、guest_percent_rate、guest_nice_percent_rate；相对上一轮变化率'),
      field('load/memory rate group', 'float × 7', 'load_avg_1_rate、load_avg_3_rate、load_avg_15_rate、mem_used_percent_rate、mem_total_rate、mem_free_rate、mem_avail_rate；相对上一轮变化率'),
      field('network rate group', 'float × 4', 'net_in_rate_rate、net_out_rate_rate、net_in_drop_rate_rate、net_out_drop_rate_rate；相对上一轮变化率'),
      field('incident', 'optional IncidentRecord', '诊断状态变化或需要关闭 episode 时携带；否则为空')
    ], 'ProcessOne', 'PersistenceWorker'),

    'persistence-enqueue': model('持久化入队', '只在 PersistenceWorker 已启动且有界队列可接收时入队。', [
      field('task', 'PersistenceTask', '包含普通快照和可选诊断事件'),
      field('accepted', 'bool', '队列是否接收任务')
    ], 'PersistenceTask', 'PersistenceWorker queue'),
    'persistence-run': model('持久化消费', '单消费者取出任务，记录排队时间并调用任务处理器。', [
      field('queued task', 'PersistenceTask', '待写入数据'),
      field('queue wait', 'microseconds', '入队到消费的等待时长'),
      field('handler', 'task callback', '普通性能写入和诊断写入')
    ], 'PersistenceWorker queue', 'HostManager::PersistTask'),
    'persist-task': model('写入编排', '按任务内容写普通性能/详情数据，并在有 incident 时写诊断链。', [
      field('ordinary snapshot', 'HostScore + *_rate', 'server_performance 及逐实体 detail 所需字段'),
      field('incident', 'optional IncidentRecord', 'diagnostic_incident、root cause、evidence'),
      field('write result', 'bool/status', '数据库写入是否完成')
    ], 'PersistenceTask', 'MySQL rows or degraded result'),
    'diagnostic-save': model('诊断持久化对象', '在 MySQL 已启用且已初始化、root_causes 非空时，把一个 IncidentRecord 的主表、根因和 Evidence 放入同一保存路径。', [
      field('incident', 'IncidentRecord', '主机、严重度、状态、时间、根因、证据'),
      field('diagnostic_incident', 'incident row', '主事件及最高置信度根因摘要'),
      field('diagnostic_root_cause/evidence', 'detail rows', '关联 incident_id 的完整诊断明细'),
      field('preconditions', 'initialized && connection && root_causes not empty', '不满足时 Save 返回 false，不产生诊断写入')
    ], 'IncidentRecord', '事务中的诊断表数据或失败结果'),
    'transaction': model('事务边界', '诊断主表、根因和 Evidence 的原子写入边界；任何 SQL/关联写入失败都回滚。', [
      field('precondition', 'bool', 'MySQL 已初始化、连接有效且 root_causes 非空'),
      field('begin', 'START TRANSACTION', '开始一次诊断写入'),
      field('writes', 'incident + causes + evidence', '主记录及其关联明细'),
      field('outcome', 'COMMIT / ROLLBACK', '全部成功提交，否则回滚')
    ], 'IncidentRecord', '原子数据库结果'),
    'mysql-main': model('主表数据', 'server_performance 保存主机概览、变化率和评分结果。', [
      field('identity/time', 'server_name + timestamp', '主机和采样时间'),
      field('CPU/load/memory/disk/network', 'scalar columns', '主机概览及单位对应的指标'),
      field('score', 'resource score alias', '兼容资源余量分数'),
      field('health fields', 'health/resource/anomaly/confidence/state', '自适应健康评分和 Top Signals'),
      field('rate fields', '*_rate', 'CPU/load/memory/disk/network 变化率')
    ], 'PersistenceTask.host_score + rates', 'server_performance row'),
    'mysql-detail': model('逐实体详情数据', '保存网卡、磁盘、内存分类和 SoftIRQ 的历史明细。', [
      field('network detail', 'server_net_detail', '网卡名、速率、包速率、错误/丢弃及变化率'),
      field('disk detail', 'server_disk_detail', '磁盘累计计数、吞吐、IOPS、延迟、util 及变化率'),
      field('memory detail', 'server_mem_detail', '内存分类容量及变化率'),
      field('SoftIRQ detail', 'server_softirq_detail', 'CPU 名、events/s 和变化率')
    ], 'MonitorInfo repeated fields + Manager-derived rates', 'detail table rows'),
    'mysql-diagnostic': model('诊断持久化数据', '保存可查询的 incident、root cause 和 evidence 关联链。', [
      field('diagnostic_incident', 'incident row', 'incident_key、host、root_cause、severity、confidence、state、summary、时间'),
      field('diagnostic_root_cause', 'cause row', 'incident_id、ordinal、root_cause、confidence、evidence_ids、summary'),
      field('diagnostic_evidence', 'evidence row', '类型、source、target、metric、value、unit、severity、detail、event_time')
    ], 'DiagnosticPersistence.Save', 'diagnostic_* rows'),
    'commit': model('事务结果', '返回诊断写入的提交或回滚结果。', [
      field('commit', 'COMMIT', '主表和所有关联明细均成功'),
      field('rollback', 'ROLLBACK', '任一 SQL/关联写入失败'),
      field('saved', 'bool', 'Save 的最终结果')
    ], 'diagnostic write operations', '持久化成功或失败'),
    'query-contract': model('查询 RPC 合约', 'QueryService 对外暴露的历史、评分、详情和 incident 查询消息。', [
      field('performance/trend', 'QueryPerformance/QueryTrend', '时间范围、分页、聚合间隔'),
      field('anomaly/score', 'QueryAnomaly/QueryScoreRank/Latest', '阈值、排序和评分类型'),
      field('detail/incident', 'QueryDetail + Incident requests', '逐实体详情和诊断筛选条件'),
      field('response', 'Query*Response', 'records、servers、cluster_stats、incidents、分页')
    ], 'Query*Request', 'Query*Response'),
    'query-handler': model('查询处理', '校验查询请求，调用 QueryManager；只有 incident 相关读取在需要时使用 HostManager/IncidentStore 内存状态，再填充 protobuf。', [
      field('time range', 'Timestamp pair', '起止时间并检查 start <= end'),
      field('pagination', 'page/page_size', 'page 从 1 开始，缺省 page_size 为 100'),
      field('response mapping', 'protobuf conversion', 'C++ record → repeated response records')
    ], 'QueryService RPC request', 'QueryService response'),
    'time-pagination': model('查询控制数据', '决定历史查询的时间范围、排序粒度和返回页。', [
      field('TimeRange', 'Timestamp start/end', '查询时间窗口'),
      field('Pagination', 'int32 page/page_size', '页码和每页大小'),
      field('interval_seconds', 'int32', '趋势聚合间隔，0 表示不聚合')
    ], 'Query request', 'QueryManager SQL 条件和分页 response'),
    'query-manager': model('查询结果集', '执行历史 SQL 或读取查询结果，转换为内部记录。', [
      field('PerformanceRecord', '主表记录', '主机概览、评分、健康字段和变化率'),
      field('detail records', 'detail structs', '逐网卡、磁盘、内存、SoftIRQ 记录'),
      field('incident records', 'IncidentRecord', '诊断主表及关联根因/Evidence')
    ], 'validated query conditions', 'C++ records'),
    'performance-query': model('性能查询数据', '返回主表历史快照或按时间桶聚合后的趋势记录。', [
      field('records', 'repeated PerformanceRecord', 'server_name、Timestamp、CPU/load/memory/disk/network'),
      field('scores', 'health/resource/anomaly', '评分和健康模型状态'),
      field('rates', '*_rate', '相对上一轮变化率'),
      field('page metadata', 'total_count/page/page_size', '分页统计')
    ], 'QueryPerformance/QueryTrend request', 'QueryPerformanceResponse or QueryTrendResponse'),
    'score-query': model('评分查询数据', '返回每台主机最新或排序后的评分摘要和关键指标。', [
      field('ServerScoreSummary', 'repeated', 'server_name、score、last_update、status、CPU/mem/disk/load'),
      field('health fields', 'health_score/resource_score/anomaly_score', '健康评分、资源评分、异常分数、置信度、状态'),
      field('ClusterStats', 'aggregate', '总数、在线/离线、均值、最大/最小值和最佳/最差主机')
    ], 'QueryScoreRank/QueryLatestScore request', 'servers + cluster_stats'),
    'detail-query': model('详情查询数据', '返回按网卡、磁盘、内存分类或 SoftIRQ 维度的历史记录。', [
      field('NetDetailRecord[]', 'network', '网卡累计错误/丢弃、速率、包速率和变化率'),
      field('DiskDetailRecord[]', 'disk', '吞吐、IOPS、读写延迟、util 和变化率'),
      field('MemDetailRecord[]', 'memory', '分类容量和变化率'),
      field('SoftIrqDetailRecord[]', 'SoftIRQ', 'CPU 维度 events/s 和变化率'),
      field('page metadata', 'total_count/page/page_size', '分页统计')
    ], 'QueryDetailRequest', '对应 Query*DetailResponse'),
    'main-select': model('主表查询行', '从 server_performance 取出主表值、健康字段和变化率列。', [
      field('identity', 'server_name/timestamp', '主机和时间'),
      field('base metrics', 'CPU/load/memory/disk/network', '普通监控字段'),
      field('health metrics', 'health/resource/anomaly/confidence/state/top signals', '自适应评分字段'),
      field('rate metrics', '*_rate', '变化率字段')
    ], 'time range + server_name + pagination', 'PerformanceRecord internal row'),
    'record-map': model('性能响应记录', '把内部 PerformanceRecord 映射为 protobuf 的一条记录。', [
      field('identity', 'server_name + Timestamp', '主机和时间'),
      field('snapshot', 'CPU/load/memory/disk/network', '主表普通指标'),
      field('score', 'resource/health/anomaly', '评分、状态和 Top Signals'),
      field('rates', '*_rate', '变化率字段')
    ], 'QueryManager PerformanceRecord', 'QueryPerformanceResponse.records'),
    'incident-switch': model('诊断读取分支', '根据持久化状态选择 MySQL incident 查询或 HostManager 内存状态。', [
      field('persistence state', 'initialized/degraded', '数据库诊断能力状态'),
      field('fallback', 'IncidentStore', 'MySQL 不可用时的 active/history 内存结果'),
      field('result', 'IncidentRecord[]', '统一交给 protobuf mapping')
    ], 'GetIncidents/GetActiveDiagnosis request', '持久化或内存 incident records'),
    'mysql-incidents': model('历史诊断结果', '查询 diagnostic_incident 及关联 root cause/evidence，形成 IncidentRecord。', [
      field('filters', 'host/time/root_cause/severity', 'incident 查询筛选'),
      field('incident rows', 'diagnostic_incident', '事件主信息'),
      field('relations', 'root cause + evidence', '按 incident_id 关联的完整诊断链')
    ], 'GetIncidents request', 'IncidentRecord[]'),
    'memory-incidents': model('实时诊断结果', '直接读取 HostManager 的 active IncidentStore，不依赖 MySQL。', [
      field('active', 'IncidentRecord[]', '当前 active episode'),
      field('history', 'IncidentRecord[]', '内存保留的关闭历史'),
      field('filters', 'server/time/root cause/severity', '与查询条件匹配')
    ], 'incident query request', 'IncidentRecord[]'),
    'incident-map': model('Incident protobuf', '把内部 IncidentRecord 转为对外 Incident 消息。', [
      field('id/server_name', 'uint64 + string', '事件 id 和主机'),
      field('severity/state/active', 'string + bool', '严重度、状态和 active 标识'),
      field('start/end_time_ms', 'int64', '事件时间范围'),
      field('root_causes', 'RootCause[]', '根因与 evidence_ids'),
      field('evidence', 'Evidence[]', '诊断证据明细')
    ], 'IncidentRecord', 'GetIncidentsResponse / detail response')
  };

  var style = document.createElement('style');
  style.textContent = [
    '#focus-passport-meta, #focus-evidence, #focus-detail { display: none !important; }',
    'svg [data-detail="fine"], svg .source-evidence-beacon { display: none !important; }',
    '.monitor-node-data { margin-top: .62rem; max-width: 70rem; color: var(--text); }',
    '.monitor-node-data-head { display: flex; align-items: baseline; gap: .5rem; flex-wrap: wrap; margin-bottom: .28rem; }',
    '.monitor-node-data-label { color: var(--messagebus-stroke); font-size: .54rem; font-weight: 800; letter-spacing: .12em; text-transform: uppercase; }',
    '.monitor-node-data-kind { color: var(--text-faint); font-size: .55rem; }',
    '.monitor-node-data-summary { display: block; margin-bottom: .5rem; color: var(--text-muted); font-size: .67rem; line-height: 1.45; }',
    '.monitor-node-data-grid { display: grid; grid-template-columns: minmax(7rem, .55fr) minmax(8rem, .7fr) minmax(13rem, 1.7fr); border: 1px solid color-mix(in srgb, var(--toolbar-border) 80%, transparent); border-radius: .48rem; overflow: hidden; }',
    '.monitor-node-data-cell { min-width: 0; padding: .34rem .45rem; border-bottom: 1px solid color-mix(in srgb, var(--toolbar-border) 62%, transparent); background: color-mix(in srgb, var(--panel) 74%, transparent); font-size: .61rem; line-height: 1.4; overflow-wrap: anywhere; }',
    '.monitor-node-data-cell:nth-child(6n + 4), .monitor-node-data-cell:nth-child(6n + 5), .monitor-node-data-cell:nth-child(6n + 6) { background: color-mix(in srgb, var(--backend-fill) 12%, var(--panel)); }',
    '.monitor-node-data-cell.is-head { color: var(--text-faint); font-size: .52rem; font-weight: 800; letter-spacing: .06em; text-transform: uppercase; }',
    '.monitor-node-data-cell.is-name { color: var(--cloud-stroke); font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }',
    '.monitor-node-data-cell.is-type { color: var(--frontend-stroke); }',
    '.monitor-node-data-subtitle { margin: .56rem 0 .28rem; color: var(--text-faint); font-size: .57rem; font-weight: 800; letter-spacing: .08em; text-transform: uppercase; }',
    '.monitor-node-data-nested { display: grid; gap: .28rem; grid-template-columns: repeat(auto-fit, minmax(13rem, 1fr)); }',
    '.monitor-node-data-nested-item { padding: .38rem .45rem; border: 1px solid color-mix(in srgb, var(--toolbar-border) 70%, transparent); border-radius: .42rem; background: color-mix(in srgb, var(--panel) 78%, transparent); }',
    '.monitor-node-data-nested-name { display: block; color: var(--messagebus-stroke); font-size: .6rem; font-weight: 800; }',
    '.monitor-node-data-nested-shape { display: block; margin-top: .18rem; color: var(--text-muted); font-size: .59rem; line-height: 1.4; overflow-wrap: anywhere; }',
    '.monitor-node-data-flow { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: .38rem; margin-top: .56rem; }',
    '.monitor-node-data-flow-item { min-width: 0; padding: .38rem .45rem; border-left: 2px solid var(--messagebus-stroke); background: color-mix(in srgb, var(--panel) 78%, transparent); }',
    '.monitor-node-data-flow-label { display: block; color: var(--text-faint); font-size: .52rem; font-weight: 800; letter-spacing: .08em; text-transform: uppercase; }',
    '.monitor-node-data-flow-value { display: block; margin-top: .14rem; color: var(--text-muted); font-size: .61rem; line-height: 1.4; overflow-wrap: anywhere; }',
    '@media (max-width: 760px) { .monitor-node-data-grid { grid-template-columns: minmax(6.5rem, .65fr) minmax(8rem, .9fr) minmax(10rem, 1.5fr); } }',
    '@media (max-width: 560px) { .monitor-node-data-grid { grid-template-columns: 1fr; } .monitor-node-data-cell.is-head { display: none; } .monitor-node-data-cell { border-bottom: 0; } .monitor-node-data-flow { grid-template-columns: 1fr; } }'
  ].join('\n');
  document.head.appendChild(style);

  var copyText = function (value) {
    var element = document.createElement('span');
    element.textContent = value == null ? '' : String(value);
    return element;
  };
  var addCell = function (grid, value, className) {
    var cell = document.createElement('div');
    cell.className = 'monitor-node-data-cell' + (className ? ' ' + className : '');
    cell.appendChild(copyText(value));
    grid.appendChild(cell);
  };
  var addSectionTitle = function (parent, value) {
    var title = document.createElement('div');
    title.className = 'monitor-node-data-subtitle';
    title.appendChild(copyText(value));
    parent.appendChild(title);
  };
  var addFlow = function (parent, input, output) {
    if (!input && !output) return;
    var flow = document.createElement('div');
    flow.className = 'monitor-node-data-flow';
    [['输入', input], ['输出', output]].forEach(function (item) {
      var block = document.createElement('div');
      block.className = 'monitor-node-data-flow-item';
      var label = document.createElement('span');
      label.className = 'monitor-node-data-flow-label';
      label.appendChild(copyText(item[0]));
      var value = document.createElement('span');
      value.className = 'monitor-node-data-flow-value';
      value.appendChild(copyText(item[1] || '—'));
      block.appendChild(label);
      block.appendChild(value);
      flow.appendChild(block);
    });
    parent.appendChild(flow);
  };

  var nodeById = function (id) {
    var nodes = document.querySelectorAll('[data-node-id]');
    for (var index = 0; index < nodes.length; index += 1) {
      if (nodes[index].getAttribute('data-node-id') === id) return nodes[index];
    }
    return null;
  };
  var dataPanel = null;
  var lastId = '';
  var render = function () {
    var idElement = document.getElementById('focus-id');
    var chip = document.getElementById('focus-chip');
    if (!idElement || !chip || chip.hidden) return;
    var id = (idElement.textContent || '').trim();
    if (!id || id === lastId) return;
    lastId = id;
    var node = nodeById(id);
    var record = DATA[id] || model(
      '节点数据',
      node ? (node.getAttribute('data-node-sublabel') || '此节点的图上语义说明。') : '此节点暂无数据说明。',
      [],
      '',
      ''
    );
    if (!dataPanel) {
      dataPanel = document.createElement('section');
      dataPanel.className = 'monitor-node-data';
      dataPanel.setAttribute('aria-label', '节点相关数据');
      var copy = document.querySelector('.relationship-lens-copy');
      if (copy) copy.appendChild(dataPanel);
    }
    dataPanel.textContent = '';
    var head = document.createElement('div');
    head.className = 'monitor-node-data-head';
    var label = document.createElement('span');
    label.className = 'monitor-node-data-label';
    label.appendChild(copyText('节点数据'));
    var kind = document.createElement('span');
    kind.className = 'monitor-node-data-kind';
    kind.appendChild(copyText(record.kind));
    head.appendChild(label);
    head.appendChild(kind);
    dataPanel.appendChild(head);
    var summary = document.createElement('span');
    summary.className = 'monitor-node-data-summary';
    summary.appendChild(copyText(record.summary));
    dataPanel.appendChild(summary);

    if (record.fields.length) {
      addSectionTitle(dataPanel, '字段与数据形状');
      var grid = document.createElement('div');
      grid.className = 'monitor-node-data-grid';
      addCell(grid, '字段', 'is-head');
      addCell(grid, '类型 / 结构', 'is-head');
      addCell(grid, '含义 / 单位', 'is-head');
      record.fields.forEach(function (item) {
        addCell(grid, item.name, 'is-name');
        addCell(grid, item.type, 'is-type');
        addCell(grid, item.meaning, '');
      });
      dataPanel.appendChild(grid);
    }
    if (record.nested.length) {
      addSectionTitle(dataPanel, '嵌套与重复项');
      var nested = document.createElement('div');
      nested.className = 'monitor-node-data-nested';
      record.nested.forEach(function (item) {
        var block = document.createElement('div');
        block.className = 'monitor-node-data-nested-item';
        var name = document.createElement('span');
        name.className = 'monitor-node-data-nested-name';
        name.appendChild(copyText(item.name));
        var shape = document.createElement('span');
        shape.className = 'monitor-node-data-nested-shape';
        shape.appendChild(copyText(item.shape));
        block.appendChild(name);
        block.appendChild(shape);
        nested.appendChild(block);
      });
      dataPanel.appendChild(nested);
    }
    addFlow(dataPanel, record.input, record.output);
  };

  var cleanSourcePresentation = function () {
    var eyebrow = document.querySelector('.relationship-lens-eyebrow');
    if (eyebrow) eyebrow.textContent = '节点数据';
    var nodes = document.querySelectorAll('[data-node-id]');
    Array.prototype.forEach.call(nodes, function (node) {
      var original = node.getAttribute('data-source-evidence-original-label');
      if (original) node.setAttribute('aria-label', original);
      var title = node.querySelector('title');
      if (title) {
        var label = node.getAttribute('data-node-label') || '';
        var sublabel = node.getAttribute('data-node-sublabel') || '';
        var nextTitle = label + (sublabel ? ' · ' + sublabel : '');
        if (title.textContent !== nextTitle) title.textContent = nextTitle;
      }
    });
    Array.prototype.forEach.call(document.querySelectorAll('.node-finder-result'), function (result) {
      var id = result.getAttribute('data-node-id');
      var node = nodeById(id);
      var small = result.querySelector('small');
      if (!node || !small) return;
      var record = DATA[id];
      var kind = record ? record.kind : (node.getAttribute('data-node-kind') || 'node');
      var summary = [kind, id, record ? record.summary : '节点数据'].filter(Boolean).join(' · ');
      if (small.textContent !== summary) small.textContent = summary;
      if (small.title !== summary) small.title = summary;
    });
  };

  var install = function () {
    cleanSourcePresentation();
    var focusId = document.getElementById('focus-id');
    var chip = document.getElementById('focus-chip');
    if (!focusId || !chip) return;
    var observer = new MutationObserver(function () {
      cleanSourcePresentation();
      render();
    });
    observer.observe(focusId, { childList: true, characterData: true, subtree: true });
    observer.observe(chip, { attributes: true, attributeFilter: ['hidden'] });
    window.setInterval(cleanSourcePresentation, 500);
    document.addEventListener('click', function () { window.setTimeout(render, 0); }, true);
    document.addEventListener('keydown', function () { window.setTimeout(render, 0); }, true);
    render();
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', install);
  else install();
})();
