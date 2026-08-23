/**
 * @file net_ebpf_monitor.cpp
 * @brief 实现 TC/eBPF 网卡流量采集、Per-CPU 汇总和速率计算。
 *
 * 初始化阶段为每个非 loopback 网卡挂载 TC ingress/egress 程序；采集阶段
 * 读取累计计数、与上次快照做差，再按实际时间间隔换算成协议需要的速率。
 */

#include "monitor/net_ebpf_monitor.h"

#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <algorithm>
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
#include "monitor/net_monitor.h"

// 包含生成的 skeleton 头文件
#include "monitor/net_stats.skel.h"

namespace monitor {

// 枚举 /sys/class/net，得到当前系统可见网卡的 ifindex。
static std::vector<uint32_t> GetAllIfIndexes() {
  std::vector<uint32_t> indexes;
  DIR* dir = opendir("/sys/class/net");
  if (!dir) return indexes;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') continue;
    if (strcmp(entry->d_name, "lo") == 0) continue;

    unsigned int ifindex = if_nametoindex(entry->d_name);
    if (ifindex > 0) {
      indexes.push_back(ifindex);
    }
  }
  closedir(dir);
  return indexes;
}

// 创建指定网卡的 clsact qdisc，为 TC ingress/egress 挂载提供容器。
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

  snprintf(cmd, sizeof(cmd),
           "tc qdisc add dev %s clsact 2>/dev/null || "
           "tc qdisc show dev %s clsact >/dev/null 2>&1",
           ifname, ifname);
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
}

NetEbpfMonitor::~NetEbpfMonitor() { CleanupEbpf(); }

bool NetEbpfMonitor::InitEbpf() {
  struct net_stats_bpf* skel = nullptr;
  int err;

  // 先打开 skeleton，再加载程序和 map；任一步失败都使该 source 不可用。
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

  // 保存 Per-CPU 网卡计数 map 的 fd，后续按 ifindex 读取。
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

  if (bpf_map__max_entries(skel->maps.net_stats_map) < 1 ||
      bpf_map__max_entries(skel->maps.net_stats_map) < 64) {
    net_stats_bpf__destroy(skel);
    map_fd_ = -1;
    return false;
  }

  // 为每个非 loopback 网卡创建 clsact，并分别挂载 ingress/egress 程序。
  auto ifindexes = GetAllIfIndexes();
  if (ifindexes.size() > 64 || ifindexes.empty()) {
    net_stats_bpf__destroy(skel);
    map_fd_ = -1;
    return false;
  }
  int ingress_fd = bpf_program__fd(skel->progs.tc_ingress);
  int egress_fd = bpf_program__fd(skel->progs.tc_egress);

  for (uint32_t ifindex : ifindexes) {
    char ifname[IF_NAMESIZE];
    if (if_indextoname(ifindex, ifname) == nullptr) {
      net_stats_bpf__destroy(skel);
      map_fd_ = -1;
      CleanupEbpf();
      return false;
    }

    // 跳过 loopback
    if (strcmp(ifname, "lo") == 0) continue;

    // 创建 clsact qdisc；任一接口失败都不能留下部分 coverage。
    if (tc_qdisc_create_clsact(ifindex) != 0) {
      net_stats_bpf__destroy(skel);
      map_fd_ = -1;
      CleanupEbpf();
      return false;
    }

    // 使用 libbpf 的 TC attach API
    LIBBPF_OPTS(bpf_tc_hook, hook, .ifindex = static_cast<int>(ifindex),
                .attach_point = BPF_TC_INGRESS, );

    // 创建 hook
    err = bpf_tc_hook_create(&hook);
    if (err && err != -EEXIST) {
      std::cerr << "Failed to create TC hook for " << ifname << ": "
                << strerror(-err) << std::endl;
      net_stats_bpf__destroy(skel);
      map_fd_ = -1;
      CleanupEbpf();
      return false;
    }

    // 附加 ingress 程序
    LIBBPF_OPTS(bpf_tc_opts, opts_in, .prog_fd = ingress_fd, );
    err = bpf_tc_attach(&hook, &opts_in);
    if (err) {
      std::cerr << "Failed to attach TC ingress for " << ifname << ": "
                << strerror(-err) << std::endl;
      net_stats_bpf__destroy(skel);
      map_fd_ = -1;
      CleanupEbpf();
      return false;
    }
    // 从 ingress 成功开始就登记当前接口，确保后续 egress 失败时清理
    // 不会遗留半挂载的 ingress hook。
    attached_ifindexes_.push_back(ifindex);

    // 附加 egress 程序
    hook.attach_point = BPF_TC_EGRESS;
    LIBBPF_OPTS(bpf_tc_opts, opts_eg, .prog_fd = egress_fd, );
    err = bpf_tc_attach(&hook, &opts_eg);
    if (err) {
      std::cerr << "Failed to attach TC egress for " << ifname << ": "
                << strerror(-err) << std::endl;
      net_stats_bpf__destroy(skel);
      map_fd_ = -1;
      CleanupEbpf();
      return false;
    }
    interface_set_.push_back(ifindex);
  }

  // 将 skeleton 交给成员管理，CleanupEbpf 负责统一销毁。
  bpf_obj_ = reinterpret_cast<struct bpf_object*>(skel);

  if (attached_ifindexes_.size() != ifindexes.size()) {
    std::cerr << "No interfaces attached" << std::endl;
    net_stats_bpf__destroy(skel);
    bpf_obj_ = nullptr;
    return false;
  }

  std::cout << "NetEbpfMonitor: eBPF TC hook loaded successfully" << std::endl;
  return true;
}

bool NetEbpfMonitor::Init() {
  loaded_ = InitEbpf();
  return loaded_;
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
  interface_set_.clear();

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
  if (!file.is_open()) return false;
  std::string line;
  if (!std::getline(file, line) || !std::getline(file, line)) return false;

  while (std::getline(file, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    NetRawSample sample;
    if (!ParseNetDevLine(line, &sample)) return false;
    if (sample.name != ifname) continue;
    *err_in = sample.err_in;
    *err_out = sample.err_out;
    *drop_in = sample.drop_in;
    *drop_out = sample.drop_out;
    return true;
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
  if (!stats || per_cpu_stats_.empty()) {
    return false;
  }
  if (bpf_map_lookup_elem(map_fd_, &ifindex, per_cpu_stats_.data()) != 0) {
    if (errno == ENOENT) {
      *stats = {};
      return true;
    }
    return false;
  }

  // Per-CPU map 每个槽位对应一个 CPU，需要在用户态相加后再计算速率。
  *stats = {};
  for (const auto& cpu_stats : per_cpu_stats_) {
    stats->rcv_bytes += cpu_stats.rcv_bytes;
    stats->rcv_packets += cpu_stats.rcv_packets;
    stats->snd_bytes += cpu_stats.snd_bytes;
    stats->snd_packets += cpu_stats.snd_packets;
  }
  return true;
}

CollectStatus NetEbpfMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info || !loaded_ || map_fd_ < 0) return CollectStatus::kError;
  auto current_interfaces = GetAllIfIndexes();
  std::sort(current_interfaces.begin(), current_interfaces.end());
  auto expected_interfaces = interface_set_;
  std::sort(expected_interfaces.begin(), expected_interfaces.end());
  if (current_interfaces != expected_interfaces) return CollectStatus::kError;

  const auto now = std::chrono::steady_clock::now();
  struct InterfaceStats {
    uint32_t ifindex;
    NetStats stats;
    NetStatCache previous{};
    bool has_previous = false;
    uint64_t err_in = 0;
    uint64_t err_out = 0;
    uint64_t drop_in = 0;
    uint64_t drop_out = 0;
  };
  std::vector<InterfaceStats> current;
  bool not_ready = false;
  for (const uint32_t ifindex : interface_set_) {
    InterfaceStats value;
    value.ifindex = ifindex;
    if (!ReadAggregatedStats(ifindex, &value.stats)) return CollectStatus::kError;
    const std::string ifname = GetIfName(ifindex);
    if (ifname.empty() || !ReadErrorCounters(ifname, &value.err_in,
                                              &value.err_out, &value.drop_in,
                                              &value.drop_out)) {
      return CollectStatus::kError;
    }
    const auto it = cache_.find(ifindex);
    if (it == cache_.end()) {
      not_ready = true;
    } else {
      const auto& previous = it->second;
      value.previous = previous;
      value.has_previous = true;
      if (value.stats.rcv_bytes < previous.rcv_bytes ||
          value.stats.rcv_packets < previous.rcv_packets ||
          value.stats.snd_bytes < previous.snd_bytes ||
          value.stats.snd_packets < previous.snd_packets ||
          std::chrono::duration<double>(now - previous.timestamp).count() <=
              0) {
        not_ready = true;
      }
    }
    current.push_back(value);
  }

  for (const auto& value : current) {
    cache_[value.ifindex] = {value.stats.rcv_bytes, value.stats.rcv_packets,
                             value.stats.snd_bytes, value.stats.snd_packets,
                             now};
  }
  if (not_ready) return CollectStatus::kNotReady;

  for (const auto& value : current) {
    if (!value.has_previous) return CollectStatus::kNotReady;
    const auto& previous = value.previous;
    const double seconds = std::chrono::duration<double>(
                               now - previous.timestamp)
                               .count();
    auto* net = monitor_info->add_net_info();
    net->set_name(GetIfName(value.ifindex));
    net->set_rcv_rate((value.stats.rcv_bytes - previous.rcv_bytes) /
                      1024.0 / seconds);
    net->set_send_rate((value.stats.snd_bytes - previous.snd_bytes) /
                       1024.0 / seconds);
    net->set_rcv_packets_rate(
        (value.stats.rcv_packets - previous.rcv_packets) / seconds);
    net->set_send_packets_rate(
        (value.stats.snd_packets - previous.snd_packets) / seconds);
    net->set_err_in(value.err_in);
    net->set_err_out(value.err_out);
    net->set_drop_in(value.drop_in);
    net->set_drop_out(value.drop_out);
  }
  return CollectStatus::kOk;
}

void NetEbpfMonitor::Stop() { CleanupEbpf(); }

}  // namespace monitor
