/**
 * @file metric_collector.cpp
 * @brief Worker 基础指标采集器的组装与执行实现。
 *
 * 数据流为：主机名与各监控器读取 Linux/内核数据 -> 共同填充
 * MonitorInfo -> MonitorPusher 继续执行异常判断和诊断快照。
 * 本文件只负责监控器生命周期和采集编排，不聚合 CPU 核、不判定异常。
 */

#include "monitor/metric_collector.h"

#include <unistd.h>

#include <memory>

#include "monitor/cpu_load_monitor.h"
#include "monitor/cpu_softirq_monitor.h"
#include "monitor/cpu_stat_monitor.h"
#include "monitor/disk_monitor.h"
#include "monitor/mem_monitor.h"
#include "monitor/host_info_monitor.h"

#ifdef ENABLE_EBPF
#include "monitor/net_ebpf_monitor.h"
#else
#include "monitor/net_monitor.h"
#endif

namespace monitor {

MetricCollector::MetricCollector() {
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
    hostname_ = hostname;
  }

  // 初始化所有监控器。各实例按固定顺序写入同一个 MonitorInfo，
  // 这里不创建线程；周期调度由 MonitorPusher 完成。
  monitors_.push_back(std::make_unique<CpuLoadMonitor>());
  monitors_.push_back(std::make_unique<CpuStatMonitor>());
  monitors_.push_back(std::make_unique<CpuSoftIrqMonitor>());
  monitors_.push_back(std::make_unique<MemMonitor>());
#ifdef ENABLE_EBPF
  monitors_.push_back(std::make_unique<NetEbpfMonitor>());
#else
  monitors_.push_back(std::make_unique<NetMonitor>());
#endif
  monitors_.push_back(std::make_unique<DiskMonitor>());
  monitors_.push_back(std::make_unique<HostInfoMonitor>());
}

bool MetricCollector::Init() {
  if (hostname_.empty()) {
    return false;
  }
  for (auto& monitor : monitors_) {
    if (!monitor->Init()) {
      return false;
    }
  }
  initialized_ = true;
  return true;
}

MetricCollector::~MetricCollector() {
  // 先通知每个监控器停止其外部资源，例如 eBPF attach 或 mmap 关联的
  // 设备访问，再由 unique_ptr 自动释放对象。
  for (auto& monitor : monitors_) {
    monitor->Stop();
  }
}

CollectStatus MetricCollector::CollectAll(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info || !initialized_) {
    return CollectStatus::kError;
  }

  // 设置主机名，作为 Manager 侧按主机分片和持久化的主键语义之一。
  monitor_info->set_name(hostname_);

  // 每个监控器只负责自己的 Protobuf 字段。采集器不在这里做异常判断，
  // 因此普通监控路径和诊断路径仍由 MonitorPusher 分阶段处理。
  for (auto& monitor : monitors_) {
    const CollectStatus status = monitor->UpdateOnce(monitor_info);
    if (status != CollectStatus::kOk) {
      monitor_info->Clear();
      return status;
    }
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
