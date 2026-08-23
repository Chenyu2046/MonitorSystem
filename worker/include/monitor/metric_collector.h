#pragma once

/**
 * @file metric_collector.h
 * @brief Worker 基础监控采集器的组合入口。
 *
 * MetricCollector 负责把 CPU、内存、磁盘、网络和主机信息等多个
 * MonitorInter 实现组织成一次完整的 MonitorInfo 采集。上游是
 * MonitorPusher 的周期调用，下游是待发送的 Protobuf；本类不负责
 * 诊断状态机、序列化或网络发送。
 */

#include <memory>
#include <string>
#include <vector>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief 按固定顺序运行 Worker 的基础监控器集合。
 *
 * 每个监控器只向同一个 MonitorInfo 写入自己的字段，所有权由
 * unique_ptr 管理。ENABLE_EBPF 只决定网络监控器的具体实现，未改变
 * MetricCollector 的调用接口。
 */
class MetricCollector {
 public:
  MetricCollector();
  ~MetricCollector();

  /** @brief 验证 hostname 和所有 required data source。 */
  bool Init();

  /** @brief 采集完整样本；任一 required collector 非 OK 时停止发布。 */
  CollectStatus CollectAll(monitor::proto::MonitorInfo* monitor_info);

 private:
  // monitors 里面要放指向Monitorinter的子类对象的 unique_ptr, make_unique 返回的是 unique_ptr 的实例
  std::vector<std::unique_ptr<MonitorInter>> monitors_; 
  std::string hostname_;
  bool initialized_ = false;
};

}  // namespace monitor
