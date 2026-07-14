#include "rpc/grpc_server.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include <google/protobuf/message.h>

namespace monitor {

namespace {
constexpr size_t kMaxHostIdentifierLength = 255;
constexpr size_t kMaxCachedHosts = 256;
constexpr size_t kMaxTelemetryItems = 256;
constexpr auto kHostDataTtl = std::chrono::seconds(60);

bool HasOnlyFiniteFloatingPoint(const google::protobuf::Message& message) {
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);
    const int count = field->is_repeated() ? reflection->FieldSize(message, field)
                                           : (reflection->HasField(message, field) ? 1 : 0);
    for (int index = 0; index < count; ++index) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT) {
        const float value = field->is_repeated()
                                ? reflection->GetRepeatedFloat(message, field, index)
                                : reflection->GetFloat(message, field);
        if (!std::isfinite(value)) return false;
      } else if (field->cpp_type() ==
                 google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE) {
        const double value = field->is_repeated()
                                 ? reflection->GetRepeatedDouble(message, field, index)
                                 : reflection->GetDouble(message, field);
        if (!std::isfinite(value)) return false;
      } else if (field->cpp_type() ==
                     google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
                 (field->is_repeated()
                      ? !HasOnlyFiniteFloatingPoint(
                            reflection->GetRepeatedMessage(message, field, index))
                      : !HasOnlyFiniteFloatingPoint(
                            reflection->GetMessage(message, field)))) {
        return false;
      }
    }
  }
  return true;
}
}

GrpcServerImpl::GrpcServerImpl(runtime_config::AuthorizationConfig authorization)
    : authorization_(std::move(authorization)) {}

bool GrpcServerImpl::IsAuthorizedWorker(
    const ::grpc::ServerContext& context, const std::string& hostname) const {
  if (authorization_.allow_insecure) return true;
  // 上报 hostname 同时是授权主体和状态主键；必须既在 allowlist 中，也实际
  // 出现在证书 SAN 中，避免多 SAN 证书借用某个允许 SAN 冒认另一个主机。
  if (std::find(authorization_.worker_sans.begin(),
                authorization_.worker_sans.end(), hostname) ==
      authorization_.worker_sans.end()) return false;
  return runtime_config::PeerHasAllowedSan(context, {hostname});
}

void GrpcServerImpl::PruneExpiredHostDataLocked(
    std::chrono::system_clock::time_point now) {
  for (auto it = host_data_.begin(); it != host_data_.end();) {
    if (now - it->second.timestamp > kHostDataTtl) {
      it = host_data_.erase(it);
    } else {
      ++it;
    }
  }
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
  // 先做常数时间的容量限制，再递归检查浮点值；避免异常大消息先消耗反射遍历 CPU。
  if (request->ByteSizeLong() > 256 * 1024 ||
      request->cpu_stat_size() > kMaxTelemetryItems ||
      request->net_info_size() > kMaxTelemetryItems ||
      request->disk_info_size() > kMaxTelemetryItems ||
      request->soft_irq_size() > kMaxTelemetryItems) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Monitor payload exceeds resource limits");
  }
  // 浮点 NaN/Inf 不能安全拼入现有数值 SQL，也没有有效的监控语义；在信任
  // 边界一次性递归拒绝所有嵌套遥测字段，避免遗漏新增的浮点指标。
  if (!HasOnlyFiniteFloatingPoint(*request)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Monitor payload contains non-finite values");
  }
  if (hostname.empty()) {
    hostname = request->name();
  }
  if (hostname.size() > kMaxHostIdentifierLength) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Host identifier is too long");
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
  if (ingest_result == HostManager::IngestResult::kResourceExhausted) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Manager host capacity reached");
  }
  if (ingest_result == HostManager::IngestResult::kOverloaded) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Manager ingest capacity is busy");
  }
  if (ingest_result != HostManager::IngestResult::kPersisted) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Monitor data persistence is unavailable");
  }

  {
    std::lock_guard<std::mutex> lock(mtx_);
    PruneExpiredHostDataLocked(std::chrono::system_clock::now());
    if (host_data_.find(hostname) == host_data_.end() &&
        host_data_.size() >= kMaxCachedHosts) {
      auto oldest = host_data_.begin();
      for (auto it = host_data_.begin(); it != host_data_.end(); ++it) {
        if (it->second.timestamp < oldest->second.timestamp) oldest = it;
      }
      host_data_.erase(oldest);
    }
    host_data_[hostname] = {*request, std::chrono::system_clock::now()};
  }

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
  PruneExpiredHostDataLocked(std::chrono::system_clock::now());
  if (!host_data_.empty()) {
    *response = host_data_.begin()->second.info;
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::NOT_FOUND,
                      "No fresh monitor data is available");
}

std::unordered_map<std::string, HostData> GrpcServerImpl::GetAllHostData() {
  std::lock_guard<std::mutex> lock(mtx_);
  PruneExpiredHostDataLocked(std::chrono::system_clock::now());
  return host_data_;
}

bool GrpcServerImpl::GetHostData(const std::string& hostname, HostData* data) {
  std::lock_guard<std::mutex> lock(mtx_);
  PruneExpiredHostDataLocked(std::chrono::system_clock::now());
  auto it = host_data_.find(hostname);
  if (it != host_data_.end()) {
    *data = it->second;
    return true;
  }
  return false;
}

}  // namespace monitor
