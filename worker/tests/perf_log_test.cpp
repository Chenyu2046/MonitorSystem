#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "perf/perf_log.h"

int main() {
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
  std::cerr.rdbuf(old_buffer);

  setenv("PERF_SUMMARY_INTERVAL_SEC", "37", 1);
  assert(monitor::perf::LoadConfig().summary_interval_sec == 37);

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
