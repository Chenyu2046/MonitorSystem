#pragma once

/**
 * @file cpu_load_monitor.h
 * @brief Worker CPU load average 采集器接口。
 *
 * 上游是 MetricCollector，下游是 MonitorInfo.cpu_load。数据来自
 * /proc/loadavg；这里的
 * load average 是调度队列负载，不等同于 CPU busy 百分比。
 */

#include <string>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief 采集 1/3/15 分钟系统 load average。
 *
 * 该类只填充主机级 CPU load 字段，不保存跨轮 delta，也不参与逐核
 * CPU 百分比计算；逐核 CPU 时间由 CpuStatMonitor 负责。
 */
class CpuLoadMonitor : public MonitorInter {
 public:
  CpuLoadMonitor() {}
  /**
   * @brief 读取内核模块或 /proc/loadavg 并写入本轮 Protobuf。
   * @sideeffect 修改 monitor_info->cpu_load；设备和 mmap 资源在函数内释放。
   */
  bool Init() override;
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  float load_avg_1_;
  float load_avg_3_;
  float load_avg_15_;
};

}  // namespace monitor
