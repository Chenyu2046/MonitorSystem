#pragma once

/**
 * @file query_service.h
 * @brief Manager QueryService 的 protobuf RPC 到 C++ 查询转换层。
 *
 * 每个 handler 负责参数校验、调用 QueryManager/HostManager、分页元数据
 * 和 response message 转换；历史数据优先读 MySQL，诊断数据在持久化降级
 * 时回退到 HostManager 内存 IncidentStore。
 */

#include <memory>

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "host_manager.h"
#include "query_manager.h"

#include "query_api.grpc.pb.h"
#include "query_api.pb.h"

namespace monitor {

/** @brief 暴露性能、趋势、详细资源和 incident 查询 RPC。 */
class QueryServiceImpl : public monitor::proto::QueryService::Service {
 public:
  explicit QueryServiceImpl(QueryManager* query_manager,
                            HostManager* host_manager = nullptr);
  virtual ~QueryServiceImpl() = default;

  /** @brief QueryPerformance request -> 主表分页 response。 */
  ::grpc::Status QueryPerformance(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryPerformanceRequest* request,
      ::monitor::proto::QueryPerformanceResponse* response) override;

  /** @brief QueryTrend request -> 原始或时间桶聚合 response。 */
  ::grpc::Status QueryTrend(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryTrendRequest* request,
      ::monitor::proto::QueryTrendResponse* response) override;

  /** @brief QueryAnomaly request -> 历史异常分页 response。 */
  ::grpc::Status QueryAnomaly(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryAnomalyRequest* request,
      ::monitor::proto::QueryAnomalyResponse* response) override;

  /** @brief QueryScoreRank request -> score 排序 response。 */
  ::grpc::Status QueryScoreRank(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryScoreRankRequest* request,
      ::monitor::proto::QueryScoreRankResponse* response) override;

  /** @brief QueryLatestScore request -> 最新 server/cluster response。 */
  ::grpc::Status QueryLatestScore(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryLatestScoreRequest* request,
      ::monitor::proto::QueryLatestScoreResponse* response) override;

  /** @brief 查询网卡 detail 表并转换 response。 */
  ::grpc::Status QueryNetDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QueryNetDetailResponse* response) override;

  /** @brief 查询磁盘 detail 表并转换 response。 */
  ::grpc::Status QueryDiskDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QueryDiskDetailResponse* response) override;

  /** @brief 查询内存 detail 表并转换 response。 */
  ::grpc::Status QueryMemDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QueryMemDetailResponse* response) override;

  /** @brief 查询逐核 SoftIRQ detail 表并转换 response。 */
  ::grpc::Status QuerySoftIrqDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::QueryDetailRequest* request,
      ::monitor::proto::QuerySoftIrqDetailResponse* response) override;

  ::grpc::Status GetIncidents(
      ::grpc::ServerContext* context,
      const ::monitor::proto::GetIncidentsRequest* request,
      ::monitor::proto::GetIncidentsResponse* response) override;

  ::grpc::Status GetIncidentDetail(
      ::grpc::ServerContext* context,
      const ::monitor::proto::GetIncidentDetailRequest* request,
      ::monitor::proto::GetIncidentDetailResponse* response) override;

  ::grpc::Status GetActiveDiagnosis(
      ::grpc::ServerContext* context,
      const ::monitor::proto::GetActiveDiagnosisRequest* request,
      ::monitor::proto::GetActiveDiagnosisResponse* response) override;

 private:
  /** @brief 把 protobuf Timestamp 秒转换为 system_clock 时间范围。 */
  TimeRange ConvertTimeRange(const ::monitor::proto::TimeRange& proto_range);

  /** @brief 把 system_clock 时间点写入 protobuf Timestamp。 */
  void SetTimestamp(::google::protobuf::Timestamp* ts,
                    const std::chrono::system_clock::time_point& tp);
  void SetIncident(const diagnostics::IncidentRecord& incident,
                   ::monitor::proto::Incident* output);

  QueryManager* query_manager_;
  HostManager* host_manager_;
};

}  // namespace monitor
