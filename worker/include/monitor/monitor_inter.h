#pragma once

/**
 * @file monitor_inter.h
 * @brief Worker 基础监控器的最小生命周期接口。
 *
 * MetricCollector 持有各采集器的 unique_ptr，并在同一轮采集中依次调用
 * UpdateOnce()；Stop() 用于释放 eBPF/设备等外部资源。接口不约束具体
 * 数据源，子类可读取显式选择的 /proc、字符设备 mmap 或 eBPF source。
 */

#include <string>

#include "monitor_info.pb.h"

namespace monitor {
enum class CollectStatus { kOk, kNotReady, kError };

/** @brief 所有基础监控器共享的采集/停止抽象接口。 */
class MonitorInter {
 public:
  MonitorInter() = default;
  virtual ~MonitorInter() = default;
  /** @brief 启动期验证所选数据源和外部资源。 */
  virtual bool Init() { return true; }
  /** @brief 采集一轮并写入传入的 MonitorInfo。 */
  virtual CollectStatus UpdateOnce(
      monitor::proto::MonitorInfo* monitor_info) = 0;
  /** @brief 停止并释放该监控器持有的外部资源。 */
  virtual void Stop() = 0;
};
}  // namespace monitor
