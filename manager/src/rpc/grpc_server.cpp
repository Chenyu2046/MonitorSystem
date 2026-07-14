#include "rpc/grpc_server.h"

#include <iostream>

namespace monitor {

GrpcServerImpl::GrpcServerImpl(runtime_config::AuthorizationConfig authorization)
    : authorization_(std::move(authorization)) {}

bool GrpcServerImpl::IsAuthorizedWorker(
    const ::grpc::ServerContext& context, const std::string& hostname) const {
  if (authorization_.allow_insecure) return true;
  if (!runtime_config::PeerHasAllowedSan(context, authorization_.worker_sans)) {
    return false;
  }
  return runtime_config::PeerHasAllowedSan(context, {hostname});
}

::grpc::Status GrpcServerImpl::SetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::monitor::proto::MonitorInfo* request,
    ::google::protobuf::Empty* response) {
  if (!request) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Empty request");
  }

  std::string hostname;
  if (request->has_host_info()) {
    hostname = request->host_info().hostname();
    if (hostname.empty() && !request->host_info().ip_address().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Host info with an IP address requires a hostname");
    }
    if (!hostname.empty() && !request->name().empty() &&
        request->name() != hostname) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "name and host_info.hostname must match");
    }
  }
  if (hostname.empty()) {
    hostname = request->name();
  }

  if (hostname.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing hostname");
  }
  if (!IsAuthorizedWorker(*context, hostname)) {
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "Worker certificate is not authorized for this host");
  }

  // 调用回调函数
  const auto ingest_result = callback_ ? callback_(*request)
                                       : HostManager::IngestResult::kFailed;
  if (ingest_result == HostManager::IngestResult::kCommitUnknown) {
    return grpc::Status(grpc::StatusCode::UNKNOWN,
                        "Monitor data commit outcome is unknown");
  }
  if (ingest_result != HostManager::IngestResult::kPersisted) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Monitor data persistence is unavailable");
  }

  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_data_[hostname] = {*request, std::chrono::system_clock::now()};
  }

  std::cout << "Received monitor data from: " << hostname << std::endl;

  return grpc::Status::OK;
}

::grpc::Status GrpcServerImpl::GetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::google::protobuf::Empty* request,
    ::monitor::proto::MonitorInfo* response) {
  if (!authorization_.allow_insecure &&
      !runtime_config::PeerHasAllowedSan(*context, authorization_.query_sans)) {
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "Query certificate is not authorized");
  }

  // 返回第一个主机的数据（或空）
  std::lock_guard<std::mutex> lock(mtx_);
  if (!host_data_.empty()) {
    *response = host_data_.begin()->second.info;
  }
  return grpc::Status::OK;
}

std::unordered_map<std::string, HostData> GrpcServerImpl::GetAllHostData() {
  std::lock_guard<std::mutex> lock(mtx_);
  return host_data_;
}

bool GrpcServerImpl::GetHostData(const std::string& hostname, HostData* data) {
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = host_data_.find(hostname);
  if (it != host_data_.end()) {
    *data = it->second;
    return true;
  }
  return false;
}

}  // namespace monitor
