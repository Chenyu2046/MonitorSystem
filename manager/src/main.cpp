/**
 * @file main.cpp
 * @brief Manager 进程入口、gRPC 服务注册和关闭顺序。
 *
 * 启动链路为 GrpcServer push/query service -> HostManager 分片/持久化
 * worker -> gRPC ServerBuilder；关闭时先停止接收服务，再等待 Manager
 * 处理和 QueryManager 连接释放。SIGINT/SIGTERM 只设置退出标志。
 */

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

/** @brief 只设置异步信号安全的退出标志，资源回收留给主循环。 */
void HandleShutdownSignal(int) { g_shutdown_requested = 1; }

/** @brief 读取非空环境变量，供 Manager listen/MySQL 配置使用。 */
const char* GetEnvOrDefault(const char* name, const char* default_value) {
  const char* value = std::getenv(name);
  return value && value[0] != '\0' ? value : default_value;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);
  std::string listen_address = kDefaultListenAddress;

  // 解析监听地址；Worker push 的目标地址由部署配置决定，Manager 只在
  // 这里绑定本地 gRPC 入口。
  if (argc > 1) {
    listen_address = argv[1];
  }

  std::cout << "Starting Monitor Client (Manager Mode)..." << std::endl;
  std::cout << "Listening on: " << listen_address << std::endl;

  // 创建 gRPC 服务：SetMonitorInfo 负责接收 push，QueryService 负责历史
  // 与诊断查询，二者共享 HostManager/QueryManager 但边界不同。
  monitor::GrpcServerImpl service;

  // 创建 HostManager 并设置回调；RPC handler 只把消息交给有界 shard 队列。
  monitor::HostManager mgr;
  service.SetDataReceivedCallback(
      [&mgr](const monitor::proto::MonitorInfo& info,
             std::chrono::system_clock::time_point deadline,
             monitor::proto::MonitorFeedback* feedback) {
        return mgr.SubmitWithFeedback(info, deadline, feedback);
      });

  // 先启动 HostManager，确保 gRPC 开始接收前已有消费者和持久化能力状态。
  if (!mgr.Start()) {
    std::cerr << "Failed to initialize Manager data path" << std::endl;
    return 1;
  }

  // QueryManager 是历史查询的 MySQL 路径；未启用/初始化失败时，诊断
  // QueryService 仍可按接口逻辑回退到 HostManager 内存 incident。
  monitor::QueryManager query_mgr;
#ifdef ENABLE_MYSQL
  if (query_mgr.Init(
          GetEnvOrDefault("MONITOR_MYSQL_HOST", kDefaultMysqlHost),
          GetEnvOrDefault("MONITOR_MYSQL_USER", kDefaultMysqlUser),
          GetEnvOrDefault("MONITOR_MYSQL_PASSWORD", kDefaultMysqlPass),
          GetEnvOrDefault("MONITOR_MYSQL_DATABASE", kDefaultMysqlDb))) {
    std::cout << "QueryManager initialized successfully" << std::endl;
  } else {
    std::cerr << "QueryManager initialization failed" << std::endl;
    mgr.Stop();
    return 1;
  }
#endif

  // 创建查询服务，将数据库查询对象和实时内存对象注入 handler。
  monitor::QueryServiceImpl query_service(&query_mgr, &mgr);

  // 注册 Worker push 与 QueryService 两组 protobuf service，并启动 HTTP/2
  // gRPC server；实际业务处理仍在 Manager 后台线程。
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
