#pragma once

/**
 * @file user_monitor.h
 * @brief 当前采集进程用户身份的监控接口。
 */

#include <string>
#include "monitor/monitor_inter.h"

namespace monitor {

/**
 * 用户信息监控器
 * 
 * 通过系统调用获取当前进程的实际用户ID(UID)，
 * 然后解析 /etc/passwd 文件查找对应的用户名。
 * 
 * 这种方式比 getenv("USER") 更可靠，不依赖环境变量，
 * 在容器环境下也能准确获取用户信息。
 */
class UserMonitor : public MonitorInter {
 public:
  /** 使用默认配置构造用户监控器。 */
  UserMonitor() = default;
  /** 用户监控器不持有需要释放的外部资源。 */
  ~UserMonitor() override = default;

  /** 采集当前进程 UID 对应的用户名。 */
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  /** 用户信息采集没有持续运行资源需要停止。 */
  void Stop() override {}

 private:
  /**
   * 根据 UID 从 /etc/passwd 文件中查找用户名
   * 模拟 getpwuid() 的核心功能
   * 
   * @param uid 用户ID
   * @return 用户名，如果未找到返回空字符串
   */
  std::string GetUsernameByUid(uid_t uid);
};

}  // namespace monitor
