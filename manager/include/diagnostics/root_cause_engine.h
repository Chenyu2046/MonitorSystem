#pragma once

/**
 * @file root_cause_engine.h
 * @brief 基于多条 Evidence 的规则型根因组合接口。
 *
 * RootCauseEngine 是确定性的规则系统，不是机器学习模型；confidence
 * 来自命中证据的固定权重，evidence_ids 用于回溯具体输入。
 */

#include <string>
#include <vector>

#include "diagnostics/evidence_builder.h"

namespace monitor::diagnostics {

/** @brief 当前实现可识别的规则型根因类别。 */
enum class RootCauseType {
  kCpuSaturation,
  kDiskIoSaturation,
  kNetworkStackPressure,
  kMemoryPressure,
  kLockContention,
  kUnknown,
};

/**
 * @brief 一条由多证据组合得到的规则根因。
 */
struct RootCause {
  RootCauseType type = RootCauseType::kUnknown;
  double confidence = 0.0;
  std::vector<std::string> evidence_ids;
  std::string summary;
};

/** @brief 返回稳定根因名称，用于日志、查询和 MySQL。 */
const char* RootCauseTypeName(RootCauseType type);

/** @brief 组合 CPU/磁盘/网络/内存/锁等待证据并排序根因。 */
class RootCauseEngine {
 public:
  /** @brief 评估所有规则并按 confidence 从高到低返回命中结果。 */
  std::vector<RootCause> Evaluate(const std::vector<Evidence>& evidence) const;
};

}  // namespace monitor::diagnostics
