#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/probe_controller.h"
#include "monitor/metric_collector.h"

#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * 监控数据推送器
 *
 * Adaptive sampling (10 seconds in NORMAL) pushes metrics over gRPC.
 */
class MonitorPusher {
 public:
  /**
   * 构造函数
   * @param manager_address 管理者服务器地址（如 "192.168.1.100:50051"）
   * @param interval_seconds 推送间隔（秒），默认 10 秒
   */
  explicit MonitorPusher(const std::string& manager_address,
                         int interval_seconds = 10);
  ~MonitorPusher();

  // 启动推送线程
  void Start();

  // 停止推送
  void Stop();

  // 获取管理者地址
  const std::string& GetManagerAddress() const { return manager_address_; }

 private:
  void PushLoop();
  bool PushOnce();
  void WaitForNextSample();

  std::string manager_address_;
  int interval_seconds_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> thread_;
  std::unique_ptr<MetricCollector> collector_;
  std::unique_ptr<monitor::proto::GrpcManager::Stub> stub_;
  diagnostics::ObservabilityConfig observability_config_;
  diagnostics::AnomalyDetector anomaly_detector_;
  diagnostics::ObservabilityStateMachine state_machine_;
  diagnostics::ProbeController probe_controller_;
};

}  // namespace monitor
