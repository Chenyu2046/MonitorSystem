#include <cassert>
#include <cstdlib>

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
