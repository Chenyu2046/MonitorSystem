#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "health/health_types.h"
#include "health/metric_detector.h"
#include "health/rolling_window.h"
#include "monitor_info.pb.h"

namespace monitor::health {

struct HealthConfig {
  std::size_t max_samples = 60;
  std::chrono::seconds window_age{600};
  std::size_t min_samples = 30;
  double mad_warning_z = 3.5;
  double mad_critical_z = 6.0;
  double ewma_alpha = 0.2;
  double ewma_warning_sigma = 2.0;
  double ewma_critical_sigma = 4.0;
  int consensus_min_votes = 2;
  double cpu_domain_weight = 0.30;
  double memory_domain_weight = 0.20;
  double disk_domain_weight = 0.25;
  double network_domain_weight = 0.15;
  double scheduler_domain_weight = 0.10;
  std::chrono::seconds nar_window{300};
  std::size_t max_nar_frames = 600;
  std::chrono::seconds minimum_history_duration{300};

  // 检查健康评分参数的范围和相互关系。
  bool IsValid() const;
};

// 从环境变量加载健康评分配置，并在失败时写入错误原因。
bool LoadHealthConfigFromEnvironment(HealthConfig* config,
                                     std::string* error);

class HealthScoreEngine {
 public:
  using Clock = std::chrono::system_clock;
  using ActivityClock = std::chrono::steady_clock;

  // 创建评分引擎，并为每个指标初始化独立的历史窗口。
  explicit HealthScoreEngine(HealthConfig config = {});

  // 使用当前 Manager 活动时间计算一次健康评分。
  HealthResult Evaluate(const monitor::proto::MonitorInfo& info,
                        Clock::time_point timestamp, double resource_score);
  // 使用调用方提供的活动时间计算评分，便于低流量引擎过期清理。
  HealthResult Evaluate(const monitor::proto::MonitorInfo& info,
                        Clock::time_point timestamp, double resource_score,
                        ActivityClock::time_point activity_timestamp);
  // 返回最近一次有效评分对应的 Manager 活动时间。
  std::optional<ActivityClock::time_point> LastActivity() const {
    return last_activity_;
  }

 private:
  struct AnomalyFrame {
    Clock::time_point timestamp;
    std::size_t anomalous_metrics = 0;
    std::size_t valid_metrics = 0;
  };

  struct NetworkCounters {
    long double drops = 0.0;
    long double errors = 0.0;
    Clock::time_point timestamp;
  };

  HealthConfig config_;
  MetricDetector detector_;
  std::vector<RollingWindow> windows_;
  std::deque<AnomalyFrame> anomaly_history_;
  std::optional<NetworkCounters> network_counters_;
  std::optional<Clock::time_point> last_event_timestamp_;
  std::optional<ActivityClock::time_point> last_activity_;
};

// 移除超过 max_idle 未活动的主机引擎，并返回被移除的主机名。
std::vector<std::string> PruneStaleHealthEngines(
    std::unordered_map<std::string, HealthScoreEngine>* engines,
    HealthScoreEngine::ActivityClock::time_point now,
    HealthScoreEngine::ActivityClock::duration max_idle);

}  // namespace monitor::health
