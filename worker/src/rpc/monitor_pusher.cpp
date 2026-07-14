#include "rpc/monitor_pusher.h"

#include <iostream>
#include <chrono>
#include <utility>

namespace monitor {

namespace {
constexpr auto kPushDeadline = std::chrono::seconds(5);
}

MonitorPusher::MonitorPusher(const std::string& manager_address,
                             std::shared_ptr<grpc::ChannelCredentials> credentials,
                             int interval_seconds)
    : manager_address_(manager_address),
      interval_seconds_(interval_seconds),
      running_(false) {
  // 创建 gRPC channel 和 stub
  auto channel = grpc::CreateChannel(manager_address, std::move(credentials));
  stub_ = monitor::proto::GrpcManager::NewStub(channel);

  // 创建指标采集器
  collector_ = std::make_unique<MetricCollector>();
}

MonitorPusher::~MonitorPusher() {
  Stop();
}

void MonitorPusher::Start() {
  if (running_) {
    return;
  }
  running_ = true;
  // 启动一个后台线程，在当前 MonitorPusher 对象上执行 PushLoop。
  // PushLoop 会循环调用 PushOnce，所以这里创建线程后，周期 Push 流程就开始了。
  thread_ = std::make_unique<std::thread>(&MonitorPusher::PushLoop, this);
  std::cout << "MonitorPusher started, pushing to " << manager_address_
            << " every " << interval_seconds_ << " seconds" << std::endl;
}

void MonitorPusher::Stop() {
  running_ = false;
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void MonitorPusher::PushLoop() {
  while (running_) {
    if (!PushOnce()) {
      std::cerr << "Failed to push monitor data to " << manager_address_
                << std::endl;
    }

    // 等待指定间隔
    for (int i = 0; i < interval_seconds_ && running_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

bool MonitorPusher::PushOnce() {
  // 采集监控数据
  monitor::proto::MonitorInfo info;
  collector_->CollectAll(&info);

  // 推送数据
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kPushDeadline);
  google::protobuf::Empty response;

  grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);

  if (status.ok()) {
    return true;
  } else {
    std::cerr << ">>> Push failed: " << status.error_message() << " <<<" << std::endl;
    return false;
  }
}

}  // namespace monitor
