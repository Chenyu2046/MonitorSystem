#include "health/top_signal_codec.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace monitor::health {
namespace {

bool ParseDouble(const std::string& text, double* output) {
  // 严格解析有限浮点数，拒绝空字段、溢出和尾随字符。
  if (!output || text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
      !std::isfinite(value)) {
    return false;
  }
  *output = value;
  return true;
}

bool ParseMetric(const std::string& name, MetricId* metric) {
  // 在受支持的指标集合中反向查找持久化文本对应的枚举值。
  constexpr std::array<MetricId, 15> kMetrics = {
      MetricId::kCpuAverage,
      MetricId::kCpuPeak,
      MetricId::kIoWait,
      MetricId::kSoftIrqPercent,
      MetricId::kLoadPerCpu,
      MetricId::kMemoryUsed,
      MetricId::kDiskUtil,
      MetricId::kDiskLatency,
      MetricId::kDiskIops,
      MetricId::kNetworkPps,
      MetricId::kNetworkRxThroughput,
      MetricId::kNetworkTxThroughput,
      MetricId::kNetworkDropsPerSec,
      MetricId::kNetworkErrorsPerSec,
      MetricId::kNetworkSoftIrqPerSec,
  };
  for (const auto candidate : kMetrics) {
    if (name == MetricName(candidate)) {
      *metric = candidate;
      return true;
    }
  }
  return false;
}

ModelState ParseModelState(const std::string& name) {
  // 解析模型状态；未知文本按最保守的冷启动状态处理。
  if (name == "READY") return ModelState::kReady;
  if (name == "WARMING") return ModelState::kWarming;
  return ModelState::kCold;
}

}  // namespace

std::string EncodeTopSignals(const std::vector<TopSignal>& signals) {
  // 将候选信号按稳定字段顺序编码为数据库可存储文本。
  std::ostringstream output;
  output << std::setprecision(17);
  for (std::size_t index = 0; index < signals.size(); ++index) {
    if (index != 0) output << ';';
    const auto& signal = signals[index];
    output << MetricName(signal.metric) << '|' << signal.value << '|'
           << (signal.detector.threshold_available ? 1 : 0) << '|'
           << signal.detector.threshold_score << '|'
           << signal.detector.mad_score << '|' << signal.detector.ewma_score
           << '|' << signal.detector.anomaly_votes << '|'
           << signal.detector.anomaly_score << '|'
           << (signal.detector.anomalous ? 1 : 0) << '|'
           << ModelStateName(signal.detector.model_state);
  }
  return output.str();
}

std::vector<TopSignal> DecodeTopSignals(const std::string& encoded) {
  // 解析信号文本；跳过格式错误记录，并限制最多恢复三个信号。
  std::vector<TopSignal> signals;
  std::istringstream records(encoded);
  std::string record;
  while (signals.size() < 3 && std::getline(records, record, ';')) {
    std::istringstream fields(record);
    std::array<std::string, 10> values;
    bool complete = true;
    for (auto& value : values) {
      if (!std::getline(fields, value, '|')) {
        complete = false;
        break;
      }
    }
    std::string extra;
    if (!complete || std::getline(fields, extra, '|')) continue;

    TopSignal signal;
    double threshold_available = 0.0;
    double votes = 0.0;
    double anomalous = 0.0;
    if (!ParseMetric(values[0], &signal.metric) ||
        !ParseDouble(values[1], &signal.value) ||
        !ParseDouble(values[2], &threshold_available) ||
        !ParseDouble(values[3], &signal.detector.threshold_score) ||
        !ParseDouble(values[4], &signal.detector.mad_score) ||
        !ParseDouble(values[5], &signal.detector.ewma_score) ||
        !ParseDouble(values[6], &votes) ||
        !ParseDouble(values[7], &signal.detector.anomaly_score) ||
        !ParseDouble(values[8], &anomalous)) {
      continue;
    }
    signal.detector.threshold_available = threshold_available == 1.0;
    signal.detector.anomaly_votes = static_cast<int>(votes);
    signal.detector.anomalous = anomalous == 1.0;
    signal.detector.model_state = ParseModelState(values[9]);
    signals.push_back(signal);
  }
  return signals;
}

}  // namespace monitor::health
