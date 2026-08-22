#pragma once

/**
 * @file net_monitor.h
 * @brief Worker /proc/net/dev 网络指标 fallback 接口。
 *
 * NetMonitor 属于普通网络监控路径，读取接口累计字节/包/错误/丢弃并
 * 计算速率；ENABLE_EBPF 打开时 MetricCollector 选择 NetEbpfMonitor，
 * 但其输出仍填充相同的 MonitorInfo.net_info 语义。
 */

#include <chrono>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
/**
 * @brief 维护网卡上一轮网络累计计数，用于计算接口速率。
 */
class NetMonitor : public MonitorInter {
  struct NetInfo {
    std::string name;
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    uint64_t err_in;
    uint64_t err_out;
    uint64_t drop_in;
    uint64_t drop_out;
    std::chrono::steady_clock::time_point timepoint;
  };

 public:
  NetMonitor() {}
  /**
   * @brief 读取 /proc/net/dev 并填充每个接口的包/字节速率及错误计数。
   * @sideeffect 更新 last_net_info_；首次采集的速率为零。
   */
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, NetInfo> last_net_info_;
};

}  // namespace monitor
