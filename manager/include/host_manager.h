#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "monitor_info.pb.h"
#include "runtime_config.h"

namespace monitor {

struct HostScore {
  monitor::proto::MonitorInfo info;
  double score;
  std::chrono::system_clock::time_point timestamp;
};

// 管理多个远程主机的监控数据（推送模式）
class HostManager {
 public:
  enum class IngestResult { kPersisted, kFailed, kCommitUnknown };
  explicit HostManager(runtime_config::DatabaseConfig database_config);
  ~HostManager();

  // 启动后台处理线程
  void Start();
  void Stop();

  // 接收工作者推送的数据（由 gRPC 服务调用）
  IngestResult OnDataReceived(const monitor::proto::MonitorInfo& info);

  // 获取所有主机评分
  std::unordered_map<std::string, HostScore> GetAllHostScores();

  // 获取最优主机
  std::string GetBestHost();

 private:
  struct NetDetailSample {
    float rcv_bytes_rate = 0;
    float rcv_packets_rate = 0;
    float snd_bytes_rate = 0;
    float snd_packets_rate = 0;
    uint64_t err_in = 0, err_out = 0, drop_in = 0, drop_out = 0;
  };
  struct SoftIrqSample {
    float hi = 0, timer = 0, net_tx = 0, net_rx = 0, block = 0;
    float irq_poll = 0, tasklet = 0, sched = 0, hrtimer = 0, rcu = 0;
  };
  struct MemDetailSample {
    float total = 0, free = 0, avail = 0, buffers = 0, cached = 0;
    float swap_cached = 0, active = 0, inactive = 0, active_anon = 0;
    float inactive_anon = 0, active_file = 0, inactive_file = 0, dirty = 0;
    float writeback = 0, anon_pages = 0, mapped = 0, kreclaimable = 0;
    float sreclaimable = 0, sunreclaim = 0;
  };
  struct DiskDetailSample {
    float read_bytes_per_sec = 0, write_bytes_per_sec = 0, read_iops = 0;
    float write_iops = 0, avg_read_latency_ms = 0, avg_write_latency_ms = 0;
    float util_percent = 0;
  };
  struct PerfSample {
    float cpu_percent = 0, usr_percent = 0, system_percent = 0;
    float nice_percent = 0, idle_percent = 0, io_wait_percent = 0;
    float irq_percent = 0, soft_irq_percent = 0, load_avg_1 = 0;
    float load_avg_3 = 0, load_avg_15 = 0, mem_used_percent = 0;
    float mem_total = 0, mem_free = 0, mem_avail = 0;
    float net_in_rate = 0, net_out_rate = 0, score = 0;
  };
  void ProcessLoop();
  double CalcScore(const monitor::proto::MonitorInfo& info);
  IngestResult WriteToMysql(const std::string& host_name, const HostScore& host_score,
                    double net_in_rate, double net_out_rate,
                    float cpu_percent_rate, float usr_percent_rate,
                    float system_percent_rate, float nice_percent_rate,
                    float idle_percent_rate, float io_wait_percent_rate,
                    float irq_percent_rate, float soft_irq_percent_rate,
                    float steal_percent_rate, float guest_percent_rate,
                    float guest_nice_percent_rate, float load_avg_1_rate,
                    float load_avg_3_rate, float load_avg_15_rate,
                    float mem_used_percent_rate, float mem_total_rate,
                    float mem_free_rate, float mem_avail_rate,
                    float net_in_rate_rate, float net_out_rate_rate,
                    float net_in_drop_rate_rate, float net_out_drop_rate_rate);

  std::unordered_map<std::string, HostScore> host_scores_;
  std::mutex mtx_;
  std::mutex ingest_mtx_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> thread_;
  runtime_config::DatabaseConfig database_config_;
  void* mysql_conn_ = nullptr;
  std::map<std::string, std::map<std::string, NetDetailSample>> last_net_samples_;
  std::map<std::string, std::map<std::string, SoftIrqSample>> last_softirq_samples_;
  std::map<std::string, MemDetailSample> last_mem_samples_;
  std::map<std::string, std::map<std::string, DiskDetailSample>> last_disk_samples_;
  std::map<std::string, float> last_disk_util_samples_;
  std::map<std::string, PerfSample> last_perf_samples_;
};

}  // namespace monitor
