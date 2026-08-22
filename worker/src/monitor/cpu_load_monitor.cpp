/**
 * @file cpu_load_monitor.cpp
 * @brief CPU load average 的 mmap 采集和 /proc/loadavg fallback。
 *
 * 优先读取 /dev/cpu_load_monitor 暴露的共享结构；设备不存在、打开
 * 失败或 mmap 失败时读取 Linux 的 /proc/loadavg。输出是 1/3/15 分钟
 * load average，不应被解释成 CPU 使用率或逐核 CPU 百分比。
 */

#include "monitor/cpu_load_monitor.h"
#include "monitor/monitor_structs.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace monitor {

/**
 * @brief 从 /proc/loadavg 读取三个时间窗口的 load average。
 *
 * @return 成功解析三个浮点值时返回 true；文件打开或解析不完整时返回
 * false。调用方据此决定是否写入 Protobuf。
 */
static bool ReadLoadFromProc(float* load1, float* load3, float* load15) {
    FILE* fp = fopen("/proc/loadavg", "r");
    if (!fp) return false;
    
    // fscanf 读取三个浮点数，分别对应 1 分钟、5 分钟、15 分钟的平均负载；
    // 返回值为成功读取的项数，应该是 3
    int ret = fscanf(fp, "%f %f %f", load1, load3, load15);
    fclose(fp);
    return ret == 3;
}

void CpuLoadMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
    // 首先尝试从内核模块读取。mmap 只读共享区域，避免在普通采集路径
    // 中复制或维护额外的跨轮状态。
    int fd = open("/dev/cpu_load_monitor", O_RDONLY);
    if (fd >= 0) {
        size_t load_size = sizeof(struct cpu_load);
        void* addr = mmap(nullptr, load_size, PROT_READ, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED) {
            struct cpu_load info;
            memcpy(&info, addr, load_size);

            // 内核模块已经提供完整的三个窗口值，直接映射到同一个
            // MonitorInfo 子消息；这里不进行额外平均或单位换算。
            auto cpu_load_msg = monitor_info->mutable_cpu_load();
            cpu_load_msg->set_load_avg_1(info.load_avg_1);
            cpu_load_msg->set_load_avg_3(info.load_avg_3);
            cpu_load_msg->set_load_avg_15(info.load_avg_15);

            munmap(addr, load_size);
            close(fd);
            return;
        }
        close(fd);
    }
    
    // 后备方案：从 /proc/loadavg 读取。fallback 只改变数据源，不改变
    // Protobuf 字段语义或 MonitorPusher 后续处理方式。
    float load1, load3, load15;
    if (ReadLoadFromProc(&load1, &load3, &load15)) {
        auto cpu_load_msg = monitor_info->mutable_cpu_load();
        cpu_load_msg->set_load_avg_1(load1);
        cpu_load_msg->set_load_avg_3(load3);
        cpu_load_msg->set_load_avg_15(load15);
    }
}
}  // namespace monitor
