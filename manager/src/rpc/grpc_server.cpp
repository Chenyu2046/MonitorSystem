/**
 * @file grpc_server.cpp
 * @brief Manager Worker-push gRPC handler。
 *
 * SetMonitorInfo 的边界是 protobuf -> DataReceiveResult：确保有界
 * HostManager 接收成功；队列满返回 RESOURCE_EXHAUSTED，停止中返回
 * UNAVAILABLE，避免 RPC 看见“已接受”但实际丢失的假成功。
 */

#include "rpc/grpc_server.h"

#include <iostream>

namespace monitor {

::grpc::Status GrpcServerImpl::SetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::monitor::proto::MonitorInfo* request,
    ::monitor::proto::MonitorFeedback* response) {
  // 这是 Worker -> Manager 的 push 边界；response 只表达接收结果，真正的
  // 评分/诊断/持久化在 HostManager 的异步 shard 链路中完成。
  if (!request) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Empty request");
  }

  std::string hostname = request->name();
  if (hostname.empty() && request->has_host_info()) {
    hostname = request->host_info().hostname();
  }

  if (hostname.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing hostname");
  }

  // 先确认 Manager 已接受数据；队列满或停止时不报告假成功。
  if (callback_) {
    const DataReceiveResult result =
        callback_(*request, context->deadline(), response);
    switch (result) {
      case DataReceiveResult::kAccepted:
        break;
      case DataReceiveResult::kQueueFull:
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "manager queue full");
      case DataReceiveResult::kStopping:
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "manager stopping");
      case DataReceiveResult::kInvalidHost:
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid host identifier");
    }
  }

  std::cout << "Received monitor data from: " << hostname << std::endl;

  return grpc::Status::OK;
}

}  // namespace monitor
