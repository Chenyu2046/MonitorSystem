#pragma once

#include <cstdint>
#include <string>

/**
 * @file data_receive_result.h
 * @brief Manager 接收路径对 gRPC handler 的有限结果分类。
 */

namespace monitor {

/**
 * @brief 表示消息是否进入 HostManager 有界处理链及失败原因。
 */
enum class DataReceiveResult {
  kAccepted,
  kQueueFull,
  kStopping,
  kInvalidHost,
};

/** @brief 返回队列拒绝结果对应的稳定日志原因。 */
inline const char* QueueRejectReason(DataReceiveResult result) {
  switch (result) {
    case DataReceiveResult::kQueueFull:
      return "queue_full";
    case DataReceiveResult::kStopping:
      return "stopping";
    default:
      return "unknown";
  }
}

/** @brief Result produced by one exact shard WorkItem for RPC feedback. */
struct HostFeedbackResult {
  std::string host_name;
  double node_anomaly_score = 0.0;
  double remote_trigger_score = 0.0;
  std::int64_t result_timestamp_ms = 0;
  std::uint64_t result_version = 0;
  bool health_valid = false;
};

}  // namespace monitor
