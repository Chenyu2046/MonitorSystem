/**
 * @file root_cause_engine.cpp
 * @brief 多证据规则型 RootCause 计算实现。
 *
 * 每个根因由固定证据组合和权重组成，至少命中若干条件才产生结果；
 * confidence 被限制在 0~1，最终按置信度排序。这里没有模型训练或在线
 * 学习，结果只能解释为规则匹配强度。
 */

#include "diagnostics/root_cause_engine.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

namespace monitor::diagnostics {
namespace {

/** @brief 找到指定类型且达到最低严重度的第一条证据。 */
const Evidence* Find(const std::vector<Evidence>& evidence, EvidenceType type,
                     double minimum_severity = 0.0) {
  for (const auto& item : evidence) {
    if (item.type == type && item.severity >= minimum_severity) {
      return &item;
    }
  }
  return nullptr;
}

const Evidence* FindHealth(const std::vector<Evidence>& evidence,
                           EvidenceType type,
                           double minimum_severity = 0.5) {
  const Evidence* strongest = nullptr;
  for (const auto& item : evidence) {
    if (item.type == type && item.source == "HealthScoreEngine" &&
        item.severity >= minimum_severity &&
        (!strongest || item.severity > strongest->severity)) {
      strongest = &item;
    }
  }
  return strongest;
}

const Evidence* FindHealthDomain(const std::vector<Evidence>& evidence,
                                 std::initializer_list<EvidenceType> types,
                                 double minimum_severity = 0.5) {
  const Evidence* strongest = nullptr;
  for (const auto type : types) {
    const Evidence* candidate = FindHealth(evidence, type, minimum_severity);
    if (candidate && (!strongest || candidate->severity > strongest->severity)) {
      strongest = candidate;
    }
  }
  return strongest;
}

/** @brief 将命中证据的固定权重加入 confidence 并记录证据 ID。 */
void AddMatch(const Evidence* item, double weight, double* score,
              std::vector<std::string>* ids) {
  if (!item) {
    return;
  }
  *score += weight;
  ids->push_back(item->id);
}

/** @brief 构造并限制规则型根因结果的 confidence。 */
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
  // 先按证据类型取候选，再按每个规则的组合条件生成根因；不同根因可以
  // 同时命中，因为一台主机可能存在叠加压力。
  std::vector<RootCause> causes;

  const Evidence* cpu = Find(evidence, EvidenceType::kCpuUsage, 0.5);
  const Evidence* load = Find(evidence, EvidenceType::kRunQueue, 0.5);
  const Evidence* io_wait = Find(evidence, EvidenceType::kIoWait);
  const Evidence* on_cpu = Find(evidence, EvidenceType::kOnCpuStack, 0.5);
  const Evidence* health_cpu =
      FindHealth(evidence, EvidenceType::kCpuUsage);
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
  } else if (health_cpu) {
    causes.push_back(MakeCause(RootCauseType::kCpuSaturation,
                               health_cpu->severity, {health_cpu->id}));
  }

  const Evidence* disk_util = Find(evidence, EvidenceType::kDiskUtil, 0.5);
  const Evidence* disk_latency =
      Find(evidence, EvidenceType::kDiskLatency, 0.5);
  const Evidence* bpf_block_latency =
      Find(evidence, EvidenceType::kBpfBlockLatency, 0.5);
  const Evidence* disk_io_wait = Find(evidence, EvidenceType::kIoWait, 0.5);
  const Evidence* off_cpu = Find(evidence, EvidenceType::kOffCpuStack, 0.5);
  const Evidence* health_disk_util =
      FindHealth(evidence, EvidenceType::kDiskUtil);
  const Evidence* health_disk_latency =
      FindHealth(evidence, EvidenceType::kDiskLatency);
  const Evidence* health_disk_signal =
      FindHealth(evidence, EvidenceType::kHealthDiskSignal);
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
  } else {
    const Evidence* health_disk =
        !health_disk_latency ||
                (health_disk_util && health_disk_util->severity >=
                                         health_disk_latency->severity)
            ? health_disk_util
            : health_disk_latency;
    if (!health_disk ||
        (health_disk_signal && health_disk_signal->severity >
                                   health_disk->severity)) {
      health_disk = health_disk_signal;
    }
    if (health_disk) {
      causes.push_back(MakeCause(RootCauseType::kDiskIoSaturation,
                                 health_disk->severity, {health_disk->id}));
    }
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
  } else {
    const Evidence* health_network = FindHealthDomain(
        evidence, {EvidenceType::kHealthNetworkSignal});
    if (health_network) {
      // Throughput-only anomalies are useful activity evidence, but are not
      // proof of a network fault; retain the documented confidence ceiling.
      const bool throughput_only =
          health_network->target.find("throughput") != std::string::npos;
      causes.push_back(MakeCause(
          RootCauseType::kNetworkStackPressure,
          throughput_only ? std::min(0.5, health_network->severity)
                          : health_network->severity,
          {health_network->id}));
    }
  }

  const Evidence* memory = Find(evidence, EvidenceType::kMemoryAvailable, 0.75);
  const Evidence* health_memory =
      FindHealth(evidence, EvidenceType::kHealthMemorySignal);
  const Evidence* memory_cpu = Find(evidence, EvidenceType::kCpuUsage, 0.0);
  if (memory && memory_cpu && memory_cpu->severity < 0.5) {
    causes.push_back(MakeCause(RootCauseType::kMemoryPressure, 0.8,
                               {memory->id, memory_cpu->id}));
  } else if (health_memory) {
    causes.push_back(MakeCause(RootCauseType::kMemoryPressure,
                               health_memory->severity,
                               {health_memory->id}));
  }

  const Evidence* any_cpu = Find(evidence, EvidenceType::kCpuUsage);
  const Evidence* lock_wait =
      Find(evidence, EvidenceType::kLockWaitStack, 0.5);
  if (off_cpu && off_cpu->value > 0.0 && lock_wait &&
      lock_wait->value > 0.0 && any_cpu && any_cpu->severity < 0.5) {
    causes.push_back(MakeCause(RootCauseType::kLockContention, 0.75,
                               {off_cpu->id, lock_wait->id, any_cpu->id}));
  }

  std::sort(causes.begin(), causes.end(),
            [](const RootCause& left, const RootCause& right) {
              return left.confidence > right.confidence;
            });
  return causes;
}

}  // namespace monitor::diagnostics
