#pragma once

/**
 * @file net_monitor.h
 * @brief Worker /proc/net/dev 网络指标接口。
 *
 * NetMonitor 属于普通网络监控路径，读取接口累计字节/包/错误/丢弃并
 * 计算速率；ENABLE_EBPF 打开时 MetricCollector 选择 NetEbpfMonitor，
 * 但其输出仍填充相同的 MonitorInfo.net_info 语义。
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
struct NetRawSample {
  std::string name;
  std::uint64_t rcv_bytes = 0;
  std::uint64_t rcv_packets = 0;
  std::uint64_t snd_bytes = 0;
  std::uint64_t snd_packets = 0;
  std::uint64_t err_in = 0;
  std::uint64_t err_out = 0;
  std::uint64_t drop_in = 0;
  std::uint64_t drop_out = 0;
};

bool ParseNetDevLine(const std::string& line, NetRawSample* out);
/**
 * @brief 维护网卡上一轮网络累计计数，用于计算接口速率。
 */
class NetMonitor : public MonitorInter {
  struct NetInfo {
    NetRawSample sample;
    std::chrono::steady_clock::time_point timepoint;
  };

 public:
  NetMonitor() {}
  bool Init() override;
  /**
   * @brief 读取 /proc/net/dev 并填充每个接口的包/字节速率及错误计数。
   * @sideeffect 更新 last_net_info_；首次采集的速率为零。
   */
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, NetInfo> last_net_info_;
};

}  // namespace monitor
