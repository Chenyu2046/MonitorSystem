#pragma once

#include <optional>
#include <string>
#include <string_view>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

struct MysqlTimeoutConfig {
  unsigned int connect_timeout_sec = 5;
  unsigned int read_timeout_sec = 5;
  unsigned int write_timeout_sec = 5;
};

std::optional<unsigned int> ParseMysqlTimeoutSeconds(std::string_view value);
MysqlTimeoutConfig GetMysqlTimeoutConfig();

#ifdef ENABLE_MYSQL
bool ApplyMysqlTimeouts(MYSQL* connection, const MysqlTimeoutConfig& config,
                        const char* connection_name);
#endif

}  // namespace monitor
