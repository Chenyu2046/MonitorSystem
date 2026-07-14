#include <grpc/grpc.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "host_manager.h"
#include "query_manager.h"
#include "rpc/grpc_server.h"
#include "rpc/query_service.h"
#include "runtime_config.h"

constexpr char kDefaultListenAddress[] = "0.0.0.0:50051";
int main(int argc, char* argv[]) {
  std::string listen_address = kDefaultListenAddress;

  // 解析命令行参数
  if (argc > 1) {
    listen_address = argv[1];
  }

  std::cout << "Starting Monitor Client (Manager Mode)..." << std::endl;
  std::cout << "Listening on: " << listen_address << std::endl;

  std::string error;
  monitor::runtime_config::DatabaseConfig database_config;
  if (!monitor::runtime_config::LoadDatabaseConfig(&database_config, &error)) {
    std::cerr << error << std::endl;
    return 1;
  }
  monitor::runtime_config::AuthorizationConfig authorization;
  if (!monitor::runtime_config::LoadAuthorizationConfig(&authorization, &error)) {
    std::cerr << error << std::endl;
    return 1;
  }
  auto server_credentials =
      monitor::runtime_config::BuildServerCredentials(&error);
  if (!server_credentials) {
    std::cerr << error << std::endl;
    return 1;
  }
  if (monitor::runtime_config::AllowInsecureGrpc()) {
    std::cerr << "WARNING: MONITOR_ALLOW_INSECURE_GRPC=1 is for local development only"
              << std::endl;
  }

  // 创建 gRPC 服务
  monitor::GrpcServerImpl service(authorization);

  // 创建 HostManager 并设置回调
  monitor::HostManager mgr(database_config);
  service.SetDataReceivedCallback(
      [&mgr](const monitor::proto::MonitorInfo& info) {
        mgr.OnDataReceived(info);
      });

  // 启动 HostManager 后台处理
  mgr.Start();

  // 创建 QueryManager 并初始化
  monitor::QueryManager query_mgr;
#ifdef ENABLE_MYSQL
  if (query_mgr.Init(database_config.host, database_config.user,
                     database_config.password, database_config.database)) {
    std::cout << "QueryManager initialized successfully" << std::endl;
  } else {
    std::cerr << "QueryManager initialization failed" << std::endl;
    return 1;
  }
#endif

  // 创建查询服务
  monitor::QueryServiceImpl query_service(&query_mgr, authorization);

  // 启动 gRPC 服务器
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, server_credentials);
  builder.RegisterService(&service);
  builder.RegisterService(&query_service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "Failed to start gRPC server" << std::endl;
    return 1;
  }
  std::cout << "Monitor Client listening on " << listen_address << std::endl;
  std::cout << "Waiting for workers to push data..." << std::endl;
  std::cout << "Query service available for performance data queries" << std::endl;

  server->Wait();

  return 0;
}
