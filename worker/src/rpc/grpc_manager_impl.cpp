/**
 * @file grpc_manager_impl.cpp
 * @brief Worker 兼容 pull RPC 的采集和 response 填充实现。
 */

#include "rpc/grpc_manager_impl.h"

#include <iostream>

namespace monitor {

GrpcManagerImpl::GrpcManagerImpl() {
  collector_ = std::make_unique<MetricCollector>();
}

GrpcManagerImpl::~GrpcManagerImpl() {}

// SetMonitorInfo 已移除 - Server 端现在本地采集数据

::grpc::Status GrpcManagerImpl::GetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::google::protobuf::Empty* request,
    ::monitor::proto::MonitorInfo* response) {
  // Pull 路径直接运行基础采集器；诊断状态机和发送队列只属于 push
  // MonitorPusher，不在此 RPC 中复用或修改。
  collector_->CollectAll(response);
  return grpc::Status::OK;
}

}  // namespace monitor
