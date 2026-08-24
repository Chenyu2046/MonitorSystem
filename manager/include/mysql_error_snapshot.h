#pragma once

#include <string>

namespace monitor {

/** @brief 保存 MySQL API 调用失败时的错误快照，避免后续 ROLLBACK 覆盖原错误。 */
struct MysqlErrorSnapshot {
  unsigned int failed_errno = 0;
  std::string failed_error;
  std::string failed_sqlstate;

  void Capture(unsigned int error_code, const char* error_message,
               const char* sqlstate) {
    failed_errno = error_code;
    failed_error = error_message ? error_message : "";
    failed_sqlstate = sqlstate ? sqlstate : "";
  }
};

}  // namespace monitor
