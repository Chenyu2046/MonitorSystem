/**
 * @file probe_controller.cpp
 * @brief eBPF/perf Probe 的加载、attach、map 聚合和状态控制实现。
 *
 * 状态机 -> DesiredFor() -> LoadProbe()/Attach -> BPF map ->
 * CollectSnapshot() -> MonitorPusher protobuf。ENABLE_EBPF 未启用时，
 * Controller 保留 unavailable 语义，不改变普通监控路径；attach 失败
 * 按固定次数和递增间隔重试，避免每轮采集反复创建内核对象。
 */

#include "diagnostics/probe_controller.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <utility>
#include <vector>

#ifdef ENABLE_EBPF
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstring>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/perf_event.h>
#endif

namespace monitor::diagnostics {
namespace {

constexpr std::uint32_t kMaxAttachRetries = 3;
constexpr auto kAttachRetryBaseDelay = std::chrono::seconds(5);

/** @brief 判断一个失败 Probe 是否已达到下一次 attach 重试时间。 */
bool AttachRetryDue(const ProbeController::ProbeStatus& status,
                   ProfileSession::Clock::time_point now) {
  return status.retry_count < kMaxAttachRetries &&
         now >= status.next_retry_at;
}

/**
 * @brief 按 5s、10s 等递增间隔安排 attach 重试，最多尝试三次。
 */
void ScheduleAttachRetry(ProbeController::ProbeStatus* status,
                         ProfileSession::Clock::time_point now) {
  if (!status) return;
  ++status->retry_count;
  if (status->retry_count >= kMaxAttachRetries) {
    status->next_retry_at = ProfileSession::Clock::time_point::max();
    return;
  }
  const auto multiplier = 1u << (status->retry_count - 1);
  status->next_retry_at =
      now + kAttachRetryBaseDelay * static_cast<int>(multiplier);
}

/** @brief attach 成功或 Probe 被撤销时清空重试状态。 */
void ClearAttachRetry(ProbeController::ProbeStatus* status) {
  if (!status) return;
  status->retry_count = 0;
  status->next_retry_at = ProfileSession::Clock::time_point::min();
}

#ifdef ENABLE_EBPF

struct TcpKey {
  // 以 TGID/PID 区分进程与线程，value 记录该任务的 TCP 重传累计次数。
  std::uint32_t tgid;
  std::uint32_t pid;
};

struct TcpValue {
  std::uint64_t retransmissions;
};

struct BlockValue {
  // block_io_stats_map 的聚合值；latency 单位是纳秒，count 用于求平均。
  std::uint64_t count;
  std::uint64_t read_count;
  std::uint64_t write_count;
  std::uint64_t total_latency_ns;
  std::uint64_t max_latency_ns;
};

struct SchedulerValue {
  // Scheduler Probe 按任务累计上下文切换和唤醒次数。
  std::uint64_t switches;
  std::uint64_t wakeups;
};

struct OnCpuKey {
  // On-CPU map 按任务和 user/kernel stack id 聚合 perf_event 采样次数。
  std::uint32_t tgid;
  std::uint32_t pid;
  std::int32_t user_stack_id;
  std::int32_t kernel_stack_id;
};

struct OnCpuValue {
  std::uint64_t samples;
};

struct OffCpuKey {
  // Off-CPU map 按线程和内核栈聚合阻塞时长。
  std::uint32_t pid;
  std::int32_t kernel_stack_id;
};

struct OffCpuValue {
  std::uint64_t total_duration_ns;
  std::uint64_t samples;
};

struct LoadedProbe {
  // 一个已加载 BPF object 及其 link/perf fd 的 RAII 前置状态；销毁函数
  // 负责 detach link、关闭 perf fd 并释放 object。
  bpf_object* object = nullptr;
  std::vector<bpf_link*> links;
  std::vector<int> perf_fds;
};

/**
 * @brief 从 BPF stack map 读取 stack id 对应的地址序列。
 * @return 成功读取且至少可遍历 map 时返回 true；0 地址作为序列结束。
 */
bool ReadStackTrace(int map_fd, std::int32_t stack_id,
                    std::vector<std::uint64_t>* addresses) {
  if (!addresses || stack_id < 0 || map_fd < 0) {
    return false;
  }

  std::array<std::uint64_t, kMaxProfileStackDepth> values{};
  if (bpf_map_lookup_elem(map_fd, &stack_id, values.data()) != 0) {
    return false;
  }

  addresses->clear();
  addresses->reserve(kMaxProfileStackDepth);
  for (const auto address : values) {
    if (address == 0) {
      break;
    }
    addresses->push_back(address);
  }
  return true;
}

const char* ObjectName(ProbeKind kind) {
  switch (kind) {
    case ProbeKind::kTcp:
      return "tcp_diag.bpf.o";
    case ProbeKind::kBlockIo:
      return "block_io_diag.bpf.o";
    case ProbeKind::kScheduler:
      return "sched_diag.bpf.o";
    case ProbeKind::kOnCpuProfile:
      return "oncpu_profile.bpf.o";
    case ProbeKind::kOffCpuProfile:
      return "offcpu_profile.bpf.o";
  }
  return "";
}

void DestroyLoadedProbe(LoadedProbe* probe) {
  if (!probe) {
    return;
  }
  for (bpf_link* link : probe->links) {
    bpf_link__destroy(link);
  }
  probe->links.clear();
  for (const int fd : probe->perf_fds) {
    close(fd);
  }
  probe->perf_fds.clear();
  if (probe->object) {
    bpf_object__close(probe->object);
    probe->object = nullptr;
  }
}

bool AttachGenericPrograms(bpf_object* object, LoadedProbe* loaded,
                           int* error) {
  bpf_program* program = nullptr;
  bpf_object__for_each_program(program, object) {
    bpf_link* link = bpf_program__attach(program);
    const long attach_error = libbpf_get_error(link);
    if (attach_error) {
      if (error) {
        *error = static_cast<int>(attach_error);
      }
      return false;
    }
    loaded->links.push_back(link);
  }
  return !loaded->links.empty();
}

bool AttachOnCpuProgram(bpf_object* object, int sample_hz, LoadedProbe* loaded,
                        int* error) {
  bpf_program* program = nullptr;
  bpf_object__for_each_program(program, object) { break; }
  if (!program) {
    if (error) {
      *error = -EINVAL;
    }
    return false;
  }

  const int possible_cpus = libbpf_num_possible_cpus();
  if (possible_cpus <= 0) {
    if (error) {
      *error = -EINVAL;
    }
    return false;
  }

  perf_event_attr attr{};
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_SOFTWARE;
  attr.config = PERF_COUNT_SW_CPU_CLOCK;
  attr.freq = 1;
  attr.sample_freq = static_cast<std::uint64_t>(sample_hz);
  attr.disabled = 1;
  attr.exclude_hv = 1;

  for (int cpu = 0; cpu < possible_cpus; ++cpu) {
    const int fd =
        static_cast<int>(syscall(SYS_perf_event_open, &attr, -1, cpu, -1, 0));
    if (fd < 0) {
      if (error) {
        *error = -errno;
      }
      return false;
    }

    bpf_link* link = bpf_program__attach_perf_event(program, fd);
    const long attach_error = libbpf_get_error(link);
    if (attach_error || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
      if (error) {
        *error = attach_error ? static_cast<int>(attach_error) : -errno;
      }
      if (!attach_error) {
        bpf_link__destroy(link);
      }
      close(fd);
      return false;
    }
    loaded->links.push_back(link);
    loaded->perf_fds.push_back(fd);
  }
  return true;
}

bool LoadProbe(const std::string& object_dir, ProbeKind kind, int sample_hz,
               LoadedProbe* loaded, int* error) {
  const std::string path = object_dir + "/" + ObjectName(kind);
  bpf_object* object = bpf_object__open_file(path.c_str(), nullptr);
  const long open_error = libbpf_get_error(object);
  if (open_error) {
    if (error) {
      *error = static_cast<int>(open_error);
    }
    return false;
  }

  const int load_error = bpf_object__load(object);
  if (load_error) {
    if (error) {
      *error = load_error;
    }
    bpf_object__close(object);
    return false;
  }

  const bool attached =
      kind == ProbeKind::kOnCpuProfile
          ? AttachOnCpuProgram(object, sample_hz, loaded, error)
          : AttachGenericPrograms(object, loaded, error);
  if (!attached) {
    if (error && *error == 0) {
      *error = -EINVAL;
    }
    DestroyLoadedProbe(loaded);
    bpf_object__close(object);
    return false;
  }

  loaded->object = object;
  return true;
}

bool ReadTcpMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "tcp_diag_map");
  const int possible_cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || possible_cpus <= 0) {
    return false;
  }

  std::vector<TcpValue> per_cpu(static_cast<std::size_t>(possible_cpus));
  TcpKey current_key{};
  TcpKey next_key{};
  const void* key = nullptr;
  while (bpf_map_get_next_key(map_fd, key, &next_key) == 0) {
    if (bpf_map_lookup_elem(map_fd, &next_key, per_cpu.data()) != 0) {
      return false;
    }
    std::uint64_t retransmissions = 0;
    for (const auto& value : per_cpu) {
      retransmissions += value.retransmissions;
    }
    snapshot->tcp.push_back({next_key.tgid, next_key.pid, retransmissions});
    current_key = next_key;
    key = &current_key;
  }
  return true;
}

bool ReadBlockMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "block_io_stats_map");
  const int possible_cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || possible_cpus <= 0) {
    return false;
  }

  std::vector<BlockValue> per_cpu(static_cast<std::size_t>(possible_cpus));
  std::uint32_t zero = 0;
  if (bpf_map_lookup_elem(map_fd, &zero, per_cpu.data()) != 0) {
    return false;
  }

  BlockIoDiagnosticSample sample;
  for (const auto& value : per_cpu) {
    sample.count += value.count;
    sample.read_count += value.read_count;
    sample.write_count += value.write_count;
    sample.total_latency_ns += value.total_latency_ns;
    if (value.max_latency_ns > sample.max_latency_ns) {
      sample.max_latency_ns = value.max_latency_ns;
    }
  }
  snapshot->block_io.push_back(sample);
  return true;
}

bool ReadSchedulerMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "sched_stats_map");
  const int possible_cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || possible_cpus <= 0) {
    return false;
  }

  std::vector<SchedulerValue> per_cpu(static_cast<std::size_t>(possible_cpus));
  std::uint32_t current_key = 0;
  std::uint32_t next_key = 0;
  const void* key = nullptr;
  while (bpf_map_get_next_key(map_fd, key, &next_key) == 0) {
    if (bpf_map_lookup_elem(map_fd, &next_key, per_cpu.data()) != 0) {
      return false;
    }
    SchedulerDiagnosticSample sample;
    sample.pid = next_key;
    for (const auto& value : per_cpu) {
      sample.switches += value.switches;
      sample.wakeups += value.wakeups;
    }
    snapshot->scheduler.push_back(sample);
    current_key = next_key;
    key = &current_key;
  }
  std::sort(snapshot->scheduler.begin(), snapshot->scheduler.end(),
            [](const auto& left, const auto& right) {
              return left.switches + left.wakeups >
                     right.switches + right.wakeups;
            });
  constexpr std::size_t kMaxSchedulerSamples = 20;
  if (snapshot->scheduler.size() > kMaxSchedulerSamples) {
    snapshot->scheduler.resize(kMaxSchedulerSamples);
  }
  return true;
}

bool ReadOnCpuMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "oncpu_stack_counts");
  const int stack_map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "oncpu_stack_traces");
  const int possible_cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || possible_cpus <= 0) {
    return false;
  }

  std::vector<OnCpuValue> per_cpu(static_cast<std::size_t>(possible_cpus));
  OnCpuKey current_key{};
  OnCpuKey next_key{};
  const void* key = nullptr;
  while (bpf_map_get_next_key(map_fd, key, &next_key) == 0) {
    if (bpf_map_lookup_elem(map_fd, &next_key, per_cpu.data()) != 0) {
      return false;
    }
    OnCpuProfileSample sample;
    sample.tgid = next_key.tgid;
    sample.pid = next_key.pid;
    sample.user_stack_id = next_key.user_stack_id;
    sample.kernel_stack_id = next_key.kernel_stack_id;
    for (const auto& value : per_cpu) {
      sample.samples += value.samples;
    }
    ReadStackTrace(stack_map_fd, sample.user_stack_id, &sample.user_stack);
    ReadStackTrace(stack_map_fd, sample.kernel_stack_id,
                   &sample.kernel_stack);
    snapshot->profiling.on_cpu.push_back(sample);
    current_key = next_key;
    key = &current_key;
  }
  return true;
}

bool ReadOffCpuMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "offcpu_aggregate");
  const int stack_map_fd =
      bpf_object__find_map_fd_by_name(loaded.object, "offcpu_stack_traces");
  const int possible_cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || possible_cpus <= 0) {
    return false;
  }

  std::vector<OffCpuValue> per_cpu(static_cast<std::size_t>(possible_cpus));
  OffCpuKey current_key{};
  OffCpuKey next_key{};
  const void* key = nullptr;
  while (bpf_map_get_next_key(map_fd, key, &next_key) == 0) {
    if (bpf_map_lookup_elem(map_fd, &next_key, per_cpu.data()) != 0) {
      return false;
    }
    OffCpuProfileSample sample;
    sample.pid = next_key.pid;
    sample.kernel_stack_id = next_key.kernel_stack_id;
    for (const auto& value : per_cpu) {
      sample.total_duration_ns += value.total_duration_ns;
      sample.samples += value.samples;
    }
    ReadStackTrace(stack_map_fd, sample.kernel_stack_id,
                   &sample.kernel_stack);
    snapshot->profiling.off_cpu.push_back(sample);
    current_key = next_key;
    key = &current_key;
  }
  return true;
}

#endif  // ENABLE_EBPF

}  // namespace

struct ProbeController::Runtime {
#ifdef ENABLE_EBPF
  std::array<LoadedProbe, 5> probes;
#endif
};

ProbeController::ProbeController(std::string object_dir, int profile_sample_hz,
                                 int profile_max_duration_sec)
    : object_dir_(object_dir.empty() ? "worker/src/ebpf/.output"
                                     : std::move(object_dir)),
      profile_sample_hz_(std::max(1, profile_sample_hz)),
      profile_max_duration_(
          std::chrono::seconds(std::max(1, profile_max_duration_sec))),
      runtime_(std::make_unique<Runtime>()) {}

ProbeController::~ProbeController() {
  if (profile_session_) {
    profile_session_->Close();
    profile_session_.reset();
  }
#ifdef ENABLE_EBPF
  for (auto& probe : runtime_->probes) {
    DestroyLoadedProbe(&probe);
  }
#endif
}

bool ProbeController::Apply(ObservabilityState state, ProfileType profile_type,
                            ProfileSession::Clock::time_point now) {
  // Apply 是状态机与内核 Probe 之间的唯一编排点：先处理 profiling
  // session 生命周期，再根据目标集合增量 attach/detach，最后更新重试
  // 状态。调用发生在 MonitorPusher 的控制循环中。
  const bool profiling_requested = state == ObservabilityState::kProfiling;
  if (!profiling_requested) {
    profile_expired_ = false;
  }
  const bool profile_expired =
      profile_session_ && profiling_requested && profile_session_->Expired(now);
  if (profile_session_ &&
      (!profiling_requested || profile_session_->type() != profile_type ||
       profile_expired)) {
    profile_session_->Close();
    profile_session_.reset();
  }

  if (profile_expired) {
    profile_expired_ = true;
  }
  if (profiling_requested && !profile_session_ && !profile_expired_) {
    profile_session_ = std::make_unique<ProfileSession>(
        next_profile_id_++, profile_type, profile_max_duration_, std::nullopt,
        [this] { DetachProfile(); });
    profile_session_->Start(now);
  }

  const bool profile_active =
      profile_session_ && profile_session_->active() && profiling_requested;
  // desired 只描述当前状态所需的 Probe；例如 SUSPECT 只需要 TCP/Block
  // I/O，DIAGNOSTIC 再增加 Scheduler，profiling 才增加一种 stack Probe。
  const auto desired = DesiredFor(state, profile_type, profile_active);
  const bool changed = !initialized_ || desired != desired_probes_;

  const auto all_available_now = [this] {
    for (const ProbeKind kind : desired_probes_) {
      const ProbeStatus& status = statuses_[Index(kind)];
      if (!status.requested || !status.available || !status.attached) {
        return false;
      }
    }
    return true;
  };

  if (!changed) {
#ifdef ENABLE_EBPF
    bool retry_due = false;
    for (const ProbeKind kind : desired_probes_) {
      const ProbeStatus& status = statuses_[Index(kind)];
      if (!status.attached && AttachRetryDue(status, now)) {
        retry_due = true;
        break;
      }
    }
    if (!retry_due) return all_available_now();
#else
    return all_available_now();
#endif
  }

  // 对五类 Probe 统一处理 requested/available/attached 状态。非 requested
  // 的 Probe 必须释放旧 runtime，避免状态降级后继续持有内核资源。
  bool all_available = true;
  for (ProbeKind kind :
       {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler,
        ProbeKind::kOnCpuProfile, ProbeKind::kOffCpuProfile}) {
    ProbeStatus& status = statuses_[Index(kind)];
    const bool requested = desired.find(kind) != desired.end();
    status.requested = requested;

    if (!requested) {
#ifdef ENABLE_EBPF
      DestroyLoadedProbe(&runtime_->probes[Index(kind)]);
#endif
      status.available = false;
      status.attached = false;
      status.last_error = 0;
      ClearAttachRetry(&status);
      continue;
    }

#ifdef ENABLE_EBPF
    if (!changed && status.attached) {
      all_available = all_available && status.available;
      continue;
    }
    if (!changed && !AttachRetryDue(status, now)) {
      all_available = false;
      continue;
    }
    DestroyLoadedProbe(&runtime_->probes[Index(kind)]);
    status.last_error = 0;
    // LoadProbe 内部完成 BPF object load、hook/perf attach 和 map 准备；
    // 失败只记录状态并安排有限重试，不阻塞基础 MonitorInfo 上报。
    status.attached =
        LoadProbe(object_dir_, kind, profile_sample_hz_,
                  &runtime_->probes[Index(kind)], &status.last_error);
    status.available = status.attached;
    if (status.attached) {
      ClearAttachRetry(&status);
    } else {
      ScheduleAttachRetry(&status, now);
    }
#else
    status.available = false;
    status.attached = false;
    status.last_error = -ENOTSUP;
#endif
    all_available = all_available && status.available;
  }

  desired_probes_ = desired;
  initialized_ = true;
  ++apply_count_;
  return all_available;
}

bool ProbeController::CollectSnapshot(DiagnosticSnapshot* snapshot) {
  // 先清空输出，避免把上一轮诊断样本误带入当前 protobuf；每种 Probe
  // 只在 attached 时读取对应 map，读取失败通过返回值暴露给调用方。
  if (!snapshot) {
    return false;
  }
  *snapshot = {};

#ifdef ENABLE_EBPF
  snapshot->tcp_available = Status(ProbeKind::kTcp).attached;
  snapshot->block_io_available = Status(ProbeKind::kBlockIo).attached;
  snapshot->scheduler_available = Status(ProbeKind::kScheduler).attached;
  snapshot->profiling.on_cpu_available =
      Status(ProbeKind::kOnCpuProfile).attached;
  snapshot->profiling.off_cpu_available =
      Status(ProbeKind::kOffCpuProfile).attached;

  bool success = true;
  for (auto& status : statuses_) {
    status.snapshot_ok = status.attached || !status.requested;
    status.snapshot_error = status.snapshot_ok ? 0 : status.last_error;
  }
  const auto read = [&](ProbeKind kind, auto&& reader) {
    ProbeStatus& status = Status(kind);
    if (!status.attached) {
      const bool ok = !status.requested;
      success = success && ok;
      return ok;
    }
    status.snapshot_ok = reader();
    status.snapshot_error = status.snapshot_ok ? 0 : -EIO;
    success = status.snapshot_ok && success;
    return status.snapshot_ok;
  };
  if (snapshot->tcp_available) {
    read(ProbeKind::kTcp, [&] {
      return ReadTcpMap(runtime_->probes[Index(ProbeKind::kTcp)], snapshot);
    });
  }
  if (snapshot->block_io_available) {
    read(ProbeKind::kBlockIo, [&] {
      return ReadBlockMap(runtime_->probes[Index(ProbeKind::kBlockIo)], snapshot);
    });
  }
  if (snapshot->scheduler_available) {
    read(ProbeKind::kScheduler, [&] {
      return ReadSchedulerMap(runtime_->probes[Index(ProbeKind::kScheduler)],
                              snapshot);
    });
  }
  if (snapshot->profiling.on_cpu_available) {
    read(ProbeKind::kOnCpuProfile, [&] {
      return ReadOnCpuMap(runtime_->probes[Index(ProbeKind::kOnCpuProfile)],
                          snapshot);
    });
  }
  if (snapshot->profiling.off_cpu_available) {
    read(ProbeKind::kOffCpuProfile, [&] {
      return ReadOffCpuMap(runtime_->probes[Index(ProbeKind::kOffCpuProfile)],
                           snapshot);
    });
  }
  return success;
#else
  for (auto& status : statuses_) {
    status.snapshot_ok = false;
    status.snapshot_error = -ENOTSUP;
  }
  return true;
#endif
}

const ProbeController::ProbeStatus& ProbeController::Status(
    ProbeKind kind) const {
  return statuses_[Index(kind)];
}

ProbeController::ProbeStatus& ProbeController::Status(ProbeKind kind) {
  return statuses_[Index(kind)];
}

std::size_t ProbeController::Index(ProbeKind kind) {
  switch (kind) {
    case ProbeKind::kTcp:
      return 0;
    case ProbeKind::kBlockIo:
      return 1;
    case ProbeKind::kScheduler:
      return 2;
    case ProbeKind::kOnCpuProfile:
      return 3;
    case ProbeKind::kOffCpuProfile:
      return 4;
  }
  return 0;
}

std::set<ProbeKind> ProbeController::DesiredFor(ObservabilityState state,
                                                ProfileType profile_type,
                                                bool profile_active) {
  // 状态到 Probe 的映射体现诊断成本分层：普通/怀疑/诊断逐级增加资源，
  // profiling 只在 ProfileSession 已 active 时 attach OnCPU 或 OffCPU。
  std::set<ProbeKind> desired;
  switch (state) {
    case ObservabilityState::kNormal:
      return desired;
    case ObservabilityState::kSuspect:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo};
    case ObservabilityState::kDiagnostic:
    case ObservabilityState::kCooldown:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler};
    case ObservabilityState::kProfiling:
      desired = {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler};
      if (profile_active) {
        desired.insert(profile_type == ProfileType::kOnCpu
                           ? ProbeKind::kOnCpuProfile
                           : ProbeKind::kOffCpuProfile);
      }
      return desired;
  }
  return desired;
}

void ProbeController::DetachProfile() {
#ifdef ENABLE_EBPF
  DestroyLoadedProbe(&runtime_->probes[Index(ProbeKind::kOnCpuProfile)]);
  DestroyLoadedProbe(&runtime_->probes[Index(ProbeKind::kOffCpuProfile)]);
#endif
  for (ProbeKind kind : {ProbeKind::kOnCpuProfile, ProbeKind::kOffCpuProfile}) {
    ProbeStatus& status = statuses_[Index(kind)];
    status.requested = false;
    status.available = false;
    status.attached = false;
    status.last_error = 0;
    ClearAttachRetry(&status);
  }
}

}  // namespace monitor::diagnostics
