#pragma once

/**
 * @file monitor_pusher.h
 * @brief Worker 采集、异常判断、诊断控制、队列和 gRPC 上报总编排接口。
 *
 * 主流程为：MetricCollector -> AnomalyDetector -> StateMachine ->
 * ProbeController -> DiagnosticSnapshot/Protobuf -> MonitorSendQueue ->
 * gRPC SetMonitorInfo。采集线程和发送线程分离，队列负责背压与优先级。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/probe_controller.h"
#include "diagnostics/symbolizer.h"
#include "monitor/metric_collector.h"
#include "rpc/monitor_send_queue.h"

#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief Worker 端周期采集和异步上报控制器。
 *
 * PushLoop 只负责采集/诊断/入队，SendLoop 负责带 deadline 和有限重试的
 * gRPC 发送；Stop() 通过 running_、condition_variable 和 join 保证两个
 * 后台线程在对象析构前退出。
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

  /** @brief 打开发送队列并启动采集线程、发送线程。 */
  bool Start();

  /** @brief 停止采集、关闭队列并等待发送线程退出。 */
  void Stop();

  // 获取管理者地址
  const std::string& GetManagerAddress() const { return manager_address_; }

 private:
  /** @brief 周期执行 PushOnce，并按状态机选择下一轮间隔。 */
  void PushLoop();
  /** @brief 从队列取消息并串行执行 gRPC 发送/重试。 */
  void SendLoop();
  /** @brief 执行一次完整采集、诊断、protobuf 填充和入队。 */
  bool PushOnce();
  /** @brief 用可中断等待实现自适应采样间隔。 */
  void WaitForNextSample();
  /** @brief 按配置 deadline/retry policy 发送一条 MonitorInfo。 */
  bool SendWithRetry(const monitor::proto::MonitorInfo& info);
  /** @brief 在 retry backoff 期间等待，Stop() 时提前唤醒。 */
  bool WaitForRetry(std::chrono::milliseconds delay);
  /** @brief 判断 gRPC 状态是否属于可安全重试的瞬态错误。 */
  static bool IsRetryable(const grpc::Status& status);

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
  diagnostics::Symbolizer symbolizer_;
  MonitorSendQueue send_queue_;
  std::unique_ptr<std::thread> sender_thread_;
  std::mutex lifecycle_mutex_;
  std::mutex stop_mutex_;
  std::condition_variable stop_condition_;
};

}  // namespace monitor
