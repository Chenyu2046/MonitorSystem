#pragma once

#include <string>

#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief Return the stable host identity shared by Worker and Manager.
 *
 * Preserve the legacy Manager persistence key: hostname_ip when both are
 * present, otherwise hostname, IP, then MonitorInfo.name as the final fallback.
 */
inline std::string CanonicalHostKey(const proto::MonitorInfo& info) {
  if (info.has_host_info()) {
    const auto& host = info.host_info();
    if (!host.hostname().empty() && !host.ip_address().empty()) {
      return host.hostname() + "_" + host.ip_address();
    }
    if (!host.hostname().empty()) return host.hostname();
    if (!host.ip_address().empty()) return host.ip_address();
  }
  return info.name();
}

}  // namespace monitor
