#include "diagnostics/root_cause_engine.h"

#include <algorithm>
#include <utility>

namespace monitor::diagnostics {
namespace {

const Evidence* Find(const std::vector<Evidence>& evidence, EvidenceType type,
                     double minimum_severity = 0.0) {
  for (const auto& item : evidence) {
    if (item.type == type && item.severity >= minimum_severity) {
      return &item;
    }
  }
  return nullptr;
}

void AddMatch(const Evidence* item, double weight, double* score,
              std::vector<std::string>* ids) {
  if (!item) {
    return;
  }
  *score += weight;
  ids->push_back(item->id);
}

RootCause MakeCause(RootCauseType type, double score,
                    std::vector<std::string> ids) {
  return RootCause{type, std::clamp(score, 0.0, 1.0), std::move(ids),
                   "rule-based diagnostic confidence: " +
                       std::string(RootCauseTypeName(type))};
}

}  // namespace

const char* RootCauseTypeName(RootCauseType type) {
  switch (type) {
    case RootCauseType::kCpuSaturation:
      return "CPU_SATURATION";
    case RootCauseType::kDiskIoSaturation:
      return "DISK_IO_SATURATION";
    case RootCauseType::kNetworkStackPressure:
      return "NETWORK_STACK_PRESSURE";
    case RootCauseType::kMemoryPressure:
      return "MEMORY_PRESSURE";
    case RootCauseType::kLockContention:
      return "LOCK_CONTENTION";
    case RootCauseType::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::vector<RootCause> RootCauseEngine::Evaluate(
    const std::vector<Evidence>& evidence) const {
  std::vector<RootCause> causes;

  const Evidence* cpu = Find(evidence, EvidenceType::kCpuUsage, 0.5);
  const Evidence* load = Find(evidence, EvidenceType::kRunQueue, 0.5);
  const Evidence* io_wait = Find(evidence, EvidenceType::kIoWait);
  const Evidence* on_cpu = Find(evidence, EvidenceType::kOnCpuStack, 0.5);
  double cpu_score = 0.0;
  std::vector<std::string> cpu_ids;
  AddMatch(cpu, 0.25, &cpu_score, &cpu_ids);
  AddMatch(load, 0.25, &cpu_score, &cpu_ids);
  if (io_wait && io_wait->severity < 0.5) {
    cpu_score += 0.25;
    cpu_ids.push_back(io_wait->id);
  }
  AddMatch(on_cpu, 0.25, &cpu_score, &cpu_ids);
  if (cpu_ids.size() >= 3) {
    causes.push_back(
        MakeCause(RootCauseType::kCpuSaturation, cpu_score, cpu_ids));
  }

  const Evidence* disk_util = Find(evidence, EvidenceType::kDiskUtil, 0.5);
  const Evidence* disk_latency =
      Find(evidence, EvidenceType::kDiskLatency, 0.5);
  const Evidence* bpf_block_latency =
      Find(evidence, EvidenceType::kBpfBlockLatency, 0.5);
  const Evidence* disk_io_wait = Find(evidence, EvidenceType::kIoWait, 0.5);
  const Evidence* off_cpu = Find(evidence, EvidenceType::kOffCpuStack, 0.5);
  double disk_score = 0.0;
  std::vector<std::string> disk_ids;
  AddMatch(disk_io_wait, 0.25, &disk_score, &disk_ids);
  AddMatch(disk_util, 0.25, &disk_score, &disk_ids);
  AddMatch(disk_latency, 0.25, &disk_score, &disk_ids);
  AddMatch(bpf_block_latency, 0.25, &disk_score, &disk_ids);
  AddMatch(off_cpu, 0.25, &disk_score, &disk_ids);
  if (disk_ids.size() >= 3) {
    causes.push_back(
        MakeCause(RootCauseType::kDiskIoSaturation, disk_score, disk_ids));
  }

  const Evidence* pps = Find(evidence, EvidenceType::kNetPps, 0.5);
  const Evidence* softirq = Find(evidence, EvidenceType::kSoftirqNetRx, 0.5);
  const Evidence* retrans = Find(evidence, EvidenceType::kTcpRetrans);
  double network_score = 0.0;
  std::vector<std::string> network_ids;
  AddMatch(pps, 1.0 / 3.0, &network_score, &network_ids);
  AddMatch(softirq, 1.0 / 3.0, &network_score, &network_ids);
  if (retrans && retrans->value > 0.0) {
    network_score += 1.0 / 3.0;
    network_ids.push_back(retrans->id);
  }
  if (network_ids.size() >= 3) {
    causes.push_back(MakeCause(RootCauseType::kNetworkStackPressure,
                               network_score, network_ids));
  }

  const Evidence* memory = Find(evidence, EvidenceType::kMemoryAvailable, 0.75);
  const Evidence* memory_cpu = Find(evidence, EvidenceType::kCpuUsage, 0.0);
  if (memory && memory_cpu && memory_cpu->severity < 0.5) {
    causes.push_back(MakeCause(RootCauseType::kMemoryPressure, 0.8,
                               {memory->id, memory_cpu->id}));
  }

  if (off_cpu && cpu && cpu->severity < 0.5) {
    causes.push_back(MakeCause(RootCauseType::kLockContention, 0.75,
                               {off_cpu->id, cpu->id}));
  }

  std::sort(causes.begin(), causes.end(),
            [](const RootCause& left, const RootCause& right) {
              return left.confidence > right.confidence;
            });
  return causes;
}

}  // namespace monitor::diagnostics
