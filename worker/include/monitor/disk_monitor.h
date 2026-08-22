#pragma once

/**
 * @file disk_monitor.h
 * @brief Worker 普通磁盘指标采集器接口。
 *
 * DiskMonitor 读取 /proc/diskstats 的累计 I/O 计数，输出读写次数、字节
 * 速率、IOPS、平均延迟和 util。它属于普通监控路径，不等同于 block I/O
 * eBPF 诊断探针提供的进程/请求级证据。
 */

#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief 将块设备累计统计转换为主机磁盘概览。
 *
 * 类内实现使用静态缓存保存各设备上一轮样本和采样时间，以便从累计
 * diskstats 计算变化率；当前实现不负责设备级异常判断。
 */
class DiskMonitor : public MonitorInter {
 public:
  DiskMonitor() {}
  /**
   * @brief 读取 /proc/diskstats 并填充本轮磁盘指标。
   * @sideeffect 更新设备累计值缓存并追加 MonitorInfo.disk_info。
   */
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}
};

}  // namespace monitor
