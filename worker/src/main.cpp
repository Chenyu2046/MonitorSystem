/**
 * @file main.cpp
 * @brief Worker 进程入口与生命周期控制。
 *
 * Worker 从命令行获取 Manager 地址和基础采样间隔，启动
 * MonitorPusher 周期采集并通过 gRPC 推送 MonitorInfo。主线程只负责
 * 接收 SIGINT/SIGTERM 后通知推送器停止；具体采集、诊断和发送工作由
 * MonitorPusher 管理，避免入口函数承载业务监控逻辑。
 */

#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "rpc/monitor_pusher.h"

constexpr char kDefaultManagerAddress[] = "localhost:50051";
constexpr int kDefaultPushInterval = 10;  // 秒

volatile std::sig_atomic_t g_shutdown_requested = 0;

/**
 * @brief 将异步信号转换为主循环可观察的退出请求。
 *
 * 信号处理器只写入 sig_atomic_t 标志，不执行日志、内存分配或线程
 * 操作；资源释放由正常控制流中的 MonitorPusher::Stop() 完成。
 */
void HandleShutdownSignal(int) { g_shutdown_requested = 1; }

/**
 * @brief 打印 Worker 的命令行参数约定。
 *
 * @param program 当前可执行文件名。
 */
void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " <manager_address> [interval_seconds]"
            << std::endl;
  std::cout << "  manager_address: 管理者服务器地址 (如 192.168.1.100:50051)"
            << std::endl;
  std::cout << "  interval_seconds: 推送间隔秒数 (默认 10)" << std::endl;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);
  std::string manager_address = kDefaultManagerAddress;
  int interval_seconds = kDefaultPushInterval;

  // 解析命令行参数。地址直接传给 gRPC channel，采样间隔只作为
  // MonitorPusher 的基础配置；诊断状态机仍可能根据异常状态自适应调整。
  if (argc > 1) {
    manager_address = argv[1];
  }
  if (argc > 2) {
    try {
      const std::string interval_arg = argv[2];
      std::size_t parsed_chars = 0;
      interval_seconds = std::stoi(interval_arg, &parsed_chars);
      if (parsed_chars != interval_arg.size()) {
        throw std::invalid_argument("interval suffix");
      }
    } catch (...) {
      std::cerr << "Invalid interval_seconds" << std::endl;
      return 1;
    }
  }

  std::cout << "Starting Monitor Server (Push Mode)..." << std::endl;
  std::cout << "Manager address: " << manager_address << std::endl;
  std::cout << "Push interval: " << interval_seconds << " seconds" << std::endl;

  // 创建并启动推送器。构造阶段建立采集器、诊断组件和发送队列，
  // Start() 再创建采集线程与发送线程。
  monitor::MonitorPusher pusher(manager_address, interval_seconds);
  if (!pusher.Start()) {
    return 1;
  }

  // 主线程不参与采集，只等待信号；这样不会与后台线程共享业务锁，
  // 收到退出请求后统一交给 Stop() 按线程生命周期顺序回收。
  std::cout << "Press Ctrl+C to exit." << std::endl;
  while (!g_shutdown_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Shutdown requested, stopping worker..." << std::endl;
  pusher.Stop();

  return 0;
}
