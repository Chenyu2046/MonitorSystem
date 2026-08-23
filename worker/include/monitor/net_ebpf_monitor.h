#pragma once

/**
 * @file net_ebpf_monitor.h
 * @brief 基于 TC/eBPF 的网卡流量采集接口。
 *
 * 该监控器读取每个 CPU 上的网卡累计计数，并在用户态汇总后按时间差
 * 计算速率；eBPF 不可用或读取失败时直接返回采集错误。
 */

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "monitor/monitor_inter.h"

struct bpf_object;

namespace monitor {

/**
 * 基于 eBPF 的网络流量监控器。
 *
 * 使用 eBPF tracepoint 挂载到内核网络路径，
 * 实时统计每个网卡的收发流量。
 */
class NetEbpfMonitor : public MonitorInter {
 public:
  /** 构造监控器；实际数据源在 MetricCollector::Init 中验证。 */
  NetEbpfMonitor();
  /** 释放 eBPF 对象及已附加的 TC hook。 */
  ~NetEbpfMonitor() override;

  /** 采集各网卡流量、包数、错误和丢包，并转换为协议字段。 */
  bool Init() override;
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  /** 主动停止并清理 eBPF 资源。 */
  void Stop() override;

  /** 返回当前 eBPF 数据路径是否仍处于可用状态。 */
  bool IsLoaded() const { return loaded_; }

 private:
  /** 打开 skeleton、读取 map，并为可用网卡附加 ingress/egress TC hook。 */
  bool InitEbpf();

  /** 分离已挂载的 TC hook，并销毁 skeleton。 */
  void CleanupEbpf();

  /** 根据 ifindex 获取网卡名称，优先使用用户态缓存。 */
  std::string GetIfName(uint32_t ifindex);

  struct NetStats {
    uint64_t rcv_bytes = 0;
    uint64_t rcv_packets = 0;
    uint64_t snd_bytes = 0;
    uint64_t snd_packets = 0;
  };

  /** 读取 Per-CPU map 并把同一网卡的计数合并为一个快照。 */
  bool ReadAggregatedStats(uint32_t ifindex, NetStats* stats);

  // 上一次采集的数据，用于计算速率
  struct NetStatCache {
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    std::chrono::steady_clock::time_point timestamp;
  };

  std::unordered_map<uint32_t, NetStatCache> cache_;        // key: ifindex
  std::unordered_map<uint32_t, std::string> ifname_cache_;  // ifindex -> name
  std::vector<uint32_t> attached_ifindexes_;  // 已附加 TC hook 的网卡
  std::vector<uint32_t> interface_set_;
  std::vector<NetStats> per_cpu_stats_;

  struct bpf_object* bpf_obj_ = nullptr;
  int map_fd_ = -1;
  bool loaded_ = false;
  int consecutive_read_failures_ = 0;

  std::chrono::steady_clock::time_point last_update_;
};

}  // namespace monitor
