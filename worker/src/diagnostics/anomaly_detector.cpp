/**
 * @file anomaly_detector.cpp
 * @brief 基于阈值的 CPU/内存/磁盘/网络/调度异常评估实现。
 *
 * CPU、IOWait、SoftIRQ 和磁盘指标按逐核/逐设备最大值保留尖峰；网络
 * 包速率按接口求和。每个信号独立归一化，overall_score 取 max，最终
 * 只给状态机和 ProbeController 提供建议，不直接改变采样路径。
 */

#include "diagnostics/anomaly_detector.h"

#include "metric_semantics.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace monitor::diagnostics {
namespace {

/**
 * @brief 将 warning~critical 区间线性归一化到 0~1。
 *
 * warning 以下为 0，critical 以上为 1；阈值配置非法时返回 0，避免
 * 在异常配置下产生无意义的分数。
 */
double Normalize(double value, double warning, double critical) {
  if (critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

/**
 * @brief 追加一个信号并用其分数更新全局最大分数。
 *
 * triggered 采用 0.5 分界，仅作为信号展示标志；状态机实际使用
 * overall_score 和配置阈值。
 */
void AddSignal(AnomalyResult* result, AnomalyDomain domain, const char* metric,
               double value, double score) {
  result->signals.push_back(
      AnomalySignal{domain, metric, value, score, score >= 0.5});
  result->overall_score = std::max(result->overall_score, score);
}

}  // namespace

AnomalyDetector::AnomalyDetector(ObservabilityConfig config)
    : config_(std::move(config)) {}

AnomalyResult AnomalyDetector::Evaluate(
    const monitor::proto::MonitorInfo& info) const {
  // 一轮评估只读取 MonitorInfo，不修改输入，也不维护跨轮状态；连续
  // 样本和恢复样本由 ObservabilityStateMachine 处理。
  AnomalyResult result;

  double max_cpu = 0.0;
  double max_io_wait = 0.0;
  double max_softirq = 0.0;
  std::size_t valid_cpu_count = 0;
  // 异常路径取所有 CPU 核的最大值，避免 cpu0=95% 而其他核空闲时被
  // 主机平均值掩盖；普通概览的平均值与这里的 max-core 语义不同。
  for (const auto& cpu : info.cpu_stat()) {
    if (!cpu.sample_valid()) continue;
    max_cpu = std::max(max_cpu, static_cast<double>(cpu.cpu_percent()));
    max_io_wait =
        std::max(max_io_wait, static_cast<double>(cpu.io_wait_percent()));
    max_softirq =
        std::max(max_softirq, static_cast<double>(cpu.soft_irq_percent()));
    ++valid_cpu_count;
  }
  if (valid_cpu_count > 0) {
    AddSignal(&result, AnomalyDomain::kCpu, "cpu_percent", max_cpu,
              Normalize(max_cpu, config_.cpu_warning_percent,
                        config_.cpu_critical_percent));
    AddSignal(&result, AnomalyDomain::kCpu, "io_wait_percent", max_io_wait,
              Normalize(max_io_wait, config_.io_wait_warning_percent,
                        config_.io_wait_critical_percent));
    AddSignal(&result, AnomalyDomain::kCpu, "softirq_percent", max_softirq,
              Normalize(max_softirq, config_.softirq_warning_percent,
                        config_.softirq_critical_percent));
  }

  if (valid_cpu_count > 0 && info.has_cpu_load() &&
      info.cpu_load().sample_valid()) {
    const double load_per_cpu =
        info.cpu_load().load_avg_1() / valid_cpu_count;
    AddSignal(&result, AnomalyDomain::kScheduler, "load_per_cpu",
              load_per_cpu,
              Normalize(load_per_cpu, metric_semantics::kLoadPerCpu.warning,
                        metric_semantics::kLoadPerCpu.critical));
  }

  if (info.has_mem_info() && info.mem_info().sample_valid()) {
    const double used = info.mem_info().used_percent();
    AddSignal(&result, AnomalyDomain::kMemory, "memory_used_percent", used,
              Normalize(used, config_.memory_warning_percent,
                        config_.memory_critical_percent));
  }

  // 磁盘同样保留最忙设备/最高延迟，防止一块热点盘被其他空闲设备稀释。
  double max_disk_util = 0.0;
  double max_disk_latency = 0.0;
  std::size_t valid_disk_count = 0;
  for (const auto& disk : info.disk_info()) {
    if (!disk.sample_valid()) continue;
    ++valid_disk_count;
    max_disk_util = std::max(max_disk_util, disk.util_percent());
    max_disk_latency = std::max(
        max_disk_latency,
        std::max(disk.avg_read_latency_ms(), disk.avg_write_latency_ms()));
  }
  if (valid_disk_count > 0) {
    AddSignal(&result, AnomalyDomain::kDisk, "disk_util_percent", max_disk_util,
              Normalize(max_disk_util, config_.disk_util_warning_percent,
                        config_.disk_util_critical_percent));
    AddSignal(&result, AnomalyDomain::kDisk, "disk_latency_ms",
              max_disk_latency,
              Normalize(max_disk_latency, config_.disk_latency_warning_ms,
                        config_.disk_latency_critical_ms));
  }

  // 网络包速率是各接口收发包速率之和，表示主机整体包处理压力。
  double packets_per_second = 0.0;
  std::size_t valid_net_count = 0;
  for (const auto& net : info.net_info()) {
    if (!net.sample_valid()) continue;
    ++valid_net_count;
    packets_per_second += net.rcv_packets_rate() + net.send_packets_rate();
  }
  if (valid_net_count > 0) {
    AddSignal(&result, AnomalyDomain::kNetwork, "packets_per_second",
              packets_per_second,
              Normalize(packets_per_second, config_.network_pps_warning,
                        config_.network_pps_critical));
  }

  double net_softirq_per_second = 0.0;
  std::size_t valid_softirq_count = 0;
  for (const auto& softirq : info.soft_irq()) {
    if (!softirq.sample_valid()) continue;
    ++valid_softirq_count;
    net_softirq_per_second =
        std::max(net_softirq_per_second,
                 static_cast<double>(softirq.net_rx() + softirq.net_tx()));
  }
  if (valid_softirq_count > 0) {
    AddSignal(&result, AnomalyDomain::kNetwork, "net_softirq_per_second",
              net_softirq_per_second,
              Normalize(net_softirq_per_second, config_.softirq_warning_per_sec,
                        config_.softirq_critical_per_sec));
  }

  // 这里仅把分数映射为策略建议；连续样本、状态转换和具体 Probe 选择
  // 分别由状态机与 ProbeController 负责。
  result.should_diagnose = result.overall_score >= config_.suspect_enter_score;
  result.should_profile = result.overall_score >= config_.profiling_enter_score;
  return result;
}

}  // namespace monitor::diagnostics
