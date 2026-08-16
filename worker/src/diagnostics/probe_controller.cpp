#include "diagnostics/probe_controller.h"

#include <cerrno>
#include <cstdint>
#include <utility>
#include <vector>

#ifdef ENABLE_EBPF
#include <array>
#include <string>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#endif

namespace monitor::diagnostics {
namespace {

#ifdef ENABLE_EBPF

struct TcpKey {
  std::uint32_t tgid;
  std::uint32_t pid;
};

struct TcpValue {
  std::uint64_t retransmissions;
};

struct BlockValue {
  std::uint64_t count;
  std::uint64_t read_count;
  std::uint64_t write_count;
  std::uint64_t total_latency_ns;
  std::uint64_t max_latency_ns;
};

struct SchedulerValue {
  std::uint64_t switches;
  std::uint64_t wakeups;
};

struct LoadedProbe {
  bpf_object* object = nullptr;
  std::vector<bpf_link*> links;
};

const char* ObjectName(ProbeKind kind) {
  switch (kind) {
    case ProbeKind::kTcp:
      return "tcp_diag.bpf.o";
    case ProbeKind::kBlockIo:
      return "block_io_diag.bpf.o";
    case ProbeKind::kScheduler:
      return "sched_diag.bpf.o";
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
  if (probe->object) {
    bpf_object__close(probe->object);
    probe->object = nullptr;
  }
}

bool LoadProbe(const std::string& object_dir, ProbeKind kind,
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

  bpf_program* program = nullptr;
  bpf_object__for_each_program(program, object) {
    bpf_link* link = bpf_program__attach(program);
    const long attach_error = libbpf_get_error(link);
    if (attach_error) {
      if (error) {
        *error = static_cast<int>(attach_error);
      }
      for (bpf_link* attached : loaded->links) {
        bpf_link__destroy(attached);
      }
      loaded->links.clear();
      bpf_object__close(object);
      return false;
    }
    loaded->links.push_back(link);
  }

  if (loaded->links.empty()) {
    if (error) {
      *error = -EINVAL;
    }
    bpf_object__close(object);
    return false;
  }

  loaded->object = object;
  return true;
}

bool ReadTcpMap(const LoadedProbe& loaded, DiagnosticSnapshot* snapshot) {
  const int map_fd = bpf_object__find_map_fd(loaded.object, "tcp_diag_map");
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
      bpf_object__find_map_fd(loaded.object, "block_io_stats_map");
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
  const int map_fd = bpf_object__find_map_fd(loaded.object, "sched_stats_map");
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
  return true;
}

#endif  // ENABLE_EBPF

}  // namespace

struct ProbeController::Runtime {
#ifdef ENABLE_EBPF
  std::array<LoadedProbe, 3> probes;
#endif
};

ProbeController::ProbeController(std::string object_dir)
    : object_dir_(object_dir.empty() ? "worker/src/ebpf/.output"
                                     : std::move(object_dir)),
      runtime_(std::make_unique<Runtime>()) {}

ProbeController::~ProbeController() {
#ifdef ENABLE_EBPF
  for (auto& probe : runtime_->probes) {
    DestroyLoadedProbe(&probe);
  }
#endif
}

bool ProbeController::Apply(ObservabilityState state) {
  const auto desired = DesiredFor(state);
  const bool changed = !initialized_ || desired != desired_probes_;
  if (!changed) {
    return true;
  }

  bool all_available = true;
  for (ProbeKind kind :
       {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler}) {
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
      continue;
    }

#ifdef ENABLE_EBPF
    DestroyLoadedProbe(&runtime_->probes[Index(kind)]);
    status.last_error = 0;
    status.attached = LoadProbe(
        object_dir_, kind, &runtime_->probes[Index(kind)], &status.last_error);
    status.available = status.attached;
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

bool ProbeController::CollectSnapshot(DiagnosticSnapshot* snapshot) const {
  if (!snapshot) {
    return false;
  }
  *snapshot = {};

#ifdef ENABLE_EBPF
  snapshot->tcp_available = Status(ProbeKind::kTcp).attached;
  snapshot->block_io_available = Status(ProbeKind::kBlockIo).attached;
  snapshot->scheduler_available = Status(ProbeKind::kScheduler).attached;

  bool success = true;
  if (snapshot->tcp_available) {
    success = ReadTcpMap(runtime_->probes[Index(ProbeKind::kTcp)], snapshot) &&
              success;
  }
  if (snapshot->block_io_available) {
    success =
        ReadBlockMap(runtime_->probes[Index(ProbeKind::kBlockIo)], snapshot) &&
        success;
  }
  if (snapshot->scheduler_available) {
    success = ReadSchedulerMap(runtime_->probes[Index(ProbeKind::kScheduler)],
                               snapshot) &&
              success;
  }
  return success;
#else
  return true;
#endif
}

const ProbeController::ProbeStatus& ProbeController::Status(
    ProbeKind kind) const {
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
  }
  return 0;
}

std::set<ProbeKind> ProbeController::DesiredFor(ObservabilityState state) {
  switch (state) {
    case ObservabilityState::kNormal:
    case ObservabilityState::kCooldown:
      return {};
    case ObservabilityState::kSuspect:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo};
    case ObservabilityState::kDiagnostic:
    case ObservabilityState::kProfiling:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler};
  }
  return {};
}

}  // namespace monitor::diagnostics
