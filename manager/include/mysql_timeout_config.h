#pragma once

/**
 * @file mysql_timeout_config.h
 * @brief MySQL 连接超时配置的解析和应用接口。
 *
 * 配置来自环境变量；未启用 MySQL 时仍保留纯配置解析接口，连接相关
 * API 通过 ENABLE_MYSQL 条件编译隔离第三方依赖。
 */

#include <optional>
#include <string>
#include <string_view>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

struct MysqlTimeoutConfig {
  /** 建立连接允许的最长秒数。 */
  unsigned int connect_timeout_sec = 5;
  /** 读取响应允许的最长秒数。 */
  unsigned int read_timeout_sec = 5;
  /** 写入请求允许的最长秒数。 */
  unsigned int write_timeout_sec = 5;
};

/** 将非空正整数文本解析为秒数，非法值返回空。 */
std::optional<unsigned int> ParseMysqlTimeoutSeconds(std::string_view value);
/** 从环境变量读取配置，并为缺失或非法值使用默认值。 */
MysqlTimeoutConfig GetMysqlTimeoutConfig();

#ifdef ENABLE_MYSQL
bool ApplyMysqlTimeouts(MYSQL* connection, const MysqlTimeoutConfig& config,
                        const char* connection_name);
#endif

}  // namespace monitor
