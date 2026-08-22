#pragma once

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

}  // namespace monitor
