#pragma once

#include <string>
#include <vector>

#include "health/health_types.h"

namespace monitor::health {

std::string EncodeTopSignals(const std::vector<TopSignal>& signals);
std::vector<TopSignal> DecodeTopSignals(const std::string& encoded);

}  // namespace monitor::health
