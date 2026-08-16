#include "rpc/monitor_pusher.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

namespace {

monitor::diagnostics::ObservabilityConfig MakeObservabilityConfig(
    int interval_seconds) {
  monitor::diagnostics::ObservabilityConfig config;
  if (interval_seconds > 0) {
    config.normal_interval_ms = interval_seconds * 1000;
  }
  if (const char* object_dir = std::getenv("KERNSCOPE_EBPF_OBJECT_DIR")) {
    if (*object_dir != '\0') {
      config.ebpf_object_dir = object_dir;
    }
  }
  return config;
}

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

void FillDiagnosticProto(
    const monitor::diagnostics::AnomalyResult& anomaly,
    monitor::diagnostics::ObservabilityState state,
    const monitor::diagnostics::DiagnosticSnapshot& snapshot,
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

  for (const auto& sample : snapshot.tcp) {
    auto* signal = diagnostic->add_signals();
    signal->set_domain(monitor::proto::DOMAIN_NETWORK);
    signal->set_metric("tcp_retransmissions");
    signal->set_value(static_cast<double>(sample.retransmissions));
    signal->set_unit("count");
  }
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
  }

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
      anomaly_detector_(observability_config_),
      state_machine_(observability_config_),
      probe_controller_(observability_config_.ebpf_object_dir,
                        observability_config_.profiling_sample_hz,
                        observability_config_.profiling_max_duration_sec),
      send_queue_(observability_config_.sender_max_queue_items,
                  observability_config_.sender_max_queue_bytes) {
  // 创建 gRPC channel 和 stub
  auto channel =
      grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
  stub_ = monitor::proto::GrpcManager::NewStub(channel);

  // 创建指标采集器
  collector_ = std::make_unique<MetricCollector>();
}

MonitorPusher::~MonitorPusher() { Stop(); }

void MonitorPusher::Start() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
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
}

void MonitorPusher::Stop() {
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
  while (running_) {
    if (!PushOnce()) {
      std::cerr << "Dropped monitor data before send to " << manager_address_
                << std::endl;
    }

    WaitForNextSample();
  }
}

bool MonitorPusher::PushOnce() {
  // 采集监控数据
  monitor::proto::MonitorInfo info;
  collector_->CollectAll(&info);

  const auto anomaly = anomaly_detector_.Evaluate(info);
  state_machine_.Update(anomaly);
  probe_controller_.Apply(state_machine_.state(), SelectProfileType(anomaly));
  diagnostics::DiagnosticSnapshot diagnostic_snapshot;
  probe_controller_.CollectSnapshot(&diagnostic_snapshot);
  FillDiagnosticProto(anomaly, state_machine_.state(), diagnostic_snapshot,
                      info.mutable_diagnostic());

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
    std::cout << "  Total: " << mem.total() << " MB, "
              << "Free: " << mem.free() << " MB, "
              << "Avail: " << mem.avail() << " MB" << std::endl;
    std::cout << "  Buffers: " << mem.buffers() << " MB, "
              << "Cached: " << mem.cached() << " MB, "
              << "SwapCached: " << mem.swap_cached() << " MB" << std::endl;
    std::cout << "  Active: " << mem.active() << " MB, "
              << "Inactive: " << mem.inactive() << " MB" << std::endl;
    std::cout << "  ActiveAnon: " << mem.active_anon() << " MB, "
              << "InactiveAnon: " << mem.inactive_anon() << " MB" << std::endl;
    std::cout << "  ActiveFile: " << mem.active_file() << " MB, "
              << "InactiveFile: " << mem.inactive_file() << " MB" << std::endl;
    std::cout << "  Dirty: " << mem.dirty() << " MB, "
              << "Writeback: " << mem.writeback() << " MB" << std::endl;
    std::cout << "  AnonPages: " << mem.anon_pages() << " MB, "
              << "Mapped: " << mem.mapped() << " MB" << std::endl;
    std::cout << "  KReclaimable: " << mem.kreclaimable() << " MB, "
              << "SReclaimable: " << mem.sreclaimable() << " MB, "
              << "SUnreclaim: " << mem.sunreclaim() << " MB" << std::endl;
  }

  // 网络信息 - 所有网卡所有字段
  if (info.net_info_size() > 0) {
    std::cout << "\n--- Network Info ---" << std::endl;
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      std::cout << "[" << net.name() << "]" << std::endl;
      std::cout << "  Recv: " << net.rcv_rate() << " B/s ("
                << net.rcv_packets_rate() << " pkt/s)" << std::endl;
      std::cout << "  Send: " << net.send_rate() << " B/s ("
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

  return send_queue_.Push(std::move(info));
}

void MonitorPusher::SendLoop() {
  monitor::proto::MonitorInfo info;
  while (send_queue_.Pop(&info)) {
    if (!SendWithRetry(info)) {
      std::cerr << ">>> Push failed after retries to " << manager_address_
                << " <<<" << std::endl;
    }
  }
}

bool MonitorPusher::SendWithRetry(const monitor::proto::MonitorInfo& info) {
  int backoff_ms = observability_config_.sender_retry_initial_ms;
  for (int attempt = 0; attempt <= observability_config_.sender_max_retries;
       ++attempt) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(
                             observability_config_.sender_rpc_deadline_ms));
    google::protobuf::Empty response;
    const grpc::Status status =
        stub_->SetMonitorInfo(&context, info, &response);
    if (status.ok()) {
      return true;
    }
    if (!IsRetryable(status) ||
        attempt == observability_config_.sender_max_retries) {
      return false;
    }

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
  std::unique_lock<std::mutex> lock(stop_mutex_);
  return stop_condition_.wait_for(lock, delay,
                                  [this] { return !running_.load(); });
}

bool MonitorPusher::IsRetryable(const grpc::Status& status) {
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
  constexpr int kWaitSliceMs = 100;
  const int interval_ms = state_machine_.CurrentIntervalMs();
  for (int elapsed_ms = 0; running_ && elapsed_ms < interval_ms;
       elapsed_ms += kWaitSliceMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min(kWaitSliceMs, interval_ms - elapsed_ms)));
  }
}

}  // namespace monitor
