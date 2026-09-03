#pragma once

#include <string>
#include <vector>

#include "health/health_types.h"

namespace monitor::health {

// 将 Top Signals 编码为数据库字段使用的紧凑文本。
std::string EncodeTopSignals(const std::vector<TopSignal>& signals);
// 从持久化文本恢复 Top Signals，忽略非法记录并限制数量。
std::vector<TopSignal> DecodeTopSignals(const std::string& encoded);

}  // namespace monitor::health
