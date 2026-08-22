#pragma once

/**
 * @file cpu_stat_monitor.h
 * @brief Worker 逐核 CPU 累计时间快照、delta 和百分比采集接口。
 *
 * 输入来自内核模块 mmap 或 /proc/stat fallback；输出是每个 CPU 核的
 * MonitorInfo.cpu_stat repeated message。CPU 时间是自系统启动以来的
 * 累计计数器，因此本模块保存前一轮快照，先以 uint64_t 计算 delta，
 * 再由实现使用 double 计算百分比。Worker 不在此处做整机平均。
 */

#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

namespace cpu_stat_detail {

/**
 * @brief 一个 CPU 核在某一时刻的累计时间快照。
 *
 * guest 字段保留源数据完整性，但当前 total/busy 公式按项目既有语义
 * 使用 user、nice、system、idle、iowait、irq、softirq、steal 八项。
 */
struct CpuStatSnapshot {
  std::uint64_t user = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t nice = 0;
  std::uint64_t io_wait = 0;
  std::uint64_t irq = 0;
  std::uint64_t soft_irq = 0;
  std::uint64_t steal = 0;
  std::uint64_t guest = 0;
  std::uint64_t guest_nice = 0;
};

/**
 * @brief 两轮 CPU 累计快照之间的整数增量。
 *
 * counter_reset 表示至少一个累计计数器回退；此时不执行无符号减法，
 * 调用方不生成样本但仍可把当前快照作为下一轮基线。IOWait 计入 total
 * 但不计入 busy，IRQ/SoftIRQ/steal 计入 busy。
 */
struct CpuStatDelta {
  bool counter_reset = false;
  std::uint64_t user = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t nice = 0;
  std::uint64_t io_wait = 0;
  std::uint64_t irq = 0;
  std::uint64_t soft_irq = 0;
  std::uint64_t steal = 0;
  std::uint64_t total = 0;
  std::uint64_t busy = 0;

  /** @brief 只有非 reset 且本轮总增量非零时才允许生成 protobuf 样本。 */
  bool IsReportable() const { return !counter_reset && total > 0; }
};

/**
 * @brief 根据当前和上一轮累计快照计算本轮 CPU 时间增量。
 *
 * @param current 当前 CPU 累计时间快照。
 * @param previous 上一次 CPU 累计时间快照。
 * @return 各状态的 uint64_t delta、total、busy 及 reset 标记。
 *
 * 处理规则：先检查计数器回退，再进行整数减法；百分比由调用方在
 * 需要时转为 double，避免巨大累计值先转 float 丢失短采样周期的小增量。
 */
CpuStatDelta ComputeCpuStatDelta(const CpuStatSnapshot& current,
                                 const CpuStatSnapshot& previous);

}  // namespace cpu_stat_detail

/**
 * @brief 采集并上报每个 CPU 核的 CPU 时间占比。
 *
 * cpu_stat_map_ 是按 cpu 名称索引的前一轮累计快照缓存；该缓存只服务
 * Worker 当前采集线程，不把逐核数据提前聚合成主机平均值。
 */
class CpuStatMonitor : public MonitorInter {
 public:
  CpuStatMonitor() {}
  /**
   * @brief 从 mmap 或 /proc/stat 读取快照并生成本轮逐核 CPU 样本。
   * @sideeffect 更新每个 CPU 核缓存；reset/零 delta 时只更新缓存不追加样本。
   */
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, cpu_stat_detail::CpuStatSnapshot>
      cpu_stat_map_;
};

}  // namespace monitor
