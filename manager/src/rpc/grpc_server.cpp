/**
 * @file grpc_server.cpp
 * @brief Manager Worker-push gRPC handler。
 *
 * SetMonitorInfo 的边界是 protobuf -> DataReceiveResult：确保有界
 * HostManager 接收成功；队列满返回 RESOURCE_EXHAUSTED，停止中返回
 * UNAVAILABLE，避免 RPC 看见“已接受”但实际丢失的假成功。
 */

#include "rpc/grpc_server.h"

#include "canonical_host_key.h"
#include "perf/perf_log.h"

#include <iostream>

namespace monitor {

::grpc::Status GrpcServerImpl::SetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::monitor::proto::MonitorInfo* request,
    ::monitor::proto::MonitorFeedback* response) {
  const auto handler_start = std::chrono::steady_clock::now();
  // 这是 Worker -> Manager 的 push 边界；response 只表达接收结果，真正的
  // 评分/诊断/持久化在 HostManager 的异步 shard 链路中完成。
  if (!request) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Empty request");
  }

  const std::string hostname = CanonicalHostKey(*request);
  if (hostname.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing hostname");
  }

  // 先确认 Manager 已接受数据；队列满或停止时不报告假成功。
  DataReceiveResult result = DataReceiveResult::kAccepted;
  const auto callback_start = std::chrono::steady_clock::now();
  if (callback_) {
    result = callback_(*request, context->deadline(), response);
  }
  const auto callback_us = perf::ElapsedUs(callback_start);
  grpc::Status status = grpc::Status::OK;
  switch (result) {
    case DataReceiveResult::kAccepted:
      break;
    case DataReceiveResult::kQueueFull:
      status = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "manager queue full");
      break;
    case DataReceiveResult::kStopping:
      status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "manager stopping");
      break;
    case DataReceiveResult::kInvalidHost:
      status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid host identifier");
      break;
  }

  const auto handler_us = perf::ElapsedUs(handler_start);
  const bool slow = perf::IsSlow(
      handler_us, perf::GetConfig().slow_manager_process_ms);
  if (perf::OutputEnabled() || slow) {
    const auto trace_id = perf::BuildTraceId(hostname, *request);
    const auto fields = [&] {
      return "request_bytes=" + std::to_string(request->ByteSizeLong()) +
             " callback_us=" + std::to_string(callback_us) +
             " grpc_handler_us=" + std::to_string(handler_us) +
             " result=" + std::to_string(static_cast<int>(result)) +
             " health_valid=" +
             std::to_string(response && response->health_valid() ? 1 : 0);
    };
    if (perf::OutputEnabled()) {
      perf::LogPerf("manager", "grpc_handler", trace_id, fields);
    } else {
      perf::LogSlow("manager", "grpc_handler", trace_id, fields);
    }
  }

  if (!status.ok()) return status;

  std::cout << "Received monitor data from: " << hostname << std::endl;

  return grpc::Status::OK;
}

}  // namespace monitor
