#pragma once

/**
 * @file cpu_softirq_monitor.h
 * @brief Worker 逐核 SoftIRQ 累计计数和速率采集接口。
 *
 * 输入来自 cpu_softirq_monitor 内核模块通过 mmap 暴露的每核累计值；
 * 输出是 MonitorInfo.soft_irq。实现按两次采样的时间间隔计算每秒
 * 处理次数，NET_RX/NET_TX 可与网络包速率结合判断网络栈压力。
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
/**
 * @brief 保存每个 CPU 核上一轮 SoftIRQ 计数和采样时间。
 *
 * 该结构是速率计算的缓存基线，不是本轮 protobuf 的最终值；时间点
 * 用 steady_clock 保证计算间隔不受系统墙上时钟调整影响。
 */
class CpuSoftIrqMonitor : public MonitorInter {
  struct SoftIrq {
    std::string cpu_name;
    std::uint64_t hi;
    std::uint64_t timer;
    std::uint64_t net_tx;
    std::uint64_t net_rx;
    std::uint64_t block;
    std::uint64_t irq_poll;
    std::uint64_t tasklet;
    std::uint64_t sched;
    std::uint64_t hrtimer;
    std::uint64_t rcu;
    std::chrono::steady_clock::time_point timepoint;
  };

 public:
  CpuSoftIrqMonitor() {}
  /**
   * @brief 读取每核 SoftIRQ 累计计数并填充本轮速率。
   * @sideeffect 更新 cpu_softirqs_；设备不可用时返回 kError。
   */
  bool Init() override;
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, struct SoftIrq> cpu_softirqs_;
};

}  // namespace monitor
