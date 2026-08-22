/**
 * @file mysql_timeout_config.cpp
 * @brief 实现 MySQL 超时环境变量解析和连接选项设置。
 */

#include "mysql_timeout_config.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <system_error>

namespace monitor {

std::optional<unsigned int> ParseMysqlTimeoutSeconds(std::string_view value) {
  // 使用 from_chars 做无 locale 的整数解析，并拒绝 0、溢出和尾随字符。
  if (value.empty()) {
    return std::nullopt;
  }
  unsigned long long parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
      parsed == 0 || parsed > std::numeric_limits<unsigned int>::max()) {
    return std::nullopt;
  }
  return static_cast<unsigned int>(parsed);
}

namespace {

unsigned int ReadTimeout(const char* name, unsigned int default_value) {
  const char* value = std::getenv(name);
  if (!value) {
    return default_value;
  }
  const auto parsed = ParseMysqlTimeoutSeconds(value);
  return parsed.value_or(default_value);
}

}  // namespace

MysqlTimeoutConfig GetMysqlTimeoutConfig() {
  // 三类超时分别读取，单个非法变量不会影响其他配置项。
  return MysqlTimeoutConfig{
      ReadTimeout("KERNSCOPE_MYSQL_CONNECT_TIMEOUT_SEC", 5),
      ReadTimeout("KERNSCOPE_MYSQL_READ_TIMEOUT_SEC", 5),
      ReadTimeout("KERNSCOPE_MYSQL_WRITE_TIMEOUT_SEC", 5)};
}

#ifdef ENABLE_MYSQL
bool ApplyMysqlTimeouts(MYSQL* connection, const MysqlTimeoutConfig& config,
                        const char* connection_name) {
  if (!connection) {
    return false;
  }
  // 在真正建立连接前逐项设置选项；任何一项失败都拒绝继续使用该连接。
  const auto apply = [&](enum mysql_option option, unsigned int value,
                         const char* option_name) {
    if (mysql_options(connection, option, &value) != 0) {
      std::cerr << connection_name << ": mysql_options(" << option_name
                << ") failed: " << mysql_error(connection) << std::endl;
      return false;
    }
    return true;
  };

  const bool applied =
      apply(MYSQL_OPT_CONNECT_TIMEOUT, config.connect_timeout_sec,
            "MYSQL_OPT_CONNECT_TIMEOUT") &&
      apply(MYSQL_OPT_READ_TIMEOUT, config.read_timeout_sec,
            "MYSQL_OPT_READ_TIMEOUT") &&
      apply(MYSQL_OPT_WRITE_TIMEOUT, config.write_timeout_sec,
            "MYSQL_OPT_WRITE_TIMEOUT");
  if (!applied) {
    std::cerr << connection_name
              << ": refusing connection because timeout options were not all applied"
              << std::endl;
  }
  return applied;
}
#endif

}  // namespace monitor
