#pragma once

/**
 * @file mem_monitor.h
 * @brief Worker 主机级内存统计采集器接口。
 *
 * 数据源是 /proc/meminfo，输出为 MonitorInfo.mem_info。该监控器描述
 * 主机级 total/free/available、buffer/cache 和页面分类，不做进程级
 * 内存归因或泄漏定位。
 */

#include <string>
#include <cstdint>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief 采集 /proc/meminfo 中需要上报的内存字段。
 *
 * 结构中的值保持 /proc/meminfo 的原始 KiB 语义，UpdateOnce() 再转换为
 * protobuf 使用的单位；available 用于 used_percent 的分母计算。
 */
class MemMonitor : public MonitorInter {
  struct MenInfo {
    int64_t total;
    int64_t free;
    int64_t avail;
    int64_t buffers;
    int64_t cached;
    int64_t swap_cached;
    int64_t active;
    int64_t in_active;
    int64_t active_anon;
    int64_t inactive_anon;
    int64_t active_file;
    int64_t inactive_file;
    int64_t dirty;
    int64_t writeback;
    int64_t anon_pages;
    int64_t mapped;
    int64_t kReclaimable;
    int64_t sReclaimable;
    int64_t sUnreclaim;
  };

 public:
  MemMonitor() {}
  bool Init() override;
  /**
   * @brief 读取主机内存概览并写入 MonitorInfo。
   * @sideeffect 设置本轮主机级 memory message；不维护跨轮 delta。
   */
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}
};

}  // namespace monitor
