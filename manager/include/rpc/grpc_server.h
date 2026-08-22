#pragma once

/**
 * @file grpc_server.h
 * @brief Manager gRPC 接收服务和 Pull 兼容快照接口。
 *
 * Worker 通过 SetMonitorInfo push MonitorInfo；服务先调用 HostManager
 * callback 入有界 shard 队列，再更新 host_data_ 供保留的 GetMonitorInfo
 * pull 接口读取。host_data_ 由 mtx_ 保护，不能替代 HostManager 的主处理链。
 */

#include <grpcpp/support/status.h>
#include <grpcpp/server_context.h>

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include "data_receive_result.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/** @brief Pull 兼容路径保存的最新主机 protobuf 和接收时间。 */
struct HostData {
  monitor::proto::MonitorInfo info;
  std::chrono::system_clock::time_point timestamp;
};

/** @brief gRPC 接收后交给 HostManager 的入队回调。 */
using DataReceivedCallback =
    std::function<DataReceiveResult(const monitor::proto::MonitorInfo&)>;

/** @brief Manager 接收 Worker push 并提供兼容 pull 的 gRPC service。 */
class GrpcServerImpl : public monitor::proto::GrpcManager::Service {
 public:
  GrpcServerImpl() = default;
  virtual ~GrpcServerImpl() = default;

  /** @brief 校验主机标识、提交 HostManager 并更新 pull 快照。 */
  ::grpc::Status SetMonitorInfo(::grpc::ServerContext* context,
                                const ::monitor::proto::MonitorInfo* request,
                                ::google::protobuf::Empty* response) override;

  /** @brief 返回当前保留的第一个主机快照，兼容旧 pull 调用。 */
  ::grpc::Status GetMonitorInfo(::grpc::ServerContext* context,
                                const ::google::protobuf::Empty* request,
                                ::monitor::proto::MonitorInfo* response) override;

  /** @brief 设置 Worker push 到 HostManager 的处理回调。 */
  void SetDataReceivedCallback(DataReceivedCallback callback) {
    callback_ = std::move(callback);
  }

  /** @brief 在锁保护下复制全部 pull 快照。 */
  std::unordered_map<std::string, HostData> GetAllHostData();

  /** @brief 在锁保护下复制指定主机的 pull 快照。 */
  bool GetHostData(const std::string& hostname, HostData* data);

 private:
  std::mutex mtx_;
  std::unordered_map<std::string, HostData> host_data_;
  DataReceivedCallback callback_;
};

}  // namespace monitor
