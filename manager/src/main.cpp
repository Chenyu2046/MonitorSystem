#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <grpcpp/server_builder.h>

#include "host_manager.h"
#include "query_manager.h"
#include "rpc/grpc_server.h"
#include "rpc/query_service.h"

constexpr char kDefaultListenAddress[] = "0.0.0.0:50051";
constexpr char kDefaultMysqlHost[] = "127.0.0.1";
constexpr char kDefaultMysqlUser[] = "monitor";
constexpr char kDefaultMysqlPass[] = "monitor123";
constexpr char kDefaultMysqlDb[] = "monitor_db";

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int) { g_shutdown_requested = 1; }

const char* GetEnvOrDefault(const char* name, const char* default_value) {
  const char* value = std::getenv(name);
  return value && value[0] != '\0' ? value : default_value;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);
  std::string listen_address = kDefaultListenAddress;

  // 解析命令行参数
  if (argc > 1) {
    listen_address = argv[1];
  }

  std::cout << "Starting Monitor Client (Manager Mode)..." << std::endl;
  std::cout << "Listening on: " << listen_address << std::endl;

  // 创建 gRPC 服务
  monitor::GrpcServerImpl service;

  // 创建 HostManager 并设置回调
  monitor::HostManager mgr;
  service.SetDataReceivedCallback(
      [&mgr](const monitor::proto::MonitorInfo& info) {
        return mgr.Submit(info);
      });

  // 启动 HostManager 后台处理
  mgr.Start();

  // 创建 QueryManager 并初始化
  monitor::QueryManager query_mgr;
#ifdef ENABLE_MYSQL
  if (query_mgr.Init(
          GetEnvOrDefault("MONITOR_MYSQL_HOST", kDefaultMysqlHost),
          GetEnvOrDefault("MONITOR_MYSQL_USER", kDefaultMysqlUser),
          GetEnvOrDefault("MONITOR_MYSQL_PASSWORD", kDefaultMysqlPass),
          GetEnvOrDefault("MONITOR_MYSQL_DATABASE", kDefaultMysqlDb))) {
    std::cout << "QueryManager initialized successfully" << std::endl;
  } else {
    std::cerr << "Warning: QueryManager initialization failed, "
              << "query service will not be available" << std::endl;
  }
#endif

  // 创建查询服务
  monitor::QueryServiceImpl query_service(&query_mgr, &mgr);

  // 启动 gRPC 服务器
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.RegisterService(&query_service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "Failed to start gRPC server" << std::endl;
    mgr.Stop();
    query_mgr.Close();
    return 1;
  }
  std::cout << "Monitor Client listening on " << listen_address << std::endl;
  std::cout << "Waiting for workers to push data..." << std::endl;
  std::cout << "Query service available for performance data queries"
            << std::endl;

  std::thread server_thread([&server] { server->Wait(); });
  while (!g_shutdown_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "Shutdown requested, stopping manager..." << std::endl;
  server->Shutdown();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  mgr.Stop();
  query_mgr.Close();

  return 0;
}
