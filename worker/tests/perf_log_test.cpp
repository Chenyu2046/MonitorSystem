#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "perf/perf_log.h"

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "log_gate") {
    setenv("MONITOR_PERF_LOG", "1", 1);
    unsetenv("MONITOR_PERF_TRACE");
    std::ostringstream captured;
    auto* old_buffer = std::cerr.rdbuf(captured.rdbuf());
    if (monitor::perf::PerfTraceEnabled()) {
      monitor::perf::LogPerf(
          "worker", "high_frequency", "hostA:abc:1",
          [] { return std::string("must_not_print"); });
    }
    monitor::perf::LogError("manager", "mysql_failure", "hostA:abc:1",
                            [] { return std::string("stage=connect"); });
    std::cerr.rdbuf(old_buffer);
    assert(captured.str().find("high_frequency") == std::string::npos);
    assert(captured.str().find("[PERF][ERROR]") != std::string::npos);
    assert(captured.str().find("event=mysql_failure") != std::string::npos);
    assert(captured.str().find("trace_id=hostA:abc:1") != std::string::npos);
    assert(captured.str().find("stage=connect") != std::string::npos);
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "trace_gate") {
    unsetenv("MONITOR_PERF_LOG");
    setenv("MONITOR_PERF_TRACE", "1", 1);
    std::ostringstream captured;
    auto* old_buffer = std::cerr.rdbuf(captured.rdbuf());
    if (monitor::perf::PerfTraceEnabled()) {
      monitor::perf::LogPerf(
          "worker", "high_frequency", "hostA:abc:1",
          [] { return std::string("duration_us=1"); });
    }
    std::cerr.rdbuf(old_buffer);
    assert(captured.str().find("high_frequency") != std::string::npos);
    assert(captured.str().find("trace_id=hostA:abc:1") != std::string::npos);
    return 0;
  }

  monitor::proto::MonitorInfo info;
  info.set_name("hostA");
  info.set_sample_session_id("abc");
  info.set_sample_sequence(100);
  assert(monitor::perf::BuildTraceId(info) == "hostA:abc:100");

  info.clear_sample_session_id();
  assert(monitor::perf::BuildTraceId(info) == "hostA:legacy:100");
  assert(monitor::perf::IsSlow(1001, 1));
  assert(!monitor::perf::IsSlow(999, 1));

  unsetenv("MONITOR_PERF_LOG");
  unsetenv("MONITOR_PERF_TRACE");
  unsetenv("MONITOR_HEALTH_TRACE");
  std::ostringstream captured;
  auto* old_buffer = std::cerr.rdbuf(captured.rdbuf());
  monitor::perf::LogSlow(
      "worker", "slow_test", "hostA:abc:100",
      [] { return std::string("duration_us=60000"); });
  assert(captured.str().find("[PERF][SLOW]") != std::string::npos);
  assert(captured.str().find("trace_id=hostA:abc:100") !=
         std::string::npos);
  assert(captured.str().find("event_time_ms=") != std::string::npos);

  bool builder_called = false;
  monitor::perf::LogPerf("worker", "disabled_test", "hostA:abc:100", [&] {
    builder_called = true;
    return std::string("must_not_be_built");
  });
  assert(!builder_called);
  assert(captured.str().find("disabled_test") == std::string::npos);
  monitor::perf::LogError("manager", "error_test", "hostA:abc:100",
                          [] { return std::string("stage=connect"); });
  assert(captured.str().find("[PERF][ERROR]") != std::string::npos);
  assert(captured.str().find("event=error_test") != std::string::npos);
  assert(captured.str().find("trace_id=hostA:abc:100") != std::string::npos);
  assert(captured.str().find("stage=connect") != std::string::npos);
  std::cerr.rdbuf(old_buffer);

  setenv("PERF_SUMMARY_INTERVAL_SEC", "37", 1);
  setenv("PERF_SLOW_WORKER_QUEUE_MS", "17", 1);
  setenv("PERF_SLOW_MANAGER_QUEUE_MS", "99", 1);
  const auto thresholds = monitor::perf::LoadConfig();
  assert(thresholds.summary_interval_sec == 37);
  assert(thresholds.slow_worker_queue_ms == 17);
  assert(thresholds.slow_manager_queue_ms == 99);

  setenv("MONITOR_PERF_LOG", "invalid", 1);
  setenv("PERF_SLOW_HEALTH_MS", "invalid", 1);
  const auto config = monitor::perf::LoadConfig();
  assert(!config.log_enabled);
  assert(config.slow_health_ms == 10);

  setenv("MONITOR_PERF_LOG", "1", 1);
  setenv("MONITOR_PERF_TRACE", "1", 1);
  const auto enabled = monitor::perf::LoadConfig();
  assert(enabled.log_enabled);
  assert(enabled.trace_enabled);
  return 0;
}
