#pragma once

/**
 * @file perf_log.h
 * @brief Lightweight, dependency-free performance logging helpers.
 *
 * The helpers are intentionally header-only: production binaries pay only an
 * environment check when performance output is disabled, while enabled paths
 * format one complete line before writing it to avoid interleaved fields from
 * Worker and Manager threads. Trace identity is derived from the existing
 * host/session/sequence fields and never enters the protobuf contract.
 */

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "canonical_host_key.h"

namespace monitor::perf {

struct Config {
  bool log_enabled = false;
  bool trace_enabled = false;
  bool health_trace_enabled = false;
  int slow_worker_collect_ms = 50;
  int slow_worker_rpc_ms = 500;
  int slow_manager_queue_ms = 100;
  int slow_manager_process_ms = 50;
  int slow_health_ms = 10;
  int slow_persist_queue_ms = 100;
  int slow_mysql_ms = 100;
  int summary_interval_sec = 10;
};

inline bool ParseFlag(const char* name, bool default_value) {
  const char* raw = std::getenv(name);
  if (!raw) return default_value;
  if (std::string_view(raw) == "1") return true;
  if (std::string_view(raw) == "0") return false;
  std::cerr << "[PERF][WARN] invalid " << name << "='" << raw
            << "', using " << (default_value ? "1" : "0") << std::endl;
  return default_value;
}

inline int ParsePositiveMilliseconds(const char* name, int default_value) {
  const char* raw = std::getenv(name);
  if (!raw) return default_value;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(raw, &end, 10);
  if (*raw == '\0' || end == raw || *end != '\0' || errno == ERANGE ||
      value <= 0 || value > INT_MAX) {
    std::cerr << "[PERF][WARN] invalid " << name << "='" << raw
              << "', using " << default_value << std::endl;
    return default_value;
  }
  return static_cast<int>(value);
}

inline int ParsePositiveSeconds(const char* name, int default_value) {
  const char* raw = std::getenv(name);
  if (!raw) return default_value;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(raw, &end, 10);
  if (*raw == '\0' || end == raw || *end != '\0' || errno == ERANGE ||
      value <= 0 || value > INT_MAX) {
    std::cerr << "[PERF][WARN] invalid " << name << "='" << raw
              << "', using " << default_value << std::endl;
    return default_value;
  }
  return static_cast<int>(value);
}

inline Config LoadConfig() {
  Config config;
  config.log_enabled = ParseFlag("MONITOR_PERF_LOG", false);
  config.trace_enabled = ParseFlag("MONITOR_PERF_TRACE", false);
  config.health_trace_enabled = ParseFlag("MONITOR_HEALTH_TRACE", false);
  config.slow_worker_collect_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_WORKER_COLLECT_MS", config.slow_worker_collect_ms);
  config.slow_worker_rpc_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_WORKER_RPC_MS", config.slow_worker_rpc_ms);
  config.slow_manager_queue_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_MANAGER_QUEUE_MS", config.slow_manager_queue_ms);
  config.slow_manager_process_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_MANAGER_PROCESS_MS", config.slow_manager_process_ms);
  config.slow_health_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_HEALTH_MS", config.slow_health_ms);
  config.slow_persist_queue_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_PERSIST_QUEUE_MS", config.slow_persist_queue_ms);
  config.slow_mysql_ms = ParsePositiveMilliseconds(
      "PERF_SLOW_MYSQL_MS", config.slow_mysql_ms);
  config.summary_interval_sec = ParsePositiveSeconds(
      "PERF_SUMMARY_INTERVAL_SEC", config.summary_interval_sec);
  return config;
}

inline const Config& GetConfig() {
  static const Config config = LoadConfig();
  return config;
}

inline bool PerfLogEnabled() { return GetConfig().log_enabled; }
inline bool PerfTraceEnabled() { return GetConfig().trace_enabled; }
inline bool HealthTraceEnabled() { return GetConfig().health_trace_enabled; }
inline bool OutputEnabled() {
  return PerfLogEnabled() || PerfTraceEnabled();
}

inline std::int64_t ElapsedUs(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now()) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

inline bool IsSlow(std::int64_t duration_us, int threshold_ms) {
  return duration_us >= static_cast<std::int64_t>(threshold_ms) * 1000;
}

inline std::string BuildTraceId(std::string_view host_name,
                               const proto::MonitorInfo& info) {
  std::string trace_id(host_name);
  if (trace_id.empty()) trace_id = "unknown";
  trace_id.push_back(':');
  trace_id.append(info.sample_session_id().empty() ? "legacy"
                                                   : info.sample_session_id());
  trace_id.push_back(':');
  trace_id.append(std::to_string(info.sample_sequence()));
  return trace_id;
}

inline std::string BuildTraceId(const proto::MonitorInfo& info) {
  return BuildTraceId(CanonicalHostKey(info), info);
}

inline void Emit(std::string_view prefix, std::string_view component,
                 std::string_view event, std::string_view trace_id,
                 std::string_view fields) {
  std::ostringstream line;
  const auto event_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  line << prefix << " component=" << component << " event=" << event;
  line << " event_time_ms=" << event_time_ms;
  if (!trace_id.empty()) line << " trace_id=" << trace_id;
  if (!fields.empty()) line << " " << fields;
  std::cerr << line.str() << std::endl;
}

inline void LogPerf(std::string_view component, std::string_view event,
                    std::string_view trace_id, std::string_view fields) {
  if (!OutputEnabled()) return;
  Emit("[PERF]", component, event, trace_id, fields);
}

template <typename Builder>
inline void LogPerf(std::string_view component, std::string_view event,
                    std::string_view trace_id, Builder&& builder) {
  if (!OutputEnabled()) return;
  Emit("[PERF]", component, event, trace_id,
       std::forward<Builder>(builder)());
}

template <typename Builder>
inline void LogSlow(std::string_view component, std::string_view event,
                    std::string_view trace_id, Builder&& builder) {
  Emit("[PERF][SLOW]", component, event, trace_id,
       std::forward<Builder>(builder)());
}

template <typename Builder>
inline void LogWarn(std::string_view component, std::string_view event,
                    std::string_view trace_id, Builder&& builder) {
  if (!OutputEnabled()) return;
  Emit("[PERF][WARN]", component, event, trace_id,
       std::forward<Builder>(builder)());
}

template <typename Builder>
inline void LogError(std::string_view component, std::string_view event,
                     std::string_view trace_id, Builder&& builder) {
  if (!OutputEnabled()) return;
  Emit("[PERF][ERROR]", component, event, trace_id,
       std::forward<Builder>(builder)());
}

template <typename Builder>
inline void LogHealth(std::string_view event, std::string_view trace_id,
                      Builder&& builder) {
  if (!HealthTraceEnabled()) return;
  Emit("[PERF]", "manager", event, trace_id,
       std::forward<Builder>(builder)());
}

}  // namespace monitor::perf
