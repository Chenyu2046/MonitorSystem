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

            CpuStat current;
            current.cpu_name = cpu_name;
            if (!(values >> current.user >> current.nice >> current.system >>
                  current.idle >> current.io_wait >> current.irq >>
                  current.soft_irq >> current.steal >> current.guest >>
                  current.guest_nice)) {
                continue;
            }

            const auto it = cpu_stat_map_.find(cpu_name);
            if (it != cpu_stat_map_.end()) {
                const CpuStat& old = it->second;
                const auto total =
                    (current.user - old.user) + (current.nice - old.nice) +
                    (current.system - old.system) +
                    (current.idle - old.idle) +
                    (current.io_wait - old.io_wait) +
                    (current.irq - old.irq) +
                    (current.soft_irq - old.soft_irq) +
                    (current.steal - old.steal);
                if (total != 0) {
                    auto* message = monitor_info->add_cpu_stat();
                    const auto busy =
                        (current.user - old.user) +
                        (current.system - old.system) +
                        (current.nice - old.nice) +
                        (current.irq - old.irq) +
                        (current.soft_irq - old.soft_irq) +
                        (current.steal - old.steal);
                    message->set_cpu_name(cpu_name);
                    message->set_cpu_percent(100.0F * busy / total);
                    message->set_usr_percent(
                        100.0F * (current.user - old.user) / total);
                    message->set_system_percent(
                        100.0F * (current.system - old.system) / total);
                    message->set_nice_percent(
                        100.0F * (current.nice - old.nice) / total);
                    message->set_idle_percent(
                        100.0F * (current.idle - old.idle) / total);
                    message->set_io_wait_percent(
                        100.0F * (current.io_wait - old.io_wait) / total);
                    message->set_irq_percent(
                        100.0F * (current.irq - old.irq) / total);
                    message->set_soft_irq_percent(
                        100.0F * (current.soft_irq - old.soft_irq) / total);
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
        auto it = cpu_stat_map_.find(stats[i].cpu_name);
        if (it != cpu_stat_map_.end()) {
          struct CpuStat old = it->second;
          float new_cpu_total_time = stats[i].user + stats[i].system +
                                     stats[i].idle + stats[i].nice +
                                     stats[i].iowait + stats[i].irq +
                                     stats[i].softirq + stats[i].steal;
          float old_cpu_total_time = old.user + old.system + old.idle + old.nice +
                                     old.io_wait + old.irq + old.soft_irq +
                                     old.steal;
          const float total_delta =
              new_cpu_total_time - old_cpu_total_time;
          if (total_delta > 0.0F) {
            auto* cpu_stat_msg = monitor_info->add_cpu_stat();
            const float new_cpu_busy_time = stats[i].user + stats[i].system +
                                             stats[i].nice + stats[i].irq +
                                             stats[i].softirq + stats[i].steal;
            const float old_cpu_busy_time = old.user + old.system + old.nice +
                                            old.irq + old.soft_irq + old.steal;

            cpu_stat_msg->set_cpu_name(stats[i].cpu_name);
            cpu_stat_msg->set_cpu_percent(
                (new_cpu_busy_time - old_cpu_busy_time) / total_delta *
                100.00F);
            cpu_stat_msg->set_usr_percent(
                (stats[i].user - old.user) / total_delta * 100.00F);
            cpu_stat_msg->set_system_percent(
                (stats[i].system - old.system) / total_delta * 100.00F);
            cpu_stat_msg->set_nice_percent(
                (stats[i].nice - old.nice) / total_delta * 100.00F);
            cpu_stat_msg->set_idle_percent(
                (stats[i].idle - old.idle) / total_delta * 100.00F);
            cpu_stat_msg->set_io_wait_percent(
                (stats[i].iowait - old.io_wait) / total_delta * 100.00F);
            cpu_stat_msg->set_irq_percent(
                (stats[i].irq - old.irq) / total_delta * 100.00F);
            cpu_stat_msg->set_soft_irq_percent(
                (stats[i].softirq - old.soft_irq) / total_delta * 100.00F);
          }
        }
        // 将内核结构体数据转换为内部 CpuStat 结构体
        CpuStat& cached = cpu_stat_map_[stats[i].cpu_name];
        cached.cpu_name = stats[i].cpu_name;
        cached.user = stats[i].user;
        cached.nice = stats[i].nice;
        cached.system = stats[i].system;
        cached.idle = stats[i].idle;
        cached.io_wait = stats[i].iowait;
        cached.irq = stats[i].irq;
        cached.soft_irq = stats[i].softirq;
        cached.steal = stats[i].steal;
        cached.guest = stats[i].guest;
        cached.guest_nice = stats[i].guest_nice;
    }

    munmap(addr, stat_size);
    close(fd);
    return;
}
}  // namespace monitor
