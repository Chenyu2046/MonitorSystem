/**
 * @file observability_overhead_test.cpp
 * @brief 以固定迭代次数比较 NORMAL/DIAGNOSTIC/PROFILING 控制循环开销。
 *
 * 测试覆盖 anomaly evaluate、状态机更新、队列入出队和 profiling payload
 * 的控制路径；输出是微基准观测，不把结果作为绝对性能承诺。
 */

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "diagnostics/observability_state.h"
#include "rpc/monitor_send_queue.h"

namespace {

/** @brief 构造指定状态和 profiling payload 的控制循环输入。 */
monitor::proto::MonitorInfo MakeInfo(monitor::proto::ObservabilityState state,
                                     bool with_profile) {
  monitor::proto::MonitorInfo info;
  info.set_name("overhead-test");
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(state == monitor::proto::OBSERVABILITY_NORMAL ? 10 : 90);
  cpu->set_io_wait_percent(
      state == monitor::proto::OBSERVABILITY_PROFILING ? 20 : 1);
  auto* diagnostic = info.mutable_diagnostic();
  diagnostic->set_state(state);
  diagnostic->set_anomaly_score(
      state == monitor::proto::OBSERVABILITY_NORMAL ? 0.1 : 0.9);
  if (with_profile) {
    auto* profile = diagnostic->add_oncpu_profiles();
    profile->set_pid(1);
    profile->set_tid(1);
    profile->set_samples(10);
  }
  return info;
}

/** @brief 重复运行一组控制操作并返回耗时微秒。 */
std::int64_t Measure(const monitor::proto::MonitorInfo& info,
                     std::size_t iterations) {
  monitor::diagnostics::ObservabilityConfig config;
  monitor::diagnostics::AnomalyDetector detector(config);
  monitor::diagnostics::ObservabilityStateMachine state_machine(config);
  monitor::MonitorSendQueue queue(iterations + 1, 8 * 1024 * 1024);
  queue.Open();

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    const auto anomaly = detector.Evaluate(info);
    state_machine.Update(anomaly);
    assert(queue.Push(info));
    monitor::proto::MonitorInfo output;
    assert(queue.Pop(&output));
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

int main() {
  constexpr std::size_t kIterations = 10000;
  const auto normal = Measure(
      MakeInfo(monitor::proto::OBSERVABILITY_NORMAL, false), kIterations);
  const auto diagnostic = Measure(
      MakeInfo(monitor::proto::OBSERVABILITY_DIAGNOSTIC, false), kIterations);
  const auto profiling = Measure(
      MakeInfo(monitor::proto::OBSERVABILITY_PROFILING, true), kIterations);
  std::cout << "control_loop_microbenchmark iterations=" << kIterations
            << " normal_us=" << normal
            << " diagnostic_us=" << diagnostic << " profiling_us=" << profiling
            << std::endl;
  return 0;
}
