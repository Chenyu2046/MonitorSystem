#pragma once

/**
 * @file grpc_server.h
 * @brief Manager gRPC 接收服务。
 *
 * Worker 通过 SetMonitorInfo push MonitorInfo；服务先调用 HostManager
 * callback 入有界 shard 队列。HostManager 承担正式处理、评分和持久化链路。
 */

#include <grpcpp/support/status.h>
#include <grpcpp/server_context.h>

#include <chrono>
#include <functional>
#include "data_receive_result.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/** @brief gRPC 接收后交给 HostManager 的入队回调。 */
using DataReceivedCallback =
    std::function<DataReceiveResult(
        const monitor::proto::MonitorInfo&,
        std::chrono::system_clock::time_point deadline,
        monitor::proto::MonitorFeedback*)>;

/** @brief Manager 接收 Worker push 的 gRPC service。 */
class GrpcServerImpl : public monitor::proto::GrpcManager::Service {
 public:
  GrpcServerImpl() = default;
  virtual ~GrpcServerImpl() = default;

  /** @brief 校验主机标识并提交 HostManager。 */
  ::grpc::Status SetMonitorInfo(::grpc::ServerContext* context,
                                const ::monitor::proto::MonitorInfo* request,
                                ::monitor::proto::MonitorFeedback* response)
      override;

  /** @brief 设置 Worker push 到 HostManager 的处理回调。 */
  void SetDataReceivedCallback(DataReceivedCallback callback) {
    callback_ = std::move(callback);
  }

 private:
  DataReceivedCallback callback_;
};

}  // namespace monitor
