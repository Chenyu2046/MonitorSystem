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
    CpuStatDelta delta;
    delta.counter_reset =
        current.user < previous.user || current.nice < previous.nice ||
        current.system < previous.system || current.idle < previous.idle ||
        current.io_wait < previous.io_wait || current.irq < previous.irq ||
        current.soft_irq < previous.soft_irq || current.steal < previous.steal;
    if (delta.counter_reset) {
        return delta;
    }

    delta.user = current.user - previous.user;
    delta.nice = current.nice - previous.nice;
    delta.system = current.system - previous.system;
    delta.idle = current.idle - previous.idle;
    delta.io_wait = current.io_wait - previous.io_wait;
    delta.irq = current.irq - previous.irq;
    delta.soft_irq = current.soft_irq - previous.soft_irq;
    delta.steal = current.steal - previous.steal;
    delta.total = delta.user + delta.nice + delta.system + delta.idle +
                  delta.io_wait + delta.irq + delta.soft_irq + delta.steal;
    delta.busy = delta.user + delta.nice + delta.system + delta.irq +
                 delta.soft_irq + delta.steal;
    return delta;
}

}  // namespace cpu_stat_detail

void CpuStatMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
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
            cpu_stat_map_[cpu_name] = current;
        }
        return;
    }

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
        // 缓存本轮 CPU 累计时间快照
        cpu_stat_map_[stats[i].cpu_name] = current;
    }

    munmap(addr, stat_size);
    close(fd);
    return;
}
}  // namespace monitor
