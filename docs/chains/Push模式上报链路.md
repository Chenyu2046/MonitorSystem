# Push 模式上报链路

## 1. 结论

“为什么选择 Push 模式而不是 Pull 模式”这段表述和当前代码主链路基本一致。

准确说法是：

- 当前 worker 主运行路径是主动 Push，不是 Manager 主动轮询。
- worker 本地采集后组装 `MonitorInfo`，通过 gRPC `SetMonitorInfo` 推给 manager。
- manager 收到后缓存最近一次原始数据，回调进入 `HostManager::OnDataReceived`，再做评分、更新在线状态和写 MySQL。
- worker 推送失败后只打印错误并返回失败，下一轮继续采集和推送；当前没有本地队列、落盘缓存、重试退避或补传机制。
- manager 侧后台线程每 60 秒检查一次在线主机状态，并删除超过 60 秒没有更新的主机。

需要注意两个边界：

- `proto/monitor_info.proto` 里仍保留了 `GetMonitorInfo` 这个 Pull 风格接口，manager 侧也有实现，但当前 worker 主链路没有使用它。
- manager 不只是接收、评分、入库；当前进程里还注册了查询服务，并缓存了最近一次原始 `MonitorInfo`。面试时可以把“接收、评分、入库”作为主写链路讲，但不要说 manager 只有这三个职责。

## 2. 为什么 Push 更适合当前场景

这个项目的场景是多台被监控机器各自运行 worker，worker 本地周期性采集 CPU、内存、磁盘、网络、软中断等指标。

如果用 Pull 模式，manager 需要维护所有 worker 的地址、端口和可达性，然后按周期主动轮询每个节点。这样会带来几个问题：

- 节点扩缩容时，manager 需要及时更新目标列表。
- worker 在 NAT、内网、防火墙后面时，manager 不一定能主动访问。
- manager 要承担调度轮询、连接失败处理、慢节点隔离等复杂度。
- worker 本来就知道自己的采样周期，主动上报更直接。

当前代码选择 Push 模式后，worker 只需要知道 manager 地址；manager 只需要监听 gRPC 端口并处理上报数据。链路更符合“Agent 周期采集，中心端接收处理”的模型。

## 3. 代码主链路

```text
worker main
    -> 创建 MonitorPusher
    -> MonitorPusher::Start
    -> 后台线程 MonitorPusher::PushLoop
    -> MonitorPusher::PushOnce
    -> MetricCollector::CollectAll
    -> 组装 MonitorInfo
    -> GrpcManager::SetMonitorInfo
    -> manager GrpcServerImpl::SetMonitorInfo
    -> callback 调用 HostManager::OnDataReceived
    -> CalcScore
    -> 更新 host_scores_
    -> WriteToMysql
```

## 4. Worker 侧：周期采集并主动 Push

worker 入口在 [worker/src/main.cpp:19](../../worker/src/main.cpp#L19)。

启动时会解析 manager 地址和推送间隔：

```cpp
std::string manager_address = kDefaultManagerAddress;
int interval_seconds = kDefaultPushInterval;
```

默认 manager 地址和推送间隔定义在 [worker/src/main.cpp:8](../../worker/src/main.cpp#L8)：

```cpp
constexpr char kDefaultManagerAddress[] = "localhost:50051";
constexpr int kDefaultPushInterval = 10;
```

随后创建并启动 `MonitorPusher`：

```cpp
monitor::MonitorPusher pusher(manager_address, interval_seconds);
pusher.Start();
```

`MonitorPusher::Start()` 位于 [worker/src/rpc/monitor_pusher.cpp:26](../../worker/src/rpc/monitor_pusher.cpp#L26)，它会启动后台线程：

```cpp
thread_ = std::make_unique<std::thread>(&MonitorPusher::PushLoop, this);
```

后台线程执行 [`MonitorPusher::PushLoop()`](../../worker/src/rpc/monitor_pusher.cpp#L43)，循环调用 `PushOnce()`：

```cpp
while (running_) {
  if (!PushOnce()) {
    std::cerr << "Failed to push monitor data to " << manager_address_
              << std::endl;
  }

  for (int i = 0; i < interval_seconds_ && running_; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
```

可以看到，失败后只是打印日志，然后等待下一轮。这里没有把失败的 `MonitorInfo` 放进队列，也没有补传逻辑。

真正组装并发送数据的是 [`MonitorPusher::PushOnce()`](../../worker/src/rpc/monitor_pusher.cpp#L57)：

```cpp
monitor::proto::MonitorInfo info;
collector_->CollectAll(&info);
grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);
```

这说明 worker 是先本地采集并填充 `MonitorInfo`，再主动调用 gRPC `SetMonitorInfo`。

## 5. Protobuf 协议：MonitorInfo 和 SetMonitorInfo

统一上报结构定义在 [proto/monitor_info.proto:86](../../proto/monitor_info.proto#L86)：

```proto
message MonitorInfo{
  string name = 1;
  HostInfo host_info = 2;
  repeated SoftIrq soft_irq = 4;
  CpuLoad cpu_load = 5;
  repeated CpuStat cpu_stat = 6;
  MemInfo mem_info = 7;
  repeated NetInfo net_info = 8;
  repeated DiskInfo disk_info = 9;
}
```

Push RPC 定义在 [proto/monitor_info.proto:97](../../proto/monitor_info.proto#L97)：

```proto
service GrpcManager {
  rpc SetMonitorInfo(MonitorInfo) returns (google.protobuf.Empty) {
  }
}
```

同一个 proto 里也保留了 `GetMonitorInfo`：

```proto
rpc GetMonitorInfo(google.protobuf.Empty) returns (MonitorInfo) {
}
```

所以严谨说法是：协议层保留了 Pull 风格接口，但当前 worker 主链路使用的是 `SetMonitorInfo` Push。

## 6. Manager 侧：接收、回调、评分和入库

manager 入口在 [manager/src/main.cpp:20](../../manager/src/main.cpp#L20)。

它创建 Push 接收服务：

```cpp
monitor::GrpcServerImpl service;
```

然后把接收回调绑定到 `HostManager::OnDataReceived`，对应代码在 [manager/src/main.cpp:34](../../manager/src/main.cpp#L34)：

```cpp
monitor::HostManager mgr;
service.SetDataReceivedCallback(
    [&mgr](const monitor::proto::MonitorInfo& info) {
      mgr.OnDataReceived(info);
    });
```

gRPC 服务注册在 [manager/src/main.cpp:60](../../manager/src/main.cpp#L60)：

```cpp
builder.RegisterService(&service);
builder.RegisterService(&query_service);
```

这也说明 manager 进程里同时注册了 Push 接收服务和查询服务。

Push 接收函数是 [`GrpcServerImpl::SetMonitorInfo()`](../../manager/src/rpc/grpc_server.cpp#L7)：

```cpp
host_data_[hostname] = {*request, std::chrono::system_clock::now()};

if (callback_) {
  callback_(*request);
}

return grpc::Status::OK;
```

这里先缓存最近一次原始 `MonitorInfo`，再调用回调进入 `HostManager`。

`HostManager::OnDataReceived()` 位于 [manager/src/host_manager.cpp:126](../../manager/src/host_manager.cpp#L126)。它完成几件事：

- 从 `host_info.hostname` 和 `host_info.ip_address` 构造主机唯一标识。
- 调用 [`CalcScore()`](../../manager/src/host_manager.cpp#L316) 计算健康评分。
- 更新内存态在线主机评分缓存 `host_scores_`。
- 调用 [`WriteToMysql()`](../../manager/src/host_manager.cpp#L382) 写入 MySQL。

更新在线状态和写库的关键代码在 [manager/src/host_manager.cpp:218](../../manager/src/host_manager.cpp#L218)：

```cpp
host_scores_[host_name] = HostScore{info, score, now};

WriteToMysql(host_name, HostScore{info, score, now}, ...);
```

所以主写链路可以概括为：

```text
SetMonitorInfo 接收请求
    -> 缓存原始 MonitorInfo
    -> HostManager::OnDataReceived
    -> CalcScore
    -> host_scores_ 在线状态
    -> WriteToMysql
```

## 7. 离线清理机制

manager 启动时会调用 `mgr.Start()`，见 [manager/src/main.cpp:41](../../manager/src/main.cpp#L41)。

`HostManager::Start()` 位于 [manager/src/host_manager.cpp:95](../../manager/src/host_manager.cpp#L95)，会启动后台线程：

```cpp
thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
```

离线清理逻辑在 [`HostManager::ProcessLoop()`](../../manager/src/host_manager.cpp#L107)：

```cpp
std::this_thread::sleep_for(std::chrono::seconds(60));

auto age = std::chrono::duration_cast<std::chrono::seconds>(
    now - it->second.timestamp).count();
if (age > 60) {
  it = host_scores_.erase(it);
}
```

因此“Manager 侧有 60 秒离线清理”这句话是真的，但更精确地说是：

- 后台线程每 60 秒检查一次。
- 如果某个主机最近一次上报时间距离当前超过 60 秒，就从 `host_scores_` 中删除。
- 这只是内存态在线列表清理，不是可靠消息确认或补传机制。

## 8. 失败处理边界

worker 侧 `PushOnce()` 在 gRPC 调用失败时返回 `false`，见 [worker/src/rpc/monitor_pusher.cpp:181](../../worker/src/rpc/monitor_pusher.cpp#L181)：

```cpp
grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);

if (status.ok()) {
  return true;
} else {
  std::cerr << ">>> Push failed: " << status.error_message() << " <<<" << std::endl;
  return false;
}
```

`PushLoop()` 收到 `false` 后只是打印失败日志并进入下一轮等待。仓库里没有发现用于失败补传的本地队列、落盘缓存、重试退避、ACK 序号或历史批量重放逻辑。

manager 侧也没有把 MySQL 写入结果反馈给 worker。`GrpcServerImpl::SetMonitorInfo()` 调用回调后直接返回 `grpc::Status::OK`；而 `WriteToMysql()` 内部连接失败时只是打印错误并 `return`。这意味着：

- gRPC 成功不等于 MySQL 一定写入成功。
- 当前系统是周期性 Push 监控系统，不是可靠消息系统。
- 如果后续要增强可靠性，可以补本地队列、失败重试、批量补传、幂等写入和端到端 ACK。

## 9. 面试表达版本

可以这样讲：

> 这个项目选择 Push 模式，是因为 worker 节点多，每台机器都独立采集并按固定周期上报。Pull 模式下 manager 要维护所有 worker 地址，还要处理节点扩缩容、NAT、防火墙和轮询失败；Push 模式下 worker 只需要知道 manager 地址，采集完本机指标后组装成 `MonitorInfo`，通过 gRPC `SetMonitorInfo` 主动推给 manager。manager 收到后缓存最近一次数据，回调到 `HostManager::OnDataReceived`，计算健康评分、更新在线状态并写入 MySQL。当前代码不是可靠消息系统，Push 失败后 worker 只是打印错误，下一轮继续采集和推送；没有本地队列补传。manager 侧后台线程每 60 秒检查一次，超过 60 秒没更新的主机会从在线评分缓存里删除。

## 10. 代码索引

| 作用 | 代码位置 |
| --- | --- |
| worker 默认 manager 地址和默认推送间隔 | [worker/src/main.cpp:8](../../worker/src/main.cpp#L8) |
| worker 创建并启动 `MonitorPusher` | [worker/src/main.cpp:38](../../worker/src/main.cpp#L38) |
| `MonitorPusher` 后台 Push 循环 | [monitor_pusher.cpp:43](../../worker/src/rpc/monitor_pusher.cpp#L43) |
| `PushOnce` 组装 `MonitorInfo` 并调用 `SetMonitorInfo` | [monitor_pusher.cpp:57](../../worker/src/rpc/monitor_pusher.cpp#L57) |
| `MonitorInfo` 数据结构 | [monitor_info.proto:86](../../proto/monitor_info.proto#L86) |
| Push RPC `SetMonitorInfo` 定义 | [monitor_info.proto:97](../../proto/monitor_info.proto#L97) |
| 保留的 Pull 风格 `GetMonitorInfo` 定义 | [monitor_info.proto:102](../../proto/monitor_info.proto#L102) |
| manager 绑定接收回调到 `HostManager::OnDataReceived` | [manager/src/main.cpp:34](../../manager/src/main.cpp#L34) |
| manager Push 接收入口 `GrpcServerImpl::SetMonitorInfo` | [grpc_server.cpp:7](../../manager/src/rpc/grpc_server.cpp#L7) |
| manager 保留的 `GetMonitorInfo` 实现 | [grpc_server.cpp:40](../../manager/src/rpc/grpc_server.cpp#L40) |
| `HostManager::OnDataReceived` 处理上报数据 | [host_manager.cpp:126](../../manager/src/host_manager.cpp#L126) |
| 更新在线评分缓存并写 MySQL | [host_manager.cpp:218](../../manager/src/host_manager.cpp#L218) |
| 健康评分函数 `CalcScore` | [host_manager.cpp:316](../../manager/src/host_manager.cpp#L316) |
| MySQL 写入函数 `WriteToMysql` | [host_manager.cpp:382](../../manager/src/host_manager.cpp#L382) |
| 60 秒离线清理逻辑 | [host_manager.cpp:107](../../manager/src/host_manager.cpp#L107) |

