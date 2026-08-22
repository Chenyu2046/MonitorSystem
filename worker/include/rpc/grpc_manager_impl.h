#pragma once

/**
 * @file grpc_manager_impl.h
 * @brief Worker 兼容 pull RPC 的本地采集服务接口。
 *
 * Manager 主路径是 Worker push；该 service 保留 GetMonitorInfo，收到请求
 * 时直接调用 MetricCollector 采集一轮并返回 MonitorInfo，不经过发送队列。
 */

#include <grpcpp/support/status.h>

#include <memory>

#include "monitor/metric_collector.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

/** @brief 为兼容 pull 调用提供 Worker 本地实时采集。 */
class GrpcManagerImpl : public monitor::proto::GrpcManager::Service {
 public:
  GrpcManagerImpl();
  virtual ~GrpcManagerImpl();

  // SetMonitorInfo 已移除 - Server 端现在本地采集数据

  /** @brief 采集一轮基础指标并写入 RPC response。 */
  ::grpc::Status GetMonitorInfo(::grpc::ServerContext* context,
                                const ::google::protobuf::Empty* request,
                                ::monitor::proto::MonitorInfo* response);

 private:
  std::unique_ptr<MetricCollector> collector_;
};

}  // namespace monitor
