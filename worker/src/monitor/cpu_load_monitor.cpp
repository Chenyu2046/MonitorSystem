/**
 * @file cpu_load_monitor.cpp
 * @brief CPU load average 的 /proc/loadavg 采集。
 *
 * 当前 develop 没有对应的 cpu_load 内核模块，唯一数据源是
 * /proc/loadavg。输出是 1/3/15 分钟
 * load average，不应被解释成 CPU 使用率或逐核 CPU 百分比。
 */

#include "monitor/cpu_load_monitor.h"
#include "monitor_info.pb.h"

#include <cmath>
#include <fstream>

namespace monitor {

/**
 * @brief 从 /proc/loadavg 读取三个时间窗口的 load average。
 *
 * @return 成功解析三个浮点值时返回 true；文件打开或解析不完整时返回
 * false。调用方据此决定是否写入 Protobuf。
 */
bool CpuLoadMonitor::Init() {
    std::ifstream file("/proc/loadavg");
    return file.good();
}

CollectStatus CpuLoadMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
    if (!monitor_info) return CollectStatus::kError;
    std::ifstream file("/proc/loadavg");
    double load1 = 0.0, load5 = 0.0, load15 = 0.0;
    if (!file.is_open() || !(file >> load1 >> load5 >> load15) ||
        !std::isfinite(load1) || !std::isfinite(load5) ||
        !std::isfinite(load15)) {
        return CollectStatus::kError;
    }
    auto* cpu_load_msg = monitor_info->mutable_cpu_load();
    cpu_load_msg->set_load_avg_1(load1);
    cpu_load_msg->set_load_avg_3(load5);
    cpu_load_msg->set_load_avg_15(load15);
    cpu_load_msg->set_sample_valid(true);
    return CollectStatus::kOk;
}
}  // namespace monitor
