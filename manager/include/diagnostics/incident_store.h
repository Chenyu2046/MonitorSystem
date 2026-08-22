#pragma once

/**
 * @file incident_store.h
 * @brief Manager 内存 incident 状态、历史窗口和查询接口。
 *
 * IncidentStore 按 server_name 保存一个 active incident，并将关闭后的
 * 记录放入有界 history_。Observe() 是状态转换入口；所有读写由 mutex_
 * 保护，作为 MySQL 不可用时仍可用的快速诊断路径。
 */

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "diagnostics/evidence_builder.h"
#include "diagnostics/root_cause_engine.h"

namespace monitor::diagnostics {

/**
 * @brief 一次主机异常 episode 的状态、根因和证据快照。
 *
 * active=true 表示 episode 尚未关闭；end_time 在持续更新时仍表示最近
 * 观察时间。root_causes/evidence 是该时刻的规则结果和可追溯输入。
 */
struct IncidentRecord {
  std::uint64_t id = 0;
  std::string server_name;
  std::string severity;
  std::string state;
  bool active = true;
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;
  std::vector<RootCause> root_causes;
  std::vector<Evidence> evidence;
};

/** @brief 线程安全地跟踪 active incident 并提供历史/条件查询。 */
class IncidentStore {
 public:
  explicit IncidentStore(std::size_t max_history = 256);

  // 返回状态发生变化或 active incident 需要关闭时的当前记录；空值表示
  // 本轮没有新建或关闭可持久化的 incident。
  // Returns the current incident when the observation changed incident state.
  // An empty result means there was no active incident to close and no new
  // incident to persist.
  std::optional<IncidentRecord> Observe(
      const std::string& server_name, const std::string& state,
      const std::vector<Evidence>& evidence,
      const std::vector<RootCause>& root_causes,
      std::chrono::system_clock::time_point now);

  std::vector<IncidentRecord> List(
      const std::string& server_name = {},
      std::chrono::system_clock::time_point start_time =
          std::chrono::system_clock::time_point::min(),
      std::chrono::system_clock::time_point end_time =
          std::chrono::system_clock::time_point::max(),
      const std::string& root_cause = {},
      const std::string& severity = {}) const;

  std::optional<IncidentRecord> Get(std::uint64_t id) const;
  std::vector<IncidentRecord> Active(const std::string& server_name = {}) const;

 private:
  static bool Matches(const IncidentRecord& incident,
                      const std::string& server_name,
                      std::chrono::system_clock::time_point start_time,
                      std::chrono::system_clock::time_point end_time,
                      const std::string& root_cause,
                      const std::string& severity);
  void AddHistory(IncidentRecord incident);

  const std::size_t max_history_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_ = 1;
  std::unordered_map<std::string, IncidentRecord> active_;
  std::deque<IncidentRecord> history_;
};

}  // namespace monitor::diagnostics
