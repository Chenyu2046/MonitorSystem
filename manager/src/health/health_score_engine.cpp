#include "health/health_score_engine.h"

#include "metric_semantics.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace monitor::health {
namespace {

constexpr std::size_t kMetricCount = 15;

struct Observation {
  MetricId metric;
  Domain domain;
  double value;
  std::optional<StaticThreshold> threshold;
};

std::size_t MetricIndex(MetricId metric) {
  return static_cast<std::size_t>(metric);
}

bool ValidPercent(double value) {
  return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

bool ValidNonnegative(double value) {
  return std::isfinite(value) && value >= 0.0;
}

const char* WorkerState(const monitor::proto::MonitorInfo& info) {
  if (!info.has_diagnostic()) return "UNKNOWN";
  switch (info.diagnostic().state()) {
    case monitor::proto::OBSERVABILITY_NORMAL:
      return "NORMAL";
    case monitor::proto::OBSERVABILITY_SUSPECT:
      return "SUSPECT";
    case monitor::proto::OBSERVABILITY_DIAGNOSTIC:
      return "DIAGNOSTIC";
    case monitor::proto::OBSERVABILITY_PROFILING:
      return "PROFILING";
    case monitor::proto::OBSERVABILITY_COOLDOWN:
      return "COOLDOWN";
  }
  return "UNKNOWN";
}

bool ParseSize(const char* name, std::size_t* output, std::string* error) {
  const char* value = std::getenv(name);
  if (!value) return true;
  const std::string_view text(value);
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    if (error) *error = std::string("invalid health config: ") + name;
    return false;
  }
  *output = parsed;
  return true;
}

bool ParseInt(const char* name, int* output, std::string* error) {
  const char* value = std::getenv(name);
  if (!value) return true;
  const std::string_view text(value);
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    if (error) *error = std::string("invalid health config: ") + name;
    return false;
  }
  *output = parsed;
  return true;
}

bool ParseDouble(const char* name, double* output, std::string* error) {
  const char* value = std::getenv(name);
  if (!value) return true;
  if (*value == '\0') {
    if (error) *error = std::string("invalid health config: ") + name;
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (errno == ERANGE || end == value || *end != '\0' ||
      !std::isfinite(parsed)) {
    if (error) *error = std::string("invalid health config: ") + name;
    return false;
  }
  *output = parsed;
  return true;
}

double DomainWeight(const HealthConfig& config, Domain domain) {
  switch (domain) {
    case Domain::kCpu:
      return config.cpu_domain_weight;
    case Domain::kMemory:
      return config.memory_domain_weight;
    case Domain::kDisk:
      return config.disk_domain_weight;
    case Domain::kNetwork:
      return config.network_domain_weight;
    case Domain::kScheduler:
      return config.scheduler_domain_weight;
  }
  return 0.0;
}

}  // namespace

bool HealthConfig::IsValid() const {
  const double weight_sum = cpu_domain_weight + memory_domain_weight +
                            disk_domain_weight + network_domain_weight +
                            scheduler_domain_weight;
  return max_samples >= min_samples && min_samples >= 10 &&
         window_age.count() > 0 && mad_warning_z >= 0.0 &&
         mad_critical_z > mad_warning_z && ewma_alpha > 0.0 &&
         ewma_alpha <= 1.0 && ewma_warning_sigma >= 0.0 &&
         ewma_critical_sigma > ewma_warning_sigma &&
         consensus_min_votes >= 1 && consensus_min_votes <= 3 &&
         cpu_domain_weight >= 0.0 && memory_domain_weight >= 0.0 &&
         disk_domain_weight >= 0.0 && network_domain_weight >= 0.0 &&
         scheduler_domain_weight >= 0.0 &&
         std::abs(weight_sum - 1.0) <= 1e-9 && nar_window.count() > 0 &&
         max_nar_frames > 0 && minimum_history_duration.count() > 0;
}

bool LoadHealthConfigFromEnvironment(HealthConfig* config,
                                     std::string* error) {
  if (!config) return false;
  std::size_t window_seconds = config->window_age.count();
  std::size_t nar_seconds = config->nar_window.count();
  std::size_t minimum_history_seconds =
      config->minimum_history_duration.count();
  if (!ParseSize("HEALTH_MAX_SAMPLES", &config->max_samples, error) ||
      !ParseSize("HEALTH_WINDOW_SEC", &window_seconds, error) ||
      !ParseSize("HEALTH_MIN_SAMPLES", &config->min_samples, error) ||
      !ParseDouble("MAD_WARNING_Z", &config->mad_warning_z, error) ||
      !ParseDouble("MAD_CRITICAL_Z", &config->mad_critical_z, error) ||
      !ParseDouble("EWMA_ALPHA", &config->ewma_alpha, error) ||
      !ParseDouble("EWMA_WARNING_SIGMA", &config->ewma_warning_sigma, error) ||
      !ParseDouble("EWMA_CRITICAL_SIGMA", &config->ewma_critical_sigma, error) ||
      !ParseInt("CONSENSUS_MIN_VOTES", &config->consensus_min_votes, error) ||
      !ParseDouble("CPU_DOMAIN_WEIGHT", &config->cpu_domain_weight, error) ||
      !ParseDouble("MEM_DOMAIN_WEIGHT", &config->memory_domain_weight, error) ||
      !ParseDouble("DISK_DOMAIN_WEIGHT", &config->disk_domain_weight, error) ||
      !ParseDouble("NET_DOMAIN_WEIGHT", &config->network_domain_weight, error) ||
      !ParseDouble("SCHEDULER_DOMAIN_WEIGHT",
                   &config->scheduler_domain_weight, error) ||
      !ParseSize("NAR_WINDOW_SEC", &nar_seconds, error) ||
      !ParseSize("HEALTH_NAR_MAX_FRAMES", &config->max_nar_frames, error) ||
      !ParseSize("HEALTH_MIN_HISTORY_SEC", &minimum_history_seconds, error)) {
    return false;
  }
  config->window_age = std::chrono::seconds(window_seconds);
  config->nar_window = std::chrono::seconds(nar_seconds);
  config->minimum_history_duration =
      std::chrono::seconds(minimum_history_seconds);
  if (!config->IsValid()) {
    if (error) *error = "invalid health config relationship";
    return false;
  }
  return true;
}

HealthScoreEngine::HealthScoreEngine(HealthConfig config)
    : config_(std::move(config)),
      detector_(DetectorConfig{config_.min_samples, config_.mad_warning_z,
                               config_.mad_critical_z, config_.ewma_alpha,
                               config_.ewma_warning_sigma,
                               config_.ewma_critical_sigma,
                               config_.consensus_min_votes,
                               config_.minimum_history_duration}) {
  windows_.reserve(kMetricCount);
  for (std::size_t index = 0; index < kMetricCount; ++index) {
    windows_.emplace_back(config_.max_samples, config_.window_age);
  }
}

HealthResult HealthScoreEngine::Evaluate(
    const monitor::proto::MonitorInfo& info, Clock::time_point timestamp,
    double resource_score) {
  return Evaluate(info, timestamp, resource_score, ActivityClock::now());
}

HealthResult HealthScoreEngine::Evaluate(
    const monitor::proto::MonitorInfo& info, Clock::time_point timestamp,
    double resource_score, ActivityClock::time_point activity_timestamp) {
  HealthResult result;
  result.resource_score = std::isfinite(resource_score) ? resource_score : 0.0;
  result.state = WorkerState(info);
  if (!config_.IsValid()) {
    return result;
  }

  // Event time drives the model. A wall-clock rollback is still a new sample
  // (ordering is enforced by Manager's session/sequence gate); retain the
  // latest model timestamp for non-negative window/rate deltas.
  const auto model_timestamp =
      last_event_timestamp_ && timestamp < *last_event_timestamp_
          ? *last_event_timestamp_
          : timestamp;

  std::vector<Observation> observations;
  observations.reserve(kMetricCount);

  double cpu_sum = 0.0;
  double cpu_peak = 0.0;
  double io_wait_peak = 0.0;
  double softirq_peak = 0.0;
  std::size_t cpu_count = 0;
  std::size_t io_wait_count = 0;
  std::size_t softirq_count = 0;
  for (const auto& cpu : info.cpu_stat()) {
    if (!cpu.sample_valid()) continue;
    if (ValidPercent(cpu.cpu_percent())) {
      cpu_sum += cpu.cpu_percent();
      cpu_peak = std::max(cpu_peak, static_cast<double>(cpu.cpu_percent()));
      ++cpu_count;
    }
    if (ValidPercent(cpu.io_wait_percent())) {
      io_wait_peak =
          std::max(io_wait_peak, static_cast<double>(cpu.io_wait_percent()));
      ++io_wait_count;
    }
    if (ValidPercent(cpu.soft_irq_percent())) {
      softirq_peak = std::max(
          softirq_peak, static_cast<double>(cpu.soft_irq_percent()));
      ++softirq_count;
    }
  }
  if (cpu_count > 0) {
    observations.push_back({MetricId::kCpuAverage, Domain::kCpu,
                            cpu_sum / cpu_count,
                            StaticThreshold{metric_semantics::kCpuPercent.warning,
                                            metric_semantics::kCpuPercent.critical}});
    observations.push_back(
        {MetricId::kCpuPeak, Domain::kCpu, cpu_peak,
         StaticThreshold{metric_semantics::kCpuPercent.warning,
                         metric_semantics::kCpuPercent.critical}});
  }
  if (io_wait_count > 0) {
    observations.push_back(
        {MetricId::kIoWait, Domain::kScheduler, io_wait_peak,
         StaticThreshold{metric_semantics::kIoWaitPercent.warning,
                         metric_semantics::kIoWaitPercent.critical}});
  }
  if (softirq_count > 0) {
    observations.push_back({MetricId::kSoftIrqPercent, Domain::kScheduler,
                            softirq_peak,
                            StaticThreshold{metric_semantics::kSoftIrqPercent.warning,
                                            metric_semantics::kSoftIrqPercent.critical}});
  }
  if (cpu_count > 0 && info.has_cpu_load() &&
      info.cpu_load().sample_valid() &&
      ValidNonnegative(info.cpu_load().load_avg_1())) {
    observations.push_back({MetricId::kLoadPerCpu, Domain::kScheduler,
                            info.cpu_load().load_avg_1() / cpu_count,
                            StaticThreshold{metric_semantics::kLoadPerCpu.warning,
                                            metric_semantics::kLoadPerCpu.critical}});
  }

  if (info.has_mem_info() && info.mem_info().sample_valid() &&
      ValidNonnegative(info.mem_info().total()) &&
      info.mem_info().total() > 0.0 &&
      ValidNonnegative(info.mem_info().avail()) &&
      info.mem_info().avail() <= info.mem_info().total()) {
    const double used = 100.0 *
                        (info.mem_info().total() - info.mem_info().avail()) /
                        info.mem_info().total();
    observations.push_back(
        {MetricId::kMemoryUsed, Domain::kMemory, used,
         StaticThreshold{metric_semantics::kMemoryPercent.warning,
                         metric_semantics::kMemoryPercent.critical}});
  }

  double disk_util = 0.0;
  double disk_latency = 0.0;
  double disk_iops = 0.0;
  std::size_t disk_util_count = 0;
  std::size_t disk_latency_count = 0;
  std::size_t disk_iops_count = 0;
  for (const auto& disk : info.disk_info()) {
    if (!disk.sample_valid()) continue;
    if (ValidPercent(disk.util_percent())) {
      disk_util = std::max(disk_util, disk.util_percent());
      ++disk_util_count;
    }
    if (ValidNonnegative(disk.avg_read_latency_ms()) &&
        ValidNonnegative(disk.avg_write_latency_ms())) {
      disk_latency = std::max(
          disk_latency,
          std::max(disk.avg_read_latency_ms(), disk.avg_write_latency_ms()));
      ++disk_latency_count;
    }
    if (ValidNonnegative(disk.read_iops()) &&
        ValidNonnegative(disk.write_iops())) {
      disk_iops =
          std::max(disk_iops, disk.read_iops() + disk.write_iops());
      ++disk_iops_count;
    }
  }
  if (disk_util_count > 0) {
    observations.push_back(
        {MetricId::kDiskUtil, Domain::kDisk, disk_util,
         StaticThreshold{metric_semantics::kDiskUtilPercent.warning,
                         metric_semantics::kDiskUtilPercent.critical}});
  }
  if (disk_latency_count > 0) {
    observations.push_back({MetricId::kDiskLatency, Domain::kDisk,
                            disk_latency,
                            StaticThreshold{metric_semantics::kDiskLatencyMs.warning,
                                            metric_semantics::kDiskLatencyMs.critical}});
  }
  if (disk_iops_count > 0) {
    observations.push_back(
        {MetricId::kDiskIops, Domain::kDisk, disk_iops, std::nullopt});
  }

  double network_pps = 0.0;
  double network_rx_kib = 0.0;
  double network_tx_kib = 0.0;
  std::size_t network_count = 0;
  std::size_t network_throughput_count = 0;
  long double drops = 0.0;
  long double errors = 0.0;
  for (const auto& net : info.net_info()) {
    if (!net.sample_valid()) continue;
    if (ValidNonnegative(net.rcv_packets_rate()) &&
        ValidNonnegative(net.send_packets_rate())) {
      network_pps += net.rcv_packets_rate() + net.send_packets_rate();
      ++network_count;
    }
    if (ValidNonnegative(net.rcv_rate()) && ValidNonnegative(net.send_rate())) {
      network_rx_kib += net.rcv_rate();
      network_tx_kib += net.send_rate();
      ++network_throughput_count;
    }
    drops += static_cast<long double>(net.drop_in()) + net.drop_out();
    errors += static_cast<long double>(net.err_in()) + net.err_out();
  }
  if (network_count > 0 && std::isfinite(network_pps)) {
    observations.push_back({MetricId::kNetworkPps, Domain::kNetwork,
                            network_pps,
                            StaticThreshold{metric_semantics::kNetworkPps.warning,
                                            metric_semantics::kNetworkPps.critical}});
    if (network_throughput_count > 0 && std::isfinite(network_rx_kib) &&
        std::isfinite(network_tx_kib)) {
      observations.push_back({MetricId::kNetworkRxThroughput,
                              Domain::kNetwork, network_rx_kib, std::nullopt});
      observations.push_back({MetricId::kNetworkTxThroughput,
                              Domain::kNetwork, network_tx_kib, std::nullopt});
    }
  }
  if (network_count > 0) {
    if (network_counters_ && model_timestamp > network_counters_->timestamp &&
        drops >= network_counters_->drops &&
        errors >= network_counters_->errors) {
      const double seconds = std::chrono::duration<double>(
                                 model_timestamp - network_counters_->timestamp)
                                 .count();
      const double drop_rate =
          static_cast<double>((drops - network_counters_->drops) / seconds);
      const double error_rate =
          static_cast<double>((errors - network_counters_->errors) / seconds);
      if (ValidNonnegative(drop_rate)) {
        observations.push_back({MetricId::kNetworkDropsPerSec,
                                Domain::kNetwork, drop_rate,
                                StaticThreshold{1.0, 100.0}});
      }
      if (ValidNonnegative(error_rate)) {
        observations.push_back({MetricId::kNetworkErrorsPerSec,
                                Domain::kNetwork, error_rate,
                                StaticThreshold{1.0, 100.0}});
      }
    }
    if (!network_counters_ || model_timestamp > network_counters_->timestamp) {
      network_counters_ = NetworkCounters{drops, errors, model_timestamp};
    }
  }

  double network_softirq = 0.0;
  std::size_t network_softirq_count = 0;
  for (const auto& softirq : info.soft_irq()) {
    if (!softirq.sample_valid()) continue;
    if (ValidNonnegative(softirq.net_rx()) &&
        ValidNonnegative(softirq.net_tx())) {
      network_softirq = std::max(network_softirq,
                                 static_cast<double>(softirq.net_rx()) +
                                     softirq.net_tx());
      ++network_softirq_count;
    }
  }
  if (network_softirq_count > 0 && std::isfinite(network_softirq)) {
    observations.push_back({MetricId::kNetworkSoftIrqPerSec,
                            Domain::kNetwork, network_softirq,
                            StaticThreshold{metric_semantics::kNetworkSoftIrqPerSec.warning,
                                            metric_semantics::kNetworkSoftIrqPerSec.critical}});
  }

  std::array<double, 5> domain_scores{};
  std::array<bool, 5> domain_valid{};
  std::size_t anomalous_metrics = 0;
  result.model_state = ModelState::kReady;
  for (const auto& observation : observations) {
    const std::size_t index = MetricIndex(observation.metric);
    windows_[index].Prune(model_timestamp);
    DetectorResult detector = detector_.Evaluate(
        observation.value, observation.threshold, windows_[index],
        model_timestamp);
    windows_[index].Push(observation.value, model_timestamp);
    const auto domain = static_cast<std::size_t>(observation.domain);
    domain_scores[domain] =
        std::max(domain_scores[domain], detector.anomaly_score);
    domain_valid[domain] = true;
    if (detector.anomalous) ++anomalous_metrics;
    if (static_cast<int>(detector.model_state) <
        static_cast<int>(result.model_state)) {
      result.model_state = detector.model_state;
    }
    result.top_signals.push_back(
        TopSignal{observation.metric, observation.value, detector});
  }

  if (observations.empty()) {
    result.model_state = ModelState::kCold;
    return result;
  }
  result.confidence =
      static_cast<double>(observations.size()) / kMetricCount;
  result.cpu_score = domain_scores[static_cast<std::size_t>(Domain::kCpu)];
  result.memory_score =
      domain_scores[static_cast<std::size_t>(Domain::kMemory)];
  result.disk_score = domain_scores[static_cast<std::size_t>(Domain::kDisk)];
  result.network_score =
      domain_scores[static_cast<std::size_t>(Domain::kNetwork)];
  result.scheduler_score =
      domain_scores[static_cast<std::size_t>(Domain::kScheduler)];

  double weighted_score = 0.0;
  double available_weight = 0.0;
  for (std::size_t index = 0; index < domain_scores.size(); ++index) {
    if (!domain_valid[index]) continue;
    const double weight = DomainWeight(config_, static_cast<Domain>(index));
    weighted_score += domain_scores[index] * weight;
    available_weight += weight;
  }
  result.anomaly_score =
      available_weight > 0.0 ? weighted_score / available_weight : 0.0;
  result.anomaly_score = std::clamp(result.anomaly_score, 0.0, 1.0);
  result.remote_trigger_score = 0.0;
  for (std::size_t index = 0; index < domain_scores.size(); ++index) {
    if (domain_valid[index]) {
      result.remote_trigger_score =
          std::max(result.remote_trigger_score, domain_scores[index]);
    }
  }
  result.health_score = 100.0 * (1.0 - result.anomaly_score);

  anomaly_history_.push_back(
      {model_timestamp, anomalous_metrics, observations.size()});
  while (!anomaly_history_.empty() &&
         model_timestamp - anomaly_history_.front().timestamp >
             config_.nar_window) {
    anomaly_history_.pop_front();
  }
  while (anomaly_history_.size() > config_.max_nar_frames) {
    anomaly_history_.pop_front();
  }
  std::size_t anomaly_bits = 0;
  std::size_t valid_bits = 0;
  for (const auto& frame : anomaly_history_) {
    anomaly_bits += frame.anomalous_metrics;
    valid_bits += frame.valid_metrics;
  }
  result.anomaly_rate_5m =
      valid_bits == 0 ? 0.0
                      : static_cast<double>(anomaly_bits) / valid_bits;

  std::sort(result.top_signals.begin(), result.top_signals.end(),
            [](const TopSignal& left, const TopSignal& right) {
              return left.detector.anomaly_score >
                     right.detector.anomaly_score;
            });
  if (result.top_signals.size() > 3) result.top_signals.resize(3);
  result.valid = std::isfinite(result.health_score) &&
                 std::isfinite(result.anomaly_score) &&
                 std::isfinite(result.anomaly_rate_5m) &&
                 std::isfinite(result.confidence);
  if (result.valid) {
    last_event_timestamp_ = model_timestamp;
    last_activity_ = activity_timestamp;
  }
  return result;
}

std::vector<std::string> PruneStaleHealthEngines(
    std::unordered_map<std::string, HealthScoreEngine>* engines,
    HealthScoreEngine::ActivityClock::time_point now,
    HealthScoreEngine::ActivityClock::duration max_idle) {
  std::vector<std::string> removed;
  if (!engines || max_idle.count() <= 0) return removed;
  for (auto it = engines->begin(); it != engines->end();) {
    const auto last_seen = it->second.LastActivity();
    if (!last_seen || (now >= *last_seen && now - *last_seen > max_idle)) {
      removed.push_back(it->first);
      it = engines->erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

}  // namespace monitor::health
