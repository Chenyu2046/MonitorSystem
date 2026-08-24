/**
 * @file monitor_pusher.cpp
 * @brief Worker 采集控制循环、诊断转换和 gRPC 重试实现。
 *
 * 每轮流程固定为：采集基础指标 -> 计算异常分数 -> 推进状态机 ->
 * 按状态 attach/detach Probe -> 读取诊断快照 -> 转换 DiagnosticSnapshot
 * protobuf -> 放入有界发送队列。发送线程随后执行 protobuf RPC，失败时
 * 仅对 UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED 做有限退避重试。
 */

#include "rpc/monitor_pusher.h"

#include "canonical_host_key.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace {

/**
 * @brief 读取一个必须为正整数的环境变量，非法值保留为无效配置。
 *
 * 配置解析发生在 Worker 启动阶段；不会在每轮采样读取环境变量。
 */
bool ApplyPositiveEnv(const char* name, int* target) {
  const char* raw = std::getenv(name);
  if (!raw) {
    return true;
  }

  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(raw, &end, 10);
  if (*raw == '\0' || end == raw || *end != '\0' || errno == ERANGE ||
      parsed <= 0 || parsed > INT_MAX) {
    std::cerr << "Invalid " << name << "='" << raw << std::endl;
    *target = -1;
    return false;
  }
  *target = static_cast<int>(parsed);
  return true;
}

/** @brief 判断是否开启高噪声的基础指标日志。 */
bool IsMetricsLogEnabled() {
  const char* value = std::getenv("MONITOR_VERBOSE_METRICS");
  return value && std::string(value) == "1";
}

/** @brief 判断是否开启诊断状态/Probe 日志。 */
bool IsDiagnosticLogEnabled() {
  const char* value = std::getenv("KERNSCOPE_DIAGNOSTIC_LOG");
  return value && std::string(value) == "1";
}

std::string GenerateSampleSessionId() {
  std::random_device device;
  std::mt19937_64 generator(
      static_cast<std::mt19937_64::result_type>(device()) ^
      static_cast<std::mt19937_64::result_type>(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::ostringstream output;
  output << std::hex << generator() << generator();
  return output.str();
}

/**
 * @brief 将构造参数和环境变量合并为运行时可观测性配置。
 *
 * interval_seconds 只覆盖 NORMAL 基础间隔；诊断间隔、profiling 上限、
 * cooldown、采样频率和 eBPF object 目录由环境变量按合法性校验覆盖。
 */
monitor::diagnostics::ObservabilityConfig MakeObservabilityConfig(
    int interval_seconds) {
  monitor::diagnostics::ObservabilityConfig config;
  if (interval_seconds > 0 && interval_seconds <= INT_MAX / 1000) {
    config.normal_interval_ms = interval_seconds * 1000;
  } else {
    config.normal_interval_ms = -1;
  }
  ApplyPositiveEnv("KERNSCOPE_NORMAL_INTERVAL_MS", &config.normal_interval_ms);
  ApplyPositiveEnv("KERNSCOPE_SUSPECT_INTERVAL_MS", &config.suspect_interval_ms);
  ApplyPositiveEnv("KERNSCOPE_DIAGNOSTIC_INTERVAL_MS",
                   &config.diagnostic_interval_ms);
  ApplyPositiveEnv("KERNSCOPE_PROFILING_DURATION_SEC",
                   &config.profiling_duration_sec);
  ApplyPositiveEnv("KERNSCOPE_COOLDOWN_SEC", &config.cooldown_sec);
  ApplyPositiveEnv("KERNSCOPE_PROFILE_SAMPLE_HZ", &config.profiling_sample_hz);
  if (const char* object_dir = std::getenv("KERNSCOPE_EBPF_OBJECT_DIR")) {
    if (*object_dir != '\0') {
      config.ebpf_object_dir = object_dir;
    } else {
      config.ebpf_object_dir.clear();
    }
  }
  return config;
}

/**
 * @brief 根据异常信号选择 OnCPU 或 OffCPU profiling。
 *
 * 磁盘/IOWait 触发更适合观察等待时间，因此选择 OffCPU；其他异常默认
 * 选择 OnCPU。这里只选择策略类型，不启动 profiling。
 */
monitor::diagnostics::ProfileType SelectProfileType(
    const monitor::diagnostics::AnomalyResult& anomaly) {
  for (const auto& signal : anomaly.signals) {
    if (signal.triggered &&
        (signal.domain == monitor::diagnostics::AnomalyDomain::kDisk ||
         signal.metric == "io_wait_percent")) {
      return monitor::diagnostics::ProfileType::kOffCpu;
    }
  }
  return monitor::diagnostics::ProfileType::kOnCpu;
}

/** @brief 把内部状态机枚举映射为跨 gRPC 的 Protobuf 枚举。 */
monitor::proto::ObservabilityState ToProtoState(
    monitor::diagnostics::ObservabilityState state) {
  switch (state) {
    case monitor::diagnostics::ObservabilityState::kNormal:
      return monitor::proto::OBSERVABILITY_NORMAL;
    case monitor::diagnostics::ObservabilityState::kSuspect:
      return monitor::proto::OBSERVABILITY_SUSPECT;
    case monitor::diagnostics::ObservabilityState::kDiagnostic:
      return monitor::proto::OBSERVABILITY_DIAGNOSTIC;
    case monitor::diagnostics::ObservabilityState::kProfiling:
      return monitor::proto::OBSERVABILITY_PROFILING;
    case monitor::diagnostics::ObservabilityState::kCooldown:
      return monitor::proto::OBSERVABILITY_COOLDOWN;
  }
  return monitor::proto::OBSERVABILITY_NORMAL;
}

/** @brief 为诊断日志提供稳定的状态名称。 */
const char* StateName(monitor::diagnostics::ObservabilityState state) {
  switch (state) {
    case monitor::diagnostics::ObservabilityState::kNormal:
      return "NORMAL";
    case monitor::diagnostics::ObservabilityState::kSuspect:
      return "SUSPECT";
    case monitor::diagnostics::ObservabilityState::kDiagnostic:
      return "DIAGNOSTIC";
    case monitor::diagnostics::ObservabilityState::kProfiling:
      return "PROFILING";
    case monitor::diagnostics::ObservabilityState::kCooldown:
      return "COOLDOWN";
  }
  return "UNKNOWN";
}

/** @brief 把内部异常域映射为 Diagnostic protobuf 域枚举。 */
monitor::proto::DiagnosticDomain ToProtoDomain(
    monitor::diagnostics::AnomalyDomain domain) {
  switch (domain) {
    case monitor::diagnostics::AnomalyDomain::kCpu:
      return monitor::proto::DOMAIN_CPU;
    case monitor::diagnostics::AnomalyDomain::kMemory:
      return monitor::proto::DOMAIN_MEMORY;
    case monitor::diagnostics::AnomalyDomain::kDisk:
      return monitor::proto::DOMAIN_DISK;
    case monitor::diagnostics::AnomalyDomain::kNetwork:
      return monitor::proto::DOMAIN_NETWORK;
    case monitor::diagnostics::AnomalyDomain::kScheduler:
      return monitor::proto::DOMAIN_SCHEDULER;
  }
  return monitor::proto::DOMAIN_UNKNOWN;
}

/** @brief 为 ProbeStatus protobuf 和日志提供 Probe 名称。 */
const char* ProbeName(monitor::diagnostics::ProbeKind kind) {
  switch (kind) {
    case monitor::diagnostics::ProbeKind::kTcp:
      return "TCP";
    case monitor::diagnostics::ProbeKind::kBlockIo:
      return "BLOCK_IO";
    case monitor::diagnostics::ProbeKind::kScheduler:
      return "SCHEDULER";
    case monitor::diagnostics::ProbeKind::kOnCpuProfile:
      return "ONCPU";
    case monitor::diagnostics::ProbeKind::kOffCpuProfile:
      return "OFFCPU";
  }
  return "UNKNOWN";
}

/**
 * @brief 将 C++ 诊断快照和状态转换成 MonitorInfo.diagnostic。
 *
 * 基础 anomaly signals、Probe 状态、TCP/block/scheduler evidence 和
 * top-N profiling 样本都在此完成 Protobuf 边界转换；profiling 按样本数
 * 或 OffCPU 总时长排序，但不把排序结果解释成精确 CPU 百分比。
 */
void FillDiagnosticProto(
    const monitor::diagnostics::AnomalyResult& anomaly,
    monitor::diagnostics::ObservabilityState state,
    const monitor::diagnostics::DiagnosticSnapshot& snapshot,
    const monitor::diagnostics::ProbeController& probe_controller,
    const monitor::diagnostics::Symbolizer& symbolizer,
    monitor::proto::DiagnosticSnapshot* diagnostic) {
  diagnostic->Clear();
  diagnostic->set_state(ToProtoState(state));
  diagnostic->set_anomaly_score(anomaly.overall_score);

  for (const auto& signal : anomaly.signals) {
    auto* proto_signal = diagnostic->add_signals();
    proto_signal->set_domain(ToProtoDomain(signal.domain));
    proto_signal->set_metric(signal.metric);
    proto_signal->set_value(signal.value);
    proto_signal->set_anomaly_score(signal.score);
  }

  for (const auto kind : {monitor::diagnostics::ProbeKind::kTcp,
                          monitor::diagnostics::ProbeKind::kBlockIo,
                          monitor::diagnostics::ProbeKind::kScheduler,
                          monitor::diagnostics::ProbeKind::kOnCpuProfile,
                          monitor::diagnostics::ProbeKind::kOffCpuProfile}) {
    const auto& status = probe_controller.Status(kind);
    auto* proto_status = diagnostic->add_probe_status();
    proto_status->set_probe(ProbeName(kind));
    proto_status->set_requested(status.requested);
    proto_status->set_available(status.available);
    proto_status->set_attached(status.attached);
    proto_status->set_last_error(status.last_error);
    proto_status->set_snapshot_ok(status.snapshot_ok);
    proto_status->set_snapshot_error(status.snapshot_error);
  }

  for (const auto& sample : snapshot.tcp) {
    auto* signal = diagnostic->add_signals();
    signal->set_domain(monitor::proto::DOMAIN_NETWORK);
    signal->set_metric("tcp_retransmissions");
    signal->set_value(static_cast<double>(sample.retransmissions));
    signal->set_unit("count");
    signal->set_target("pid:" + std::to_string(sample.pid) +
                       "/tgid:" + std::to_string(sample.tgid));
  }
  // Block I/O 的平均延迟只有在 count>0 时才有意义，单位从 ns 转为 ms。
  for (const auto& sample : snapshot.block_io) {
    if (sample.count == 0) {
      continue;
    }
    auto* signal = diagnostic->add_signals();
    signal->set_domain(monitor::proto::DOMAIN_DISK);
    signal->set_metric("block_io_avg_latency_ms");
    signal->set_value(static_cast<double>(sample.total_latency_ns) /
                      static_cast<double>(sample.count) / 1000000.0);
    signal->set_unit("ms");
    signal->set_target("host");
  }
  for (const auto& sample : snapshot.scheduler) {
    auto* switches = diagnostic->add_signals();
    switches->set_domain(monitor::proto::DOMAIN_SCHEDULER);
    switches->set_metric("scheduler_switches");
    switches->set_value(static_cast<double>(sample.switches));
    switches->set_unit("count");
    switches->set_target("pid:" + std::to_string(sample.pid));

    auto* wakeups = diagnostic->add_signals();
    wakeups->set_domain(monitor::proto::DOMAIN_SCHEDULER);
    wakeups->set_metric("scheduler_wakeups");
    wakeups->set_value(static_cast<double>(sample.wakeups));
    wakeups->set_unit("count");
    wakeups->set_target("pid:" + std::to_string(sample.pid));
  }

  // 复制后排序只影响本轮 protobuf 的 top-N 顺序，不改变 ProbeController
  // 持有的快照，也不把 samples 当作精确的进程 CPU 利用率。
  std::vector<monitor::diagnostics::OnCpuProfileSample> on_cpu =
      snapshot.profiling.on_cpu;
  std::sort(on_cpu.begin(), on_cpu.end(),
            [](const auto& left, const auto& right) {
              return left.samples > right.samples;
            });
  constexpr std::size_t kTopProfileCount = 20;
  for (std::size_t index = 0; index < std::min(kTopProfileCount, on_cpu.size());
       ++index) {
    const auto& sample = on_cpu[index];
    auto* entry = diagnostic->add_oncpu_profiles();
    entry->set_pid(static_cast<std::int32_t>(sample.tgid));
    entry->set_tid(static_cast<std::int32_t>(sample.pid));
    entry->set_samples(sample.samples);
    entry->set_user_stack_id(sample.user_stack_id);
    entry->set_kernel_stack_id(sample.kernel_stack_id);
    for (const auto address : sample.user_stack) {
      auto* frame = entry->add_user_stack();
      frame->set_address(address);
      frame->set_symbol(symbolizer.SymbolizeUser(
          static_cast<int>(sample.tgid), address));
    }
    for (const auto address : sample.kernel_stack) {
      auto* frame = entry->add_kernel_stack();
      frame->set_address(address);
      frame->set_symbol(symbolizer.SymbolizeKernel(address));
    }
  }

  std::vector<monitor::diagnostics::OffCpuProfileSample> off_cpu =
      snapshot.profiling.off_cpu;
  std::sort(off_cpu.begin(), off_cpu.end(),
            [](const auto& left, const auto& right) {
              return left.total_duration_ns > right.total_duration_ns;
            });
  for (std::size_t index = 0;
       index < std::min(kTopProfileCount, off_cpu.size()); ++index) {
    const auto& sample = off_cpu[index];
    auto* entry = diagnostic->add_offcpu_profiles();
    entry->set_pid(static_cast<std::int32_t>(sample.pid));
    entry->set_samples(sample.samples);
    entry->set_total_offcpu_ns(sample.total_duration_ns);
    entry->set_kernel_stack_id(sample.kernel_stack_id);
    for (const auto address : sample.kernel_stack) {
      auto* frame = entry->add_kernel_stack();
      frame->set_address(address);
      frame->set_symbol(symbolizer.SymbolizeKernel(address));
    }
  }
}

}  // namespace

namespace monitor {

MonitorPusher::MonitorPusher(const std::string& manager_address,
                             int interval_seconds)
    : manager_address_(manager_address),
      interval_seconds_(interval_seconds),
      running_(false),
      observability_config_(MakeObservabilityConfig(interval_seconds)),
      remote_health_feedback_(std::chrono::milliseconds(
          std::max<std::int64_t>(
              5000, static_cast<std::int64_t>(
                        observability_config_.normal_interval_ms) *
                        3))),
      anomaly_detector_(observability_config_),
      state_machine_(observability_config_),
      probe_controller_(observability_config_.ebpf_object_dir,
                        observability_config_.profiling_sample_hz,
                        observability_config_.profiling_max_duration_sec),
      send_queue_(observability_config_.sender_max_queue_items,
                  observability_config_.sender_max_queue_bytes) {
  sample_session_id_ = GenerateSampleSessionId();
  // 创建 gRPC channel 和 stub。这里使用不带 TLS 的现有部署语义；RPC
  // deadline 和重试策略在 SendWithRetry() 中统一控制。
  auto channel =
      grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
  stub_ = monitor::proto::GrpcManager::NewStub(channel);

  // 创建指标采集器和内核符号索引；符号化失败不会阻断普通监控发送。
  collector_ = std::make_unique<MetricCollector>();
  symbolizer_.LoadKernelSymbols();
}

MonitorPusher::~MonitorPusher() { Stop(); }

bool MonitorPusher::Start() {
  // lifecycle_mutex_ 串行化 Start/Stop；compare_exchange 防止重复启动
  // 两套采集/发送线程。
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  if (!observability_config_.IsValid() || !collector_->Init()) {
    running_.store(false);
    std::cerr << "MonitorPusher startup validation failed" << std::endl;
    return false;
  }
  // 先打开队列，再启动消费者，保证采集线程创建后可以立即入队。
  send_queue_.Open();
  sender_thread_ =
      std::make_unique<std::thread>(&MonitorPusher::SendLoop, this);
  // 启动一个后台线程，在当前 MonitorPusher 对象上执行 PushLoop。
  // PushLoop 会循环调用 PushOnce，所以这里创建线程后，周期 Push 流程就开始了。
  thread_ = std::make_unique<std::thread>(&MonitorPusher::PushLoop, this);
  std::cout << "MonitorPusher started, pushing to " << manager_address_
            << " with adaptive sampling (normal interval "
            << observability_config_.normal_interval_ms / 1000 << " seconds)"
            << std::endl;
  return true;
}

void MonitorPusher::Stop() {
  // 先把 running_ 置 false 并唤醒等待，再 join 采集线程；采集停止后
  // 关闭队列，让 sender thread 发送完剩余项或在空队列退出。
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!running_.exchange(false)) {
    return;
  }
  stop_condition_.notify_all();
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
  send_queue_.Close();
  if (sender_thread_ && sender_thread_->joinable()) {
    sender_thread_->join();
  }
}

void MonitorPusher::PushLoop() {
  // 采集线程不直接做网络 RPC，避免 Manager 慢或重试阻塞下一次采样。
  while (running_) {
    if (!PushOnce()) {
      std::cerr << "Dropped monitor data before send to " << manager_address_
                << std::endl;
    }

    WaitForNextSample();
  }
}

bool MonitorPusher::PushOnce() {
  // ---------- 1. 采集本轮基础监控数据 ----------
  // MetricCollector 只填充 CPU/内存/磁盘/网络/主机等普通指标。
  monitor::proto::MonitorInfo info;
  if (collector_->CollectAll(&info) != CollectStatus::kOk) {
    return false;
  }
  const auto sample_timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  info.set_sample_sequence(next_sample_sequence_++);
  info.set_sample_timestamp_ms(sample_timestamp_ms);
  info.set_sample_session_id(sample_session_id_);

  // ---------- 2. 根据基础指标计算异常程度 ----------
  // AnomalyDetector 输出逐核/逐设备 max 和整体 max 分数，不改写 info。
  const auto local_anomaly = anomaly_detector_.Evaluate(info);
  const auto anomaly = remote_health_feedback_.Merge(
      local_anomaly, CanonicalHostKey(info), observability_config_);
  // ---------- 3. 推进异常诊断状态机 ----------
  // 状态转换依赖连续样本而非单次尖峰，避免瞬时抖动直接启用重型 Probe。
  state_machine_.Update(anomaly);
  const auto state = state_machine_.state();
  // ---------- 4. 根据状态按需开启/关闭诊断 Probe ----------
  const bool probes_ready =
      probe_controller_.Apply(state, SelectProfileType(anomaly));
  if (IsDiagnosticLogEnabled()) {
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    std::cout << "[KernScope] timestamp_ms=" << timestamp_ms
              << " state=" << StateName(state) << " active_probes=";
    bool first = true;
    for (const auto kind : {diagnostics::ProbeKind::kTcp,
                            diagnostics::ProbeKind::kBlockIo,
                            diagnostics::ProbeKind::kScheduler,
                            diagnostics::ProbeKind::kOnCpuProfile,
                            diagnostics::ProbeKind::kOffCpuProfile}) {
      const auto& status = probe_controller_.Status(kind);
      if (!status.requested) continue;
      if (!first) std::cout << ",";
      std::cout << ProbeName(kind) << (status.attached ? ":attached" : ":failed");
      first = false;
    }
    std::cout << " probes_ready=" << probes_ready << std::endl;
  }
  // ---------- 5. 读取本轮 eBPF/profiling 诊断快照 ----------
  diagnostics::DiagnosticSnapshot diagnostic_snapshot;
  probe_controller_.CollectSnapshot(&diagnostic_snapshot);
  // ---------- 6. 将诊断结果填入 protobuf ----------
  FillDiagnosticProto(anomaly, state, diagnostic_snapshot,
                      probe_controller_, symbolizer_, info.mutable_diagnostic());

  // 可选的 verbose 日志只读本轮 protobuf，不参与业务处理。
  if (IsMetricsLogEnabled()) {
    // 打印采集到的所有指标
    std::cout << "\n================== Collected Metrics =================="
              << std::endl;

  // 主机信息
  if (info.has_host_info()) {
    std::cout << "[Host] Hostname: " << info.host_info().hostname()
              << ", IP: " << info.host_info().ip_address() << std::endl;
  }

  // CPU 统计信息 - 所有核心
  std::cout << "\n--- CPU Statistics ---" << std::endl;
  for (int i = 0; i < info.cpu_stat_size(); ++i) {
    const auto& cpu = info.cpu_stat(i);
    std::cout << "[" << cpu.cpu_name() << "] "
              << "Total: " << cpu.cpu_percent() << "%, "
              << "User: " << cpu.usr_percent() << "%, "
              << "System: " << cpu.system_percent() << "%, "
              << "Nice: " << cpu.nice_percent() << "%, "
              << "Idle: " << cpu.idle_percent() << "%, "
              << "IOWait: " << cpu.io_wait_percent() << "%, "
              << "IRQ: " << cpu.irq_percent() << "%, "
              << "SoftIRQ: " << cpu.soft_irq_percent() << "%" << std::endl;
  }

  // CPU 负载
  if (info.has_cpu_load()) {
    std::cout << "\n--- CPU Load ---" << std::endl;
    std::cout << "[Load] 1min: " << info.cpu_load().load_avg_1()
              << ", 5min: " << info.cpu_load().load_avg_3()
              << ", 15min: " << info.cpu_load().load_avg_15() << std::endl;
  }

  // 内存信息 - 所有字段
  if (info.has_mem_info()) {
    const auto& mem = info.mem_info();
    std::cout << "\n--- Memory Info ---" << std::endl;
    std::cout << "[Memory] Used: " << mem.used_percent() << "%" << std::endl;
    std::cout << "  Total: " << mem.total() << " GiB, "
              << "Free: " << mem.free() << " GiB, "
              << "Avail: " << mem.avail() << " GiB" << std::endl;
    std::cout << "  Buffers: " << mem.buffers() << " GiB, "
              << "Cached: " << mem.cached() << " GiB, "
              << "SwapCached: " << mem.swap_cached() << " GiB" << std::endl;
    std::cout << "  Active: " << mem.active() << " GiB, "
              << "Inactive: " << mem.inactive() << " GiB" << std::endl;
    std::cout << "  ActiveAnon: " << mem.active_anon() << " GiB, "
              << "InactiveAnon: " << mem.inactive_anon() << " GiB" << std::endl;
    std::cout << "  ActiveFile: " << mem.active_file() << " GiB, "
              << "InactiveFile: " << mem.inactive_file() << " GiB" << std::endl;
    std::cout << "  Dirty: " << mem.dirty() << " GiB, "
              << "Writeback: " << mem.writeback() << " GiB" << std::endl;
    std::cout << "  AnonPages: " << mem.anon_pages() << " GiB, "
              << "Mapped: " << mem.mapped() << " GiB" << std::endl;
    std::cout << "  KReclaimable: " << mem.kreclaimable() << " GiB, "
              << "SReclaimable: " << mem.sreclaimable() << " GiB, "
              << "SUnreclaim: " << mem.sunreclaim() << " GiB" << std::endl;
  }

  // 网络信息 - 所有网卡所有字段
  if (info.net_info_size() > 0) {
    std::cout << "\n--- Network Info ---" << std::endl;
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      std::cout << "[" << net.name() << "]" << std::endl;
      std::cout << "  Recv: " << net.rcv_rate() << " KiB/s ("
                << net.rcv_packets_rate() << " pkt/s)" << std::endl;
      std::cout << "  Send: " << net.send_rate() << " KiB/s ("
                << net.send_packets_rate() << " pkt/s)" << std::endl;
      std::cout << "  Errors(in/out): " << net.err_in() << "/" << net.err_out()
                << ", Drops(in/out): " << net.drop_in() << "/" << net.drop_out()
                << std::endl;
    }
  }

  // 磁盘信息 - 所有磁盘所有字段
  if (info.disk_info_size() > 0) {
    std::cout << "\n--- Disk Info ---" << std::endl;
    for (int i = 0; i < info.disk_info_size(); ++i) {
      const auto& disk = info.disk_info(i);
      std::cout << "[" << disk.name() << "]" << std::endl;
      std::cout << "  Read: " << disk.read_bytes_per_sec() / 1024.0 << " KB/s, "
                << "IOPS: " << disk.read_iops() << ", "
                << "Latency: " << disk.avg_read_latency_ms() << " ms"
                << std::endl;
      std::cout << "  Write: " << disk.write_bytes_per_sec() / 1024.0
                << " KB/s, "
                << "IOPS: " << disk.write_iops() << ", "
                << "Latency: " << disk.avg_write_latency_ms() << " ms"
                << std::endl;
      std::cout << "  Util: " << disk.util_percent() << "%, "
                << "IO_InProgress: " << disk.io_in_progress() << std::endl;
      std::cout << "  Reads: " << disk.reads() << ", "
                << "Writes: " << disk.writes() << ", "
                << "SectorsRead: " << disk.sectors_read() << ", "
                << "SectorsWritten: " << disk.sectors_written() << std::endl;
    }
  }

  // 软中断信息 - 所有 CPU 核心
  if (info.soft_irq_size() > 0) {
    std::cout << "\n--- SoftIRQ Info ---" << std::endl;
    for (int i = 0; i < info.soft_irq_size(); ++i) {
      const auto& sirq = info.soft_irq(i);
      std::cout << "[" << sirq.cpu() << "] "
                << "HI: " << sirq.hi() << ", "
                << "TIMER: " << sirq.timer() << ", "
                << "NET_TX: " << sirq.net_tx() << ", "
                << "NET_RX: " << sirq.net_rx() << ", "
                << "BLOCK: " << sirq.block() << ", "
                << "IRQ_POLL: " << sirq.irq_poll() << ", "
                << "TASKLET: " << sirq.tasklet() << ", "
                << "SCHED: " << sirq.sched() << ", "
                << "HRTIMER: " << sirq.hrtimer() << ", "
                << "RCU: " << sirq.rcu() << std::endl;
    }
  }

    std::cout << "========================================================\n"
              << std::endl;
  }

  // ---------- 7. 放入发送队列，由发送线程异步上报 Manager ----------
  return send_queue_.Push(std::move(info));
}

void MonitorPusher::SendLoop() {
  // sender thread 与 PushLoop 解耦；队列关闭后 Pop() 会在剩余消息发送
  // 完成时返回 false，形成可等待的退出边界。
  monitor::proto::MonitorInfo info;
  while (send_queue_.Pop(&info)) {
    if (!SendWithRetry(info)) {
      std::cerr << ">>> Push failed after retries to " << manager_address_
                << " <<<" << std::endl;
    }
  }
}

bool MonitorPusher::SendWithRetry(const monitor::proto::MonitorInfo& info) {
  // 每次尝试创建独立 ClientContext，设置 RPC deadline；只重试明确的
  // 瞬态 gRPC 错误，避免把协议/参数错误重复发送。
  if (!running_.load()) {
    return false;
  }
  int backoff_ms = observability_config_.sender_retry_initial_ms;
  for (int attempt = 0; attempt <= observability_config_.sender_max_retries;
       ++attempt) {
    if (!running_.load()) {
      return false;
    }
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(
                             observability_config_.sender_rpc_deadline_ms));
    monitor::proto::MonitorFeedback response;
    const grpc::Status status =
        stub_->SetMonitorInfo(&context, info, &response);
    if (status.ok()) {
      const auto now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      remote_health_feedback_.Accept(response, CanonicalHostKey(info), now_ms);
      return true;
    }
    if (!IsRetryable(status) ||
        attempt == observability_config_.sender_max_retries) {
      return false;
    }

    // 指数退避叠加 jitter，避免多个 Worker 同时失败后形成同步重试；
    // WaitForRetry() 可被 Stop() 中的 notify 提前打断。
    static thread_local std::mt19937 random_engine(
        static_cast<std::mt19937::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const int max_backoff = observability_config_.sender_retry_max_ms;
    const int jitter_limit =
        std::min(max_backoff - backoff_ms, std::max(1, backoff_ms / 4));
    std::uniform_int_distribution<int> jitter(0, jitter_limit);
    const auto delay =
        std::chrono::milliseconds(backoff_ms + jitter(random_engine));
    if (WaitForRetry(delay)) {
      return false;
    }
    backoff_ms = backoff_ms > max_backoff / 2
                     ? max_backoff
                     : std::min(max_backoff, backoff_ms * 2);
  }
  return false;
}

bool MonitorPusher::WaitForRetry(std::chrono::milliseconds delay) {
  // condition_variable 同时承担 backoff 定时和停止通知，避免 sleep 让
  // Stop() 最多还要等待整个 retry delay。
  std::unique_lock<std::mutex> lock(stop_mutex_);
  return stop_condition_.wait_for(lock, delay,
                                  [this] { return !running_.load(); });
}

bool MonitorPusher::IsRetryable(const grpc::Status& status) {
  // 仅网络暂不可用、deadline 和资源耗尽属于当前策略允许重试的错误；
  // 其他状态直接失败，保留服务端/协议错误的可见性。
  switch (status.error_code()) {
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return true;
    default:
      return false;
  }
}

void MonitorPusher::WaitForNextSample() {
  // 用 100ms 小片段等待自适应间隔，既响应 running_ 停止，也让状态机
  // 在下一轮采集时按 NORMAL/SUSPECT/DIAGNOSTIC 选择不同频率。
  constexpr int kWaitSliceMs = 100;
  const int interval_ms = state_machine_.CurrentIntervalMs();
  for (int elapsed_ms = 0; running_ && elapsed_ms < interval_ms;
       elapsed_ms += kWaitSliceMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min(kWaitSliceMs, interval_ms - elapsed_ms)));
  }
}

}  // namespace monitor
