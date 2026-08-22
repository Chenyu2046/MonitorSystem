/**
 * @file cpu_stat_monitor.cpp
 * @brief Worker 逐核 CPU 时间采集、快照 delta 和百分比计算实现。
 *
 * 数据链路：内核模块 mmap 或 /proc/stat -> 每核累计时间快照 ->
 * uint64_t 前后差值 -> total/busy 公式 -> protobuf float 百分比 ->
 * MonitorInfo -> Manager。IOWait 进入 total 但不进入 busy；IRQ、SoftIRQ
 * 和 steal 进入 busy。计数器 reset 或 total_delta 为零时不发送样本，
 * 但始终更新缓存基线。
 */

#include "monitor/cpu_stat_monitor.h"
#include "monitor/monitor_structs.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

namespace cpu_stat_detail {

CpuStatDelta ComputeCpuStatDelta(const CpuStatSnapshot& current,
                                 const CpuStatSnapshot& previous) {
    // 必须在任何 uint64_t 减法之前检查回退，否则 reset 会产生无符号
    // 下溢，表现为一个极大的伪增量并污染后续 CPU 百分比。
    CpuStatDelta delta;
    delta.counter_reset =
        current.user < previous.user || current.nice < previous.nice ||
        current.system < previous.system || current.idle < previous.idle ||
        current.io_wait < previous.io_wait || current.irq < previous.irq ||
        current.soft_irq < previous.soft_irq || current.steal < previous.steal;
    if (delta.counter_reset) {
        return delta;
    }

    // 保持累计计数器为整数，直接计算本轮差值；不要先把累计值转成
    // float，否则系统运行时间较长时可能无法表示很小的采样增量。
    delta.user = current.user - previous.user;
    delta.nice = current.nice - previous.nice;
    delta.system = current.system - previous.system;
    delta.idle = current.idle - previous.idle;
    delta.io_wait = current.io_wait - previous.io_wait;
    delta.irq = current.irq - previous.irq;
    delta.soft_irq = current.soft_irq - previous.soft_irq;
    delta.steal = current.steal - previous.steal;
    // IOWait 属于 CPU 总时间，因此进入分母；当前项目不把 IOWait
    // 视为 CPU busy 时间。
    delta.total = delta.user + delta.nice + delta.system + delta.idle +
                  delta.io_wait + delta.irq + delta.soft_irq + delta.steal;
    // IRQ、SoftIRQ 和 steal 都表示 CPU 正在执行非 idle 工作，因此计入
    // busy；IOWait 仍保持在 total 中但不计入这里。
    delta.busy = delta.user + delta.nice + delta.system + delta.irq +
                 delta.soft_irq + delta.steal;
    return delta;
}

}  // namespace cpu_stat_detail

void CpuStatMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
    // 设备路径优先，/proc/stat 只作为整条 mmap 读取路径不可用时的
    // fallback；两条路径共用 ComputeCpuStatDelta，保证 reset/零 delta
    // 语义一致。
    int fd = open("/dev/cpu_stat_monitor", O_RDONLY);
    if (fd < 0) {
        std::ifstream input("/proc/stat");
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream values(line);
            std::string cpu_name;
            if (!(values >> cpu_name) || cpu_name == "cpu" ||
                cpu_name.rfind("cpu", 0) != 0) {
                continue;
            }

            cpu_stat_detail::CpuStatSnapshot current;
            if (!(values >> current.user >> current.nice >> current.system >>
                  current.idle >> current.io_wait >> current.irq >>
                  current.soft_irq >> current.steal >> current.guest >>
                  current.guest_nice)) {
                continue;
            }

            const auto it = cpu_stat_map_.find(cpu_name);
            if (it != cpu_stat_map_.end()) {
                // 首轮只有基线没有周期 delta，因此不生成样本；后续轮次
                // 的百分比全部基于整数 delta，再把最终比例转为 float。
                const auto delta =
                    cpu_stat_detail::ComputeCpuStatDelta(current, it->second);
                if (delta.IsReportable()) {
                    auto* message = monitor_info->add_cpu_stat();
                    const double total = static_cast<double>(delta.total);
                    message->set_cpu_name(cpu_name);
                    message->set_cpu_percent(static_cast<float>(
                        static_cast<double>(delta.busy) / total * 100.0));
                    message->set_usr_percent(static_cast<float>(
                        static_cast<double>(delta.user) / total * 100.0));
                    message->set_system_percent(static_cast<float>(
                        static_cast<double>(delta.system) / total * 100.0));
                    message->set_nice_percent(static_cast<float>(
                        static_cast<double>(delta.nice) / total * 100.0));
                    message->set_idle_percent(static_cast<float>(
                        static_cast<double>(delta.idle) / total * 100.0));
                    message->set_io_wait_percent(static_cast<float>(
                        static_cast<double>(delta.io_wait) / total * 100.0));
                    message->set_irq_percent(static_cast<float>(
                        static_cast<double>(delta.irq) / total * 100.0));
                    message->set_soft_irq_percent(static_cast<float>(
                        static_cast<double>(delta.soft_irq) / total * 100.0));
                }
            }
            // 无论本轮是否 reportable，都推进缓存。这样 reset 或静止
            // 采样后，下一轮会以最新快照重新建立正确的 delta 基线。
            cpu_stat_map_[cpu_name] = current;
        }
        return;
    }

    // 共享结构按固定容量映射；空 cpu_name 是内核模块写入的结束标记。
    size_t stat_count = 128; // 假设最多128个CPU
    size_t stat_size = sizeof(struct cpu_stat) * stat_count;
    void* addr = mmap(nullptr, stat_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return;
    }

    struct cpu_stat* stats = static_cast<struct cpu_stat*>(addr);
    for (size_t i = 0; i < stat_count; ++i) {
        if (stats[i].cpu_name[0] == '\0') break;
        cpu_stat_detail::CpuStatSnapshot current;
        current.user = stats[i].user;
        current.nice = stats[i].nice;
        current.system = stats[i].system;
        current.idle = stats[i].idle;
        current.io_wait = stats[i].iowait;
        current.irq = stats[i].irq;
        current.soft_irq = stats[i].softirq;
        current.steal = stats[i].steal;
        current.guest = stats[i].guest;
        current.guest_nice = stats[i].guest_nice;
        auto it = cpu_stat_map_.find(stats[i].cpu_name);
        if (it != cpu_stat_map_.end()) {
          // mmap 路径与 /proc/stat 路径使用同一 delta helper，避免两条
          // 数据源在累计值精度、reset 或零增量处理上产生分叉。
          const auto delta =
              cpu_stat_detail::ComputeCpuStatDelta(current, it->second);
          if (delta.IsReportable()) {
            auto* cpu_stat_msg = monitor_info->add_cpu_stat();
            const double total = static_cast<double>(delta.total);

            cpu_stat_msg->set_cpu_name(stats[i].cpu_name);
            cpu_stat_msg->set_cpu_percent(static_cast<float>(
                static_cast<double>(delta.busy) / total * 100.0));
            cpu_stat_msg->set_usr_percent(static_cast<float>(
                static_cast<double>(delta.user) / total * 100.0));
            cpu_stat_msg->set_system_percent(static_cast<float>(
                static_cast<double>(delta.system) / total * 100.0));
            cpu_stat_msg->set_nice_percent(static_cast<float>(
                static_cast<double>(delta.nice) / total * 100.0));
            cpu_stat_msg->set_idle_percent(static_cast<float>(
                static_cast<double>(delta.idle) / total * 100.0));
            cpu_stat_msg->set_io_wait_percent(static_cast<float>(
                static_cast<double>(delta.io_wait) / total * 100.0));
            cpu_stat_msg->set_irq_percent(static_cast<float>(
                static_cast<double>(delta.irq) / total * 100.0));
            cpu_stat_msg->set_soft_irq_percent(static_cast<float>(
                static_cast<double>(delta.soft_irq) / total * 100.0));
          }
        }
        // 缓存本轮 CPU 累计时间快照。即使没有追加 protobuf，也必须
        // 更新基线，避免下一轮重复使用 reset 前或静止期间的旧值。
        cpu_stat_map_[stats[i].cpu_name] = current;
    }

    munmap(addr, stat_size);
    close(fd);
    return;
}
}  // namespace monitor
