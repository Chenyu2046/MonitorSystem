#pragma once

/**
 * @file rpc_client.h
 * @brief Manager 侧兼容性 MonitorInfo 拉取客户端。
 */

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include <string>

#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/** 从远程 Worker/Manager 端拉取 MonitorInfo 的 gRPC 客户端。 */
class RpcClient {
 public:
  /** 创建到指定地址的非加密 gRPC channel。 */
  explicit RpcClient(const std::string& host_address = "localhost:50051");
  /** 释放 gRPC stub。 */
  ~RpcClient();

  // SetMonitorInfo 已移除 - Server 端现在本地采集数据

  /** 同步调用 GetMonitorInfo，成功时写入输出消息。 */
  bool GetMonitorInfo(monitor::proto::MonitorInfo* monitor_info);

  /** 返回构造时保存的目标地址。 */
  const std::string& GetHostAddress() const { return host_address_; }

 private:
  std::unique_ptr<monitor::proto::GrpcManager::Stub> stub_ptr_;
  std::string host_address_;
};

}  // namespace monitor
