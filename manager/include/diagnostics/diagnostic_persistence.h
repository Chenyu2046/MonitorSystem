#pragma once

/**
 * @file diagnostic_persistence.h
 * @brief Diagnostic incident/evidence/root-cause 的 MySQL 持久化接口。
 *
 * Init 负责连接、超时和 schema；Save 在一个事务中写 incident 主表及
 * detail/root-cause 子表。ENABLE_MYSQL 未启用或连接失败时返回 false，
 * Manager 仍保留 IncidentStore 内存结果。
 */

#include <mutex>
#include <string>

#include "diagnostics/incident_store.h"

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor::diagnostics {

/**
 * @brief 拥有诊断 schema 并为每次 incident observation 写 durable snapshot。
 */
class DiagnosticPersistence {
 public:
  DiagnosticPersistence() = default;
  ~DiagnosticPersistence();

  DiagnosticPersistence(const DiagnosticPersistence&) = delete;
  DiagnosticPersistence& operator=(const DiagnosticPersistence&) = delete;

  /** @brief 建立 MySQL 连接、设置超时并确保诊断表存在。 */
  bool Init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database);
  /** @brief 关闭连接并清除 initialized 状态。 */
  void Close();
  /** @brief 返回当前持久化能力是否已初始化。 */
  bool IsInitialized() const;
  /** @brief 事务写入 incident 及其 evidence/root causes。 */
  bool Save(const IncidentRecord& incident);

 private:
#ifdef ENABLE_MYSQL
  bool EnsureSchema();
  bool Execute(const std::string& sql);
  std::string Escape(const std::string& value) const;
  std::string FormatTime(
      const std::chrono::system_clock::time_point& time) const;
  bool SaveEvidence(std::uint64_t incident_id,
                    const std::vector<Evidence>& evidence);
  bool SaveRootCauses(std::uint64_t incident_id,
                      const std::vector<RootCause>& root_causes);

  MYSQL* connection_ = nullptr;
#endif
  mutable std::mutex mutex_;
  bool initialized_ = false;
};

}  // namespace monitor::diagnostics
