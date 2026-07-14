#include "rpc/query_service.h"
#include "query_limits.h"

#include <cmath>
#include <ctime>
#include <iostream>

namespace monitor {

namespace {
constexpr int kMinTrendIntervalSeconds = 300;
}  // namespace

QueryServiceImpl::QueryServiceImpl(
    QueryManager* query_manager, runtime_config::AuthorizationConfig authorization)
    : query_manager_(query_manager), authorization_(std::move(authorization)) {}

::grpc::Status QueryServiceImpl::AuthorizeQuery(
    const ::grpc::ServerContext& context) const {
  if (authorization_.allow_insecure ||
      runtime_config::PeerHasAllowedSan(context, authorization_.query_sans)) {
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                      "Query certificate is not authorized");
}

bool QueryServiceImpl::ConvertTimeRange(
    const ::monitor::proto::TimeRange& proto_range, TimeRange* range) {
  constexpr int64_t kMinTimestampSeconds = -62135596800LL;
  constexpr int64_t kMaxTimestampSeconds = 253402300799LL;
  const auto valid = [&](const google::protobuf::Timestamp& timestamp) {
    if (timestamp.seconds() < kMinTimestampSeconds ||
        timestamp.seconds() > kMaxTimestampSeconds || timestamp.nanos() != 0) {
      return false;
    }
    // 当前 MySQL DATETIME 查询只保留秒精度，因此拒绝非零纳秒；protobuf 可表达
    // 的年份范围也大于部分 C++ system_clock，需通过往返转换确认不会回绕。
    const std::time_t seconds = static_cast<std::time_t>(timestamp.seconds());
    if (static_cast<int64_t>(seconds) != timestamp.seconds()) return false;
    return std::chrono::system_clock::to_time_t(
               std::chrono::system_clock::from_time_t(seconds)) == seconds;
  };
  if (!range || !valid(proto_range.start_time()) || !valid(proto_range.end_time())) {
    return false;
  }
  range->start_time = std::chrono::system_clock::from_time_t(
      proto_range.start_time().seconds());
  range->end_time =
      std::chrono::system_clock::from_time_t(proto_range.end_time().seconds());
  return true;
}

void QueryServiceImpl::SetTimestamp(
    ::google::protobuf::Timestamp* ts,
    const std::chrono::system_clock::time_point& tp) {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                     tp.time_since_epoch())
                     .count();
  ts->set_seconds(seconds);
  ts->set_nanos(0);
}

::grpc::Status QueryServiceImpl::QueryPerformance(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryPerformanceRequest* request,
    ::monitor::proto::QueryPerformanceResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  // 验证时间范围
  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QueryPerformance(
      request->server_name(), time_range, page, page_size, &total_count,
      &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Performance query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_cpu_percent(rec.cpu_percent);
    proto_rec->set_usr_percent(rec.usr_percent);
    proto_rec->set_system_percent(rec.system_percent);
    proto_rec->set_nice_percent(rec.nice_percent);
    proto_rec->set_idle_percent(rec.idle_percent);
    proto_rec->set_io_wait_percent(rec.io_wait_percent);
    proto_rec->set_irq_percent(rec.irq_percent);
    proto_rec->set_soft_irq_percent(rec.soft_irq_percent);
    proto_rec->set_load_avg_1(rec.load_avg_1);
    proto_rec->set_load_avg_3(rec.load_avg_3);
    proto_rec->set_load_avg_15(rec.load_avg_15);
    proto_rec->set_mem_used_percent(rec.mem_used_percent);
    proto_rec->set_mem_total(rec.mem_total);
    proto_rec->set_mem_free(rec.mem_free);
    proto_rec->set_mem_avail(rec.mem_avail);
    proto_rec->set_disk_util_percent(rec.disk_util_percent);
    proto_rec->set_send_rate(rec.send_rate);
    proto_rec->set_rcv_rate(rec.rcv_rate);
    proto_rec->set_score(rec.score);
    proto_rec->set_cpu_percent_rate(rec.cpu_percent_rate);
    proto_rec->set_usr_percent_rate(rec.usr_percent_rate);
    proto_rec->set_system_percent_rate(rec.system_percent_rate);
    proto_rec->set_io_wait_percent_rate(rec.io_wait_percent_rate);
    proto_rec->set_mem_used_percent_rate(rec.mem_used_percent_rate);
    proto_rec->set_disk_util_percent_rate(rec.disk_util_percent_rate);
    proto_rec->set_load_avg_1_rate(rec.load_avg_1_rate);
    proto_rec->set_load_avg_3_rate(rec.load_avg_3_rate);
    proto_rec->set_load_avg_15_rate(rec.load_avg_15_rate);
    proto_rec->set_send_rate_rate(rec.send_rate_rate);
    proto_rec->set_rcv_rate_rate(rec.rcv_rate_rate);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryTrend(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryTrendRequest* request,
    ::monitor::proto::QueryTrendResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  const int interval_seconds = std::max(
      request->interval_seconds(), kMinTrendIntervalSeconds);
  bool query_ok = false;
  auto records = query_manager_->QueryTrend(
      request->server_name(), time_range, interval_seconds, &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Trend query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_cpu_percent(rec.cpu_percent);
    proto_rec->set_usr_percent(rec.usr_percent);
    proto_rec->set_system_percent(rec.system_percent);
    proto_rec->set_io_wait_percent(rec.io_wait_percent);
    proto_rec->set_load_avg_1(rec.load_avg_1);
    proto_rec->set_load_avg_3(rec.load_avg_3);
    proto_rec->set_load_avg_15(rec.load_avg_15);
    proto_rec->set_mem_used_percent(rec.mem_used_percent);
    proto_rec->set_disk_util_percent(rec.disk_util_percent);
    proto_rec->set_send_rate(rec.send_rate);
    proto_rec->set_rcv_rate(rec.rcv_rate);
    proto_rec->set_score(rec.score);
    proto_rec->set_cpu_percent_rate(rec.cpu_percent_rate);
    proto_rec->set_usr_percent_rate(rec.usr_percent_rate);
    proto_rec->set_system_percent_rate(rec.system_percent_rate);
    proto_rec->set_io_wait_percent_rate(rec.io_wait_percent_rate);
    proto_rec->set_mem_used_percent_rate(rec.mem_used_percent_rate);
    proto_rec->set_disk_util_percent_rate(rec.disk_util_percent_rate);
    proto_rec->set_load_avg_1_rate(rec.load_avg_1_rate);
    proto_rec->set_load_avg_3_rate(rec.load_avg_3_rate);
    proto_rec->set_load_avg_15_rate(rec.load_avg_15_rate);
    proto_rec->set_send_rate_rate(rec.send_rate_rate);
    proto_rec->set_rcv_rate_rate(rec.rcv_rate_rate);
  }

  response->set_interval_seconds(interval_seconds);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryAnomaly(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryAnomalyRequest* request,
    ::monitor::proto::QueryAnomalyResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  // 全局异常检索会绕开 (server_name, timestamp) 索引，导致 COUNT 和排序扫描
  // 整个历史表；生产接口要求指定单个主机以维持索引查询路径。
  if (request->server_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "server_name is required for anomaly queries");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  if (!std::isfinite(request->cpu_threshold()) ||
      !std::isfinite(request->mem_threshold()) ||
      !std::isfinite(request->disk_threshold()) ||
      !std::isfinite(request->change_rate_threshold())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Anomaly thresholds must be finite");
  }

  AnomalyThresholds thresholds;
  thresholds.cpu_threshold =
      request->cpu_threshold() > 0 ? request->cpu_threshold() : 80.0f;
  thresholds.mem_threshold =
      request->mem_threshold() > 0 ? request->mem_threshold() : 90.0f;
  thresholds.disk_threshold =
      request->disk_threshold() > 0 ? request->disk_threshold() : 85.0f;
  thresholds.change_rate_threshold =
      request->change_rate_threshold() > 0 ? request->change_rate_threshold()
                                           : 0.5f;

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QueryAnomaly(
      request->server_name(), time_range, thresholds, page, page_size,
      &total_count, &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Anomaly query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_anomalies();
    proto_rec->set_server_name(rec.server_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_anomaly_type(rec.anomaly_type);
    proto_rec->set_severity(rec.severity);
    proto_rec->set_value(rec.value);
    proto_rec->set_threshold(rec.threshold);
    proto_rec->set_metric_name(rec.metric_name);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}


::grpc::Status QueryServiceImpl::QueryScoreRank(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryScoreRankRequest* request,
    ::monitor::proto::QueryScoreRankResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  SortOrder order = (request->order() == ::monitor::proto::ASC)
                        ? SortOrder::ASC
                        : SortOrder::DESC;

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records =
      query_manager_->QueryScoreRank(order, page, page_size, &total_count,
                                     &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Score rank query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_servers();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_score(rec.score);
    SetTimestamp(proto_rec->mutable_last_update(), rec.last_update);
    proto_rec->set_status(rec.status == ServerStatus::ONLINE
                              ? ::monitor::proto::ONLINE
                              : ::monitor::proto::OFFLINE);
    proto_rec->set_cpu_percent(rec.cpu_percent);
    proto_rec->set_mem_used_percent(rec.mem_used_percent);
    proto_rec->set_disk_util_percent(rec.disk_util_percent);
    proto_rec->set_load_avg_1(rec.load_avg_1);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryLatestScore(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryLatestScoreRequest* request,
    ::monitor::proto::QueryLatestScoreResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;
  (void)request;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  ClusterStats stats;
  bool query_ok = false;
  auto records = query_manager_->QueryLatestScore(&stats, &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Latest score query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_servers();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_score(rec.score);
    SetTimestamp(proto_rec->mutable_last_update(), rec.last_update);
    proto_rec->set_status(rec.status == ServerStatus::ONLINE
                              ? ::monitor::proto::ONLINE
                              : ::monitor::proto::OFFLINE);
    proto_rec->set_cpu_percent(rec.cpu_percent);
    proto_rec->set_mem_used_percent(rec.mem_used_percent);
    proto_rec->set_disk_util_percent(rec.disk_util_percent);
    proto_rec->set_load_avg_1(rec.load_avg_1);
  }

  auto* proto_stats = response->mutable_cluster_stats();
  proto_stats->set_total_servers(stats.total_servers);
  proto_stats->set_online_servers(stats.online_servers);
  proto_stats->set_offline_servers(stats.offline_servers);
  proto_stats->set_avg_score(stats.avg_score);
  proto_stats->set_max_score(stats.max_score);
  proto_stats->set_min_score(stats.min_score);
  proto_stats->set_best_server(stats.best_server);
  proto_stats->set_worst_server(stats.worst_server);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryNetDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QueryNetDetailResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QueryNetDetail(
      request->server_name(), time_range, page, page_size, &total_count,
      &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Network detail query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_net_name(rec.net_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_err_in(rec.err_in);
    proto_rec->set_err_out(rec.err_out);
    proto_rec->set_drop_in(rec.drop_in);
    proto_rec->set_drop_out(rec.drop_out);
    proto_rec->set_rcv_bytes_rate(rec.rcv_bytes_rate);
    proto_rec->set_snd_bytes_rate(rec.snd_bytes_rate);
    proto_rec->set_rcv_packets_rate(rec.rcv_packets_rate);
    proto_rec->set_snd_packets_rate(rec.snd_packets_rate);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryDiskDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QueryDiskDetailResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QueryDiskDetail(
      request->server_name(), time_range, page, page_size, &total_count,
      &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Disk detail query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_disk_name(rec.disk_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_read_bytes_per_sec(rec.read_bytes_per_sec);
    proto_rec->set_write_bytes_per_sec(rec.write_bytes_per_sec);
    proto_rec->set_read_iops(rec.read_iops);
    proto_rec->set_write_iops(rec.write_iops);
    proto_rec->set_avg_read_latency_ms(rec.avg_read_latency_ms);
    proto_rec->set_avg_write_latency_ms(rec.avg_write_latency_ms);
    proto_rec->set_util_percent(rec.util_percent);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QueryMemDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QueryMemDetailResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QueryMemDetail(
      request->server_name(), time_range, page, page_size, &total_count,
      &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Memory detail query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_total(rec.total);
    proto_rec->set_free(rec.free);
    proto_rec->set_avail(rec.avail);
    proto_rec->set_buffers(rec.buffers);
    proto_rec->set_cached(rec.cached);
    proto_rec->set_active(rec.active);
    proto_rec->set_inactive(rec.inactive);
    proto_rec->set_dirty(rec.dirty);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

::grpc::Status QueryServiceImpl::QuerySoftIrqDetail(
    ::grpc::ServerContext* context,
    const ::monitor::proto::QueryDetailRequest* request,
    ::monitor::proto::QuerySoftIrqDetailResponse* response) {
  const auto auth_status = AuthorizeQuery(*context);
  if (!auth_status.ok()) return auth_status;

  if (!query_manager_) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Query manager not initialized");
  }

  TimeRange time_range;
  if (!ConvertTimeRange(request->time_range(), &time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid protobuf timestamp");
  }
  if (!query_manager_->ValidateTimeRange(time_range)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid time range: start_time > end_time");
  }

  int page = request->pagination().page();
  int page_size = request->pagination().page_size();
  NormalizeQueryPagination(&page, &page_size);

  int total_count = 0;
  bool query_ok = false;
  auto records = query_manager_->QuerySoftIrqDetail(
      request->server_name(), time_range, page, page_size, &total_count,
      &query_ok);
  if (!query_ok) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "SoftIRQ detail query is unavailable");
  }

  for (const auto& rec : records) {
    auto* proto_rec = response->add_records();
    proto_rec->set_server_name(rec.server_name);
    proto_rec->set_cpu_name(rec.cpu_name);
    SetTimestamp(proto_rec->mutable_timestamp(), rec.timestamp);
    proto_rec->set_hi(rec.hi);
    proto_rec->set_timer(rec.timer);
    proto_rec->set_net_tx(rec.net_tx);
    proto_rec->set_net_rx(rec.net_rx);
    proto_rec->set_block(rec.block);
    proto_rec->set_sched(rec.sched);
  }

  response->set_total_count(total_count);
  response->set_page(page);
  response->set_page_size(page_size);

  return grpc::Status::OK;
}

}  // namespace monitor
