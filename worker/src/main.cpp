#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>

#include "rpc/monitor_pusher.h"
#include "runtime_config.h"

constexpr char kDefaultManagerAddress[] = "localhost:50051";
constexpr int kDefaultPushInterval = 10;  // 秒

volatile std::sig_atomic_t g_stop_requested = 0;

void RequestStop(int) { g_stop_requested = 1; }

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " <manager_address> [interval_seconds]"
            << std::endl;
  std::cout << "  manager_address: 管理者服务器地址 (如 192.168.1.100:50051)"
            << std::endl;
  std::cout << "  interval_seconds: 推送间隔秒数 (默认 10)" << std::endl;
}

int main(int argc, char* argv[]) {
  std::string manager_address = kDefaultManagerAddress;
  int interval_seconds = kDefaultPushInterval;

  // 解析命令行参数
  if (argc > 1) {
    manager_address = argv[1];
  }
  if (argc > 2) {
    try {
      const std::string interval_arg = argv[2];
      size_t parsed = 0;
      interval_seconds = std::stoi(interval_arg, &parsed);
      if (interval_seconds <= 0 || parsed != interval_arg.size()) {
        throw std::invalid_argument("invalid interval");
      }
    } catch (const std::exception&) {
      std::cerr << "Invalid interval_seconds: " << argv[2] << std::endl;
      PrintUsage(argv[0]);
      return 1;
    }
  }

  std::cout << "Starting Monitor Server (Push Mode)..." << std::endl;
  std::cout << "Manager address: " << manager_address << std::endl;
  std::cout << "Push interval: " << interval_seconds << " seconds" << std::endl;

  std::string error;
  auto credentials = monitor::runtime_config::BuildClientCredentials(&error);
  if (!credentials) {
    std::cerr << error << std::endl;
    return 1;
  }
  if (monitor::runtime_config::AllowInsecureGrpc()) {
    std::cerr << "WARNING: MONITOR_ALLOW_INSECURE_GRPC=1 is for local development only"
              << std::endl;
  }

  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);

  // 创建并启动推送器
  monitor::MonitorPusher pusher(manager_address, std::move(credentials),
                                 interval_seconds);
  pusher.Start();

  while (!g_stop_requested) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  pusher.Stop();
  std::cout << "Monitor Server stopped." << std::endl;

  return 0;
}
