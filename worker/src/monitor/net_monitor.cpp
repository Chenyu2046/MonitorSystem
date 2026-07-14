#include "monitor/net_monitor.h"
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

struct NetStat {
    std::string name;
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    uint64_t err_in;
    uint64_t err_out;
    uint64_t drop_in;
    uint64_t drop_out;
};

// 从 /proc/net/dev 读取网络统计信息
static std::vector<NetStat> get_net_stats_from_proc() {
    std::vector<NetStat> stats;
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return stats;

    std::string line;
    // 跳过前两行标题
    std::getline(file, line);
    std::getline(file, line);

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        NetStat stat;
        
        // 解析接口名
        std::string iface;
        iss >> iface;
        if (iface.empty()) continue;
        
        // 移除末尾的冒号
        if (iface.back() == ':') {
            iface.pop_back();
        }
        
        // 跳过 lo 接口
        if (iface == "lo") continue;
        
        stat.name = iface;
        
        // 解析接收统计: bytes packets errs drop fifo frame compressed multicast
        iss >> stat.rcv_bytes >> stat.rcv_packets >> stat.err_in >> stat.drop_in;
        uint64_t dummy;
        iss >> dummy >> dummy >> dummy >> dummy;  // fifo frame compressed multicast
        
        // 解析发送统计: bytes packets errs drop fifo colls carrier compressed
        iss >> stat.snd_bytes >> stat.snd_packets >> stat.err_out >> stat.drop_out;
        
        stats.push_back(stat);
    }
    
    return stats;
}

void NetMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
    auto now = std::chrono::steady_clock::now();
    auto stats = get_net_stats_from_proc();
    std::unordered_set<std::string> active_interfaces;

    for (const auto& stat : stats) {
        active_interfaces.insert(stat.name);
        auto it = last_net_info_.find(stat.name);
        double rcv_rate = 0, rcv_packets_rate = 0, send_rate = 0, send_packets_rate = 0;

        if (it != last_net_info_.end()) {
            const NetInfo& last = it->second;
            double dt = std::chrono::duration<double>(now - last.timepoint).count();
            if (dt > 0) {
                // 网卡重建会让累计计数器回退；把当前值当作新计数周期起点，
                // 避免无符号下溢生成极大速率。
                const uint64_t rcv_bytes = stat.rcv_bytes >= last.rcv_bytes
                                               ? stat.rcv_bytes - last.rcv_bytes
                                               : stat.rcv_bytes;
                const uint64_t rcv_packets = stat.rcv_packets >= last.rcv_packets
                                                 ? stat.rcv_packets - last.rcv_packets
                                                 : stat.rcv_packets;
                const uint64_t snd_bytes = stat.snd_bytes >= last.snd_bytes
                                               ? stat.snd_bytes - last.snd_bytes
                                               : stat.snd_bytes;
                const uint64_t snd_packets = stat.snd_packets >= last.snd_packets
                                                 ? stat.snd_packets - last.snd_packets
                                                 : stat.snd_packets;
                rcv_rate = rcv_bytes / 1000.0 / dt; // kB/s
                rcv_packets_rate = rcv_packets / dt;
                send_rate = snd_bytes / 1000.0 / dt; // kB/s
                send_packets_rate = snd_packets / dt;
            }
        }

        // 填充 protobuf
        auto net_info = monitor_info->add_net_info();
        net_info->set_name(stat.name);
        net_info->set_rcv_rate(rcv_rate);
        net_info->set_rcv_packets_rate(rcv_packets_rate);
        net_info->set_send_rate(send_rate);
        net_info->set_send_packets_rate(send_packets_rate);
        // 错误和丢弃统计
        net_info->set_err_in(stat.err_in);
        net_info->set_err_out(stat.err_out);
        net_info->set_drop_in(stat.drop_in);
        net_info->set_drop_out(stat.drop_out);

        // 更新缓存
        last_net_info_[stat.name] = NetInfo{
            stat.name, stat.rcv_bytes, stat.rcv_packets, stat.snd_bytes, stat.snd_packets,
            stat.err_in, stat.err_out, stat.drop_in, stat.drop_out, now
        };
    }
    for (auto it = last_net_info_.begin(); it != last_net_info_.end();) {
        if (active_interfaces.find(it->first) == active_interfaces.end()) {
            it = last_net_info_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace monitor
