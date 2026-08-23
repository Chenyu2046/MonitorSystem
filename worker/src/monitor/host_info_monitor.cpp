/**
 * @file host_info_monitor.cpp
 * @brief 主机名和主网卡 IPv4 标识采集实现。
 *
 * 主机标识属于每轮 MonitorInfo 的路由/归属信息，不是资源指标。首次
 * 采集通过 gethostname() 和网卡枚举获取值，之后复用缓存，避免每个
 * 采样周期重复访问系统接口。
 */

#include "monitor/host_info_monitor.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <dirent.h>

#include <fstream>
#include <sstream>
#include <string>
#include <cstring>

#include "monitor_info.pb.h"

namespace monitor {

std::string HostInfoMonitor::GetHostname() {
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
    return std::string(hostname);
  }
  return {};
}

std::string HostInfoMonitor::GetPrimaryIpAddress() {
  // 枚举接口只选择第一个非 loopback、非常见虚拟网卡的 IPv4 地址；
  // 这不是网络吞吐采集，也不保证返回所有地址。
  struct ifaddrs* ifaddr = nullptr;
  struct ifaddrs* ifa = nullptr;
  std::string result;

  if (getifaddrs(&ifaddr) == -1) {
    return "";
  }

  // 遍历所有网络接口
  for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) {
      continue;
    }

    // 只处理 IPv4 地址
    if (ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    // 跳过 loopback 接口
    if (strcmp(ifa->ifa_name, "lo") == 0) {
      continue;
    }

    // 跳过 docker/虚拟网卡（通常以 docker、veth、br- 开头）
    std::string ifname(ifa->ifa_name);
    if (ifname.find("docker") == 0 ||
        ifname.find("veth") == 0 ||
        ifname.find("br-") == 0 ||
        ifname.find("virbr") == 0) {
      continue;
    }

    // 获取 IP 地址
    struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str)) != nullptr) {
      result = ip_str;
      break;  // 找到第一个有效 IP 就返回
    }
  }

  freeifaddrs(ifaddr);
  return result;
}

CollectStatus HostInfoMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return CollectStatus::kError;
  }

  // 主机信息通常不变，只需获取一次并缓存；缓存只在采集线程访问，
  // 不与 gRPC 发送线程共享可变状态。
  if (!info_cached_) {
    cached_hostname_ = GetHostname();
    cached_ip_ = GetPrimaryIpAddress();
    info_cached_ = true;
  }
  if (cached_hostname_.empty()) return CollectStatus::kError;

  // 填充 HostInfo 消息
  auto* host_info = monitor_info->mutable_host_info();
  host_info->set_hostname(cached_hostname_);
  host_info->set_ip_address(cached_ip_);
  return CollectStatus::kOk;
}

}  // namespace monitor
