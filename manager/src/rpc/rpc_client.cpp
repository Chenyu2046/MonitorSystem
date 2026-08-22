/**
 * @file rpc_client.cpp
 * @brief 实现兼容性 MonitorInfo 拉取 RPC。
 */

#include "rpc/rpc_client.h"

#include <iostream>

namespace monitor {

RpcClient::RpcClient(const std::string& host_address)
    : host_address_(host_address) {
  // 客户端只建立 channel/stub，具体 RPC 在 GetMonitorInfo 中同步执行。
  auto channel =
      grpc::CreateChannel(host_address, grpc::InsecureChannelCredentials());
  stub_ptr_ = monitor::proto::GrpcManager::NewStub(channel);
}

RpcClient::~RpcClient() {}

// SetMonitorInfo 已移除 - Server 端现在本地采集数据

bool RpcClient::GetMonitorInfo(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return false;
  }

  // 每次调用使用独立 context 和空请求，避免跨调用复用 RPC 状态。
  ::grpc::ClientContext context;
  ::google::protobuf::Empty request;

  ::grpc::Status status =
      stub_ptr_->GetMonitorInfo(&context, request, monitor_info);

  if (status.ok()) {
    return true;
  } else {
    std::cerr << "Failed to get monitor info from " << host_address_ << ": "
              << status.error_message() << std::endl;
    return false;
  }
}

}  // namespace monitor
