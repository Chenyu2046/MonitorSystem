#include "monitor/net_ebpf_monitor.h"

#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>

#include "monitor_info.pb.h"

// 包含生成的 skeleton 头文件
#include "monitor/net_stats.skel.h"

namespace monitor {

// 获取系统所有网卡的 ifindex
static std::vector<uint32_t> GetAllIfIndexes() {
  std::vector<uint32_t> indexes;
  DIR* dir = opendir("/sys/class/net");
  if (!dir) return indexes;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') continue;

    unsigned int ifindex = if_nametoindex(entry->d_name);
    if (ifindex > 0) {
      indexes.push_back(ifindex);
    }
  }
  closedir(dir);
  return indexes;
}

// TC qdisc 操作辅助函数
static int tc_qdisc_create_clsact(int ifindex) {
  // 使用 system 调用 tc 命令创建 clsact qdisc
  // 这是最简单可靠的方式
  char cmd[256];
  snprintf(
      cmd, sizeof(cmd),
      "tc qdisc add dev $(cat /sys/class/net/*/ifindex | "
      "while read idx; do [ \"$idx\" = \"%d\" ] && "
      "basename $(dirname $(grep -l $idx /sys/class/net/*/ifindex)); done) "
      "clsact 2>/dev/null || true",
      ifindex);

  // 更简单的方式：通过 ifindex 获取设备名
  char ifname[IF_NAMESIZE];
  if (if_indextoname(ifindex, ifname) == nullptr) {
    return -1;
  }

  snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null", ifname);
  return system(cmd);
}

static int tc_qdisc_delete_clsact(int ifindex) {
  char ifname[IF_NAMESIZE];
  if (if_indextoname(ifindex, ifname) == nullptr) {
    return -1;
  }

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s clsact 2>/dev/null", ifname);
  return system(cmd);
}

NetEbpfMonitor::NetEbpfMonitor() {
  last_update_ = std::chrono::steady_clock::now();
  fallback_monitor_ = std::make_unique<NetMonitor>();
  loaded_ = InitEbpf();
  if (!loaded_) {
    std::cerr << "NetEbpfMonitor: Failed to load eBPF program, "
              << "falling back to /proc/net/dev" << std::endl;
  }
}

NetEbpfMonitor::~NetEbpfMonitor() { CleanupEbpf(); }

bool NetEbpfMonitor::InitEbpf() {
  struct net_stats_bpf* skel = nullptr;
  int err;

  // 打开并加载 BPF 程序
  skel = net_stats_bpf__open();
  if (!skel) {
    std::cerr << "Failed to open BPF skeleton" << std::endl;
    return false;
  }

  err = net_stats_bpf__load(skel);
  if (err) {
    std::cerr << "Failed to load BPF program: " << strerror(-err) << std::endl;
    net_stats_bpf__destroy(skel);
    return false;
  }

  // 获取 map fd
  map_fd_ = bpf_map__fd(skel->maps.net_stats_map);
  if (map_fd_ < 0) {
    std::cerr << "Failed to get map fd" << std::endl;
    net_stats_bpf__destroy(skel);
    return false;
  }

  const int possible_cpus = libbpf_num_possible_cpus();
  if (possible_cpus <= 0) {
    std::cerr << "Failed to determine possible CPU count" << std::endl;
    net_stats_bpf__destroy(skel);
    map_fd_ = -1;
    return false;
  }
  per_cpu_stats_.resize(static_cast<size_t>(possible_cpus));

  // 获取所有网卡并附加 TC hook
  auto ifindexes = GetAllIfIndexes();
  int ingress_fd = bpf_program__fd(skel->progs.tc_ingress);
  int egress_fd = bpf_program__fd(skel->progs.tc_egress);

  for (uint32_t ifindex : ifindexes) {
    char ifname[IF_NAMESIZE];
    if (if_indextoname(ifindex, ifname) == nullptr) continue;

    // 跳过 loopback
    if (strcmp(ifname, "lo") == 0) continue;

    // 创建 clsact qdisc
    tc_qdisc_create_clsact(ifindex);

    // 使用 libbpf 的 TC attach API
    LIBBPF_OPTS(bpf_tc_hook, hook, .ifindex = static_cast<int>(ifindex),
                .attach_point = BPF_TC_INGRESS, );

    // 创建 hook
    err = bpf_tc_hook_create(&hook);
    if (err && err != -EEXIST) {
      std::cerr << "Failed to create TC hook for " << ifname << ": "
                << strerror(-err) << std::endl;
      continue;
    }

    // 附加 ingress 程序
    LIBBPF_OPTS(bpf_tc_opts, opts_in, .prog_fd = ingress_fd, );
    err = bpf_tc_attach(&hook, &opts_in);
    if (err) {
      std::cerr << "Failed to attach TC ingress for " << ifname << ": "
                << strerror(-err) << std::endl;
    } else {
      attached_ifindexes_.push_back(ifindex);
      std::cout << "Attached TC ingress to " << ifname << std::endl;
    }

    // 附加 egress 程序
    hook.attach_point = BPF_TC_EGRESS;
    LIBBPF_OPTS(bpf_tc_opts, opts_eg, .prog_fd = egress_fd, );
    err = bpf_tc_attach(&hook, &opts_eg);
    if (err) {
      std::cerr << "Failed to attach TC egress for " << ifname << ": "
                << strerror(-err) << std::endl;
    } else {
      std::cout << "Attached TC egress to " << ifname << std::endl;
    }
  }

  // 保存 skeleton 指针
  bpf_obj_ = reinterpret_cast<struct bpf_object*>(skel);

  if (attached_ifindexes_.empty()) {
    std::cerr << "No interfaces attached" << std::endl;
    net_stats_bpf__destroy(skel);
    bpf_obj_ = nullptr;
    return false;
  }

  std::cout << "NetEbpfMonitor: eBPF TC hook loaded successfully" << std::endl;
  return true;
}

void NetEbpfMonitor::CleanupEbpf() {
  // 分离 TC hook
  for (uint32_t ifindex : attached_ifindexes_) {
    LIBBPF_OPTS(bpf_tc_hook, hook, .ifindex = static_cast<int>(ifindex),
                .attach_point = BPF_TC_INGRESS, );

    LIBBPF_OPTS(bpf_tc_opts, opts);
    bpf_tc_detach(&hook, &opts);

    hook.attach_point = BPF_TC_EGRESS;
    bpf_tc_detach(&hook, &opts);

    // 可选：删除 clsact qdisc
    // tc_qdisc_delete_clsact(ifindex);
  }
  attached_ifindexes_.clear();

  if (bpf_obj_) {
    net_stats_bpf__destroy(reinterpret_cast<struct net_stats_bpf*>(bpf_obj_));
    bpf_obj_ = nullptr;
  }
  map_fd_ = -1;
  loaded_ = false;
  per_cpu_stats_.clear();
}

static bool ReadErrorCounters(const std::string& ifname, uint64_t* err_in,
                              uint64_t* err_out, uint64_t* drop_in,
                              uint64_t* drop_out) {
  std::ifstream file("/proc/net/dev");
  std::string line;
  std::getline(file, line);
  std::getline(file, line);

  while (std::getline(file, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    std::istringstream name_stream(line.substr(0, colon));
    std::string candidate;
    name_stream >> candidate;
    if (candidate != ifname) {
      continue;
    }

    std::istringstream values(line.substr(colon + 1));
    uint64_t ignored = 0;
    uint64_t rcv_bytes = 0;
    uint64_t rcv_packets = 0;
    uint64_t snd_bytes = 0;
    uint64_t snd_packets = 0;
    values >> rcv_bytes >> rcv_packets >> *err_in >> *drop_in;
    values >> ignored >> ignored >> ignored >> ignored;
    values >> snd_bytes >> snd_packets >> *err_out >> *drop_out;
    return !values.fail();
  }
  return false;
}

std::string NetEbpfMonitor::GetIfName(uint32_t ifindex) {
  // 先查缓存
  auto it = ifname_cache_.find(ifindex);
  if (it != ifname_cache_.end()) {
    return it->second;
  }

  // 使用 if_indextoname 获取网卡名
  char ifname[IF_NAMESIZE];
  if (if_indextoname(ifindex, ifname) != nullptr) {
    std::string name(ifname);
    ifname_cache_[ifindex] = name;
    return name;
  }

  return "";
}

bool NetEbpfMonitor::ReadAggregatedStats(uint32_t ifindex, NetStats* stats) {
  if (!stats || per_cpu_stats_.empty() ||
      bpf_map_lookup_elem(map_fd_, &ifindex, per_cpu_stats_.data()) != 0) {
    return false;
  }

  *stats = {};
  for (const auto& cpu_stats : per_cpu_stats_) {
    stats->rcv_bytes += cpu_stats.rcv_bytes;
    stats->rcv_packets += cpu_stats.rcv_packets;
    stats->snd_bytes += cpu_stats.snd_bytes;
    stats->snd_packets += cpu_stats.snd_packets;
  }
  return true;
}

void NetEbpfMonitor::UpdateFallback(monitor::proto::MonitorInfo* monitor_info) {
  if (fallback_monitor_) {
    fallback_monitor_->UpdateOnce(monitor_info);
  }
}

void NetEbpfMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return;
  }

  if (!loaded_ || map_fd_ < 0) {
    UpdateFallback(monitor_info);
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_)
          .count();

  // 避免除零
  if (duration == 0) {
    duration = 1;
  }

  struct InterfaceStats {
    uint32_t ifindex;
    NetStats stats;
  };
  std::vector<InterfaceStats> interfaces;
  uint32_t current_key = 0;
  uint32_t next_key = 0;
  const void* key = nullptr;
  int result = 0;

  while ((result = bpf_map_get_next_key(map_fd_, key, &next_key)) == 0) {
    NetStats stats;
    if (!ReadAggregatedStats(next_key, &stats)) {
      result = -EIO;
      break;
    }
    interfaces.push_back({next_key, stats});
    current_key = next_key;
    key = &current_key;
  }

  if (result != -ENOENT) {
    ++consecutive_read_failures_;
    std::cerr << "NetEbpfMonitor: failed to read eBPF map (failure "
              << consecutive_read_failures_ << ")" << std::endl;
    if (consecutive_read_failures_ >= 3) {
      std::cerr << "NetEbpfMonitor: switching to /proc/net/dev after repeated "
                   "map read failures"
                << std::endl;
      CleanupEbpf();
    }
    UpdateFallback(monitor_info);
    return;
  }
  consecutive_read_failures_ = 0;

  for (const auto& interface_stats : interfaces) {
    std::string ifname = GetIfName(interface_stats.ifindex);
    if (ifname.empty() || ifname == "lo") {
      continue;
    }

    const auto& stats = interface_stats.stats;
    auto* net_info = monitor_info->add_net_info();
    net_info->set_name(ifname);
    uint64_t err_in = 0;
    uint64_t err_out = 0;
    uint64_t drop_in = 0;
    uint64_t drop_out = 0;
    if (ReadErrorCounters(ifname, &err_in, &err_out, &drop_in, &drop_out)) {
      net_info->set_err_in(err_in);
      net_info->set_err_out(err_out);
      net_info->set_drop_in(drop_in);
      net_info->set_drop_out(drop_out);
    } else {
      std::cerr << "NetEbpfMonitor: failed to read error counters for "
                << ifname << std::endl;
    }

    // 查找上次的缓存数据
    auto cache_it = cache_.find(interface_stats.ifindex);
    if (cache_it != cache_.end()) {
      const auto& old = cache_it->second;
      auto old_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - old.timestamp)
                              .count();

      if (old_duration > 0) {
        // 计算速率：协议字段要求字节速率使用 kB/s。
        const uint64_t rcv_diff = stats.rcv_bytes >= old.rcv_bytes
                                      ? stats.rcv_bytes - old.rcv_bytes
                                      : stats.rcv_bytes;
        const uint64_t snd_diff = stats.snd_bytes >= old.snd_bytes
                                      ? stats.snd_bytes - old.snd_bytes
                                      : stats.snd_bytes;
        const uint64_t rcv_pkt_diff = stats.rcv_packets >= old.rcv_packets
                                          ? stats.rcv_packets - old.rcv_packets
                                          : stats.rcv_packets;
        const uint64_t snd_pkt_diff = stats.snd_packets >= old.snd_packets
                                          ? stats.snd_packets - old.snd_packets
                                          : stats.snd_packets;

        const double seconds = old_duration / 1000.0;
        net_info->set_rcv_rate(rcv_diff / 1024.0 / seconds);
        net_info->set_send_rate(snd_diff / 1024.0 / seconds);
        net_info->set_rcv_packets_rate(rcv_pkt_diff / seconds);
        net_info->set_send_packets_rate(snd_pkt_diff / seconds);
      }
    } else {
      // 首次采集，速率为 0
      net_info->set_rcv_rate(0);
      net_info->set_send_rate(0);
      net_info->set_rcv_packets_rate(0);
      net_info->set_send_packets_rate(0);
    }

    // 更新缓存
    cache_[interface_stats.ifindex] = {stats.rcv_bytes, stats.rcv_packets,
                                       stats.snd_bytes, stats.snd_packets, now};
  }

  last_update_ = now;
}

void NetEbpfMonitor::Stop() { CleanupEbpf(); }

}  // namespace monitor
