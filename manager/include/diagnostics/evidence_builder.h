#pragma once

/**
 * @file evidence_builder.h
 * @brief Manager 将 MonitorInfo 和诊断 protobuf 转换为结构化证据。
 *
 * Evidence 是 RootCauseEngine 的输入，保留来源、目标、原始值、单位、
 * 严重度、时间和解释 detail。它不是机器学习特征表，也不负责持久化。
 */

#include <chrono>
#include <string>
#include <vector>

#include "health/health_types.h"
#include "monitor_info.pb.h"

namespace monitor::diagnostics {

/** @brief 当前规则引擎支持的证据类型。 */
enum class EvidenceType {
  kCpuUsage,
  kRunQueue,
  kIoWait,
  kDiskUtil,
  kDiskLatency,
  kBpfBlockLatency,
  kNetPps,
  kTcpRetrans,
  kSoftirqNetRx,
  kSchedulerSwitches,
  kSchedulerWakeups,
  kDiagnosticCapabilityDegraded,
  kMemoryAvailable,
  kOnCpuStack,
  kOffCpuStack,
  kLockWaitStack,
  kHealthAnomalySignal,
};

/**
 * @brief 一条可追溯的异常证据。
 *
 * value 使用 unit 指定的原单位；severity 是 0~1 规则严重度；source/
 * target 用于区分普通主机指标、具体设备、PID/TID 或 Probe。
 */
struct Evidence {
  std::string id;
  EvidenceType type;
  std::string source;
  std::string target;
  double value = 0.0;
  std::string unit;
  double severity = 0.0;
  std::chrono::system_clock::time_point timestamp;
  std::string detail;
};

/** @brief 返回稳定的证据类型字符串，用于日志、ID 和持久化。 */
const char* EvidenceTypeName(EvidenceType type);

/**
 * @brief 从一轮 MonitorInfo 构造普通指标和诊断样本证据。
 *
 * CPU/IOWait/磁盘保留最大核或最大设备，网络包速率求和；这些聚合规则
 * 面向诊断，不替代 HostManager 的平均 CPU 概览。
 */
class EvidenceBuilder {
 public:
  /** @brief 构造本轮所有可用证据，缺少某类字段时跳过该类。 */
  std::vector<Evidence> Build(
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point timestamp,
      const health::HealthResult* health_result = nullptr) const;
};

}  // namespace monitor::diagnostics
