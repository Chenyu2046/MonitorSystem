#pragma once

namespace monitor {

enum class DataReceiveResult {
  kAccepted,
  kQueueFull,
  kStopping,
  kInvalidHost,
};

}  // namespace monitor
