# Protobuf 服务端和客户端信息对齐链路

## 1. 结论

当前项目里，worker 和 manager 的通信结构不是靠手写 JSON 字段名对齐，也不是运行时互相猜字段含义，而是靠同一份 `.proto` 文件生成出来的 C++ 代码对齐。

准确说，信息对齐分三层：

- 结构对齐：worker 和 manager 都使用 [proto/monitor_info.proto](../../proto/monitor_info.proto#L86) 里定义的 `MonitorInfo`。
- 字段对齐：protobuf 二进制协议靠字段编号对齐，比如 `name = 1`、`cpu_load = 5`，不是靠字段名字符串对齐。
- 调用对齐：gRPC 根据 `service GrpcManager` 生成客户端 Stub 和服务端 Service，worker 调 `SetMonitorInfo`，manager 实现同一个 RPC 方法。

所以这条链路可以概括为：

```text
编写 .proto
    -> CMake 调 protoc 生成 .pb.h/.pb.cc 和 .grpc.pb.h/.grpc.pb.cc
    -> worker 和 manager 链接同一个 monitor_proto
    -> worker 创建 MonitorInfo 并填字段
    -> gRPC/protobuf 序列化成二进制
    -> manager 收到后反序列化成 MonitorInfo
    -> manager 业务代码按字段读取
```

## 2. Proto 文件定义了共同合同

核心上报消息定义在 [proto/monitor_info.proto:86](../../proto/monitor_info.proto#L86)：

```proto
message MonitorInfo{
  string name = 1;           // 主机名（保留兼容）
  HostInfo host_info = 2;    // 主机标识信息
  repeated SoftIrq soft_irq = 4;
  CpuLoad cpu_load = 5;
  repeated CpuStat cpu_stat = 6;
  MemInfo mem_info = 7;
  repeated NetInfo net_info = 8;
  repeated DiskInfo disk_info = 9;
}
```

这里每一行后面的数字就是字段编号。比如：

- `name = 1` 表示字段编号 1 是主机名。
- `host_info = 2` 表示字段编号 2 是主机信息。
- `cpu_load = 5` 表示字段编号 5 是 CPU 负载信息。

protobuf 编码时，不会把 `"name"` 这个字段名原样写到网络包里，而是把“字段编号 1 + 字段类型 + 字段值”编码进去。服务端解码时，也不是找字符串 `"name"`，而是看到字段编号 1，再根据自己生成代码里的 schema 把它还原成 `name()`。

RPC 接口定义在 [proto/monitor_info.proto:97](../../proto/monitor_info.proto#L97)：

```proto
service GrpcManager {
  rpc SetMonitorInfo(MonitorInfo) returns (google.protobuf.Empty) {
  }
}
```

这表示客户端调用 `SetMonitorInfo` 时，请求体必须是 `MonitorInfo`，返回值是 `google.protobuf.Empty`。

## 3. 构建阶段生成 C++ 代码

proto 文件本身不是直接被 C++ 业务代码执行的。构建时，CMake 会把 `.proto` 文件交给 `protoc` 和 `protoc-gen-grpc` 生成 C++ 代码。

proto 目标定义在 [proto/CMakeLists.txt:21](../../proto/CMakeLists.txt#L21)：

```cmake
set(PROTO_FILES
    monitor_info.proto
    cpu_load.proto
    cpu_softirq.proto
    cpu_stat.proto
    mem_info.proto
    net_info.proto
    disk_info.proto
    query_api.proto
)

add_library(monitor_proto ${PROTO_FILES})
```

生成代码的位置在 [proto/CMakeLists.txt:63](../../proto/CMakeLists.txt#L63)：

```cmake
protobuf_generate(TARGET monitor_proto LANGUAGE cpp)

protobuf_generate(
  TARGET monitor_proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
)
```

这会生成两类代码：

```text
*.pb.h / *.pb.cc
```

负责 protobuf 消息类，比如 `monitor::proto::MonitorInfo`，里面会有 `set_name()`、`name()`、`mutable_cpu_load()`、`add_net_info()` 这类字段访问接口。

```text
*.grpc.pb.h / *.grpc.pb.cc
```

负责 gRPC 调用代码，比如 `GrpcManager::Stub` 和 `GrpcManager::Service`。

## 4. Worker 和 Manager 链接同一个协议库

生成出来的代码被打成 `monitor_proto`。worker 链接它的位置在 [worker/CMakeLists.txt:84](../../worker/CMakeLists.txt#L84)：

```cmake
set(WORKER_LIBS monitor_proto gRPC::grpc++)
```

manager 链接它的位置在 [manager/CMakeLists.txt:35](../../manager/CMakeLists.txt#L35)：

```cmake
set(MANAGER_LIBS monitor_proto gRPC::grpc++)
```

这点很关键：worker 和 manager 不是各自维护一套字段解释逻辑，而是都依赖同一份 `.proto` 生成出来的代码。只要两边使用的 proto 版本一致，结构和 RPC 方法就能对齐。

## 5. Worker 侧：创建消息并调用 Stub

worker 启动 RPC 客户端时，会创建 `GrpcManager` 的 Stub。代码在 [worker/src/rpc/monitor_pusher.cpp:14](../../worker/src/rpc/monitor_pusher.cpp#L14)：

```cpp
auto channel = grpc::CreateChannel(manager_address,
                                   grpc::InsecureChannelCredentials());
stub_ = monitor::proto::GrpcManager::NewStub(channel);
```

这个 `NewStub` 就来自 `.grpc.pb.h/.grpc.pb.cc` 生成代码。它代表“客户端视角的 GrpcManager 服务”。

每次推送时，worker 创建一个 generated message 对象。代码在 [worker/src/rpc/monitor_pusher.cpp:59](../../worker/src/rpc/monitor_pusher.cpp#L59)：

```cpp
monitor::proto::MonitorInfo info;
collector_->CollectAll(&info);
```

这里的 `MonitorInfo` 不是手写结构体，而是 proto 生成出来的 C++ 类。采集器往这个对象里填字段，本质上就是在填 `.proto` 里定义的那些字段编号对应的值。

最后 worker 调用 [worker/src/rpc/monitor_pusher.cpp:183](../../worker/src/rpc/monitor_pusher.cpp#L183)：

```cpp
grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);
```

业务代码只传了一个 C++ 对象 `info`。真正的二进制序列化、网络发送、服务端反序列化由 gRPC 和 protobuf 完成。

## 6. Manager 侧：实现同一个 RPC 方法

manager 侧的服务类继承生成出来的 `GrpcManager::Service`，位置在 [manager/include/rpc/grpc_server.h:26](../../manager/include/rpc/grpc_server.h#L26)：

```cpp
class GrpcServerImpl : public monitor::proto::GrpcManager::Service {
```

它实现的 `SetMonitorInfo` 位于 [manager/src/rpc/grpc_server.cpp:7](../../manager/src/rpc/grpc_server.cpp#L7)：

```cpp
::grpc::Status GrpcServerImpl::SetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::monitor::proto::MonitorInfo* request,
    ::google::protobuf::Empty* response) {
```

这里的 `request` 已经是反序列化后的 `MonitorInfo` 对象。manager 不需要自己解析字节流，可以直接读取字段：

```cpp
std::string hostname = request->name();
if (hostname.empty() && request->has_host_info()) {
  hostname = request->host_info().hostname();
}
```

这段代码在 [manager/src/rpc/grpc_server.cpp:15](../../manager/src/rpc/grpc_server.cpp#L15)。也就是说，worker 发出的字段编号 1 被 manager 解出来后，就能通过 `request->name()` 读取；字段编号 2 被解出来后，就能通过 `request->host_info()` 读取。

## 7. “靠字段编号对齐”怎么理解

可以把 protobuf 想成一个双方提前约好的编号表：

```text
字段编号 1 -> name
字段编号 2 -> host_info
字段编号 5 -> cpu_load
字段编号 8 -> net_info
```

worker 发送时，网络包里更接近这种含义：

```text
1: "worker-01"
2: HostInfo(...)
5: CpuLoad(...)
8: NetInfo(...)
```

manager 收到后，用同一份 `.proto` 生成的 schema 解释：

```text
字段 1 是 name，所以可以 request->name()
字段 2 是 host_info，所以可以 request->host_info()
字段 5 是 cpu_load，所以可以 request->cpu_load()
字段 8 是 net_info，所以可以 request->net_info()
```

所以协议兼容时最重要的规则是：

- 不要随便修改已有字段编号。
- 不要复用已经删除过的字段编号。
- 新增字段应该使用新的编号。
- 字段名从网络协议角度可以改，但 C++ 生成接口会变，业务代码也要跟着改。
- 老版本服务端遇到不认识的新字段编号，通常可以跳过，从而保留向前兼容能力。

## 8. 当前链路的边界

当前代码可以确认的是：

- worker 和 manager 通过同一个 `monitor_proto` 协议库对齐消息结构。
- worker 使用生成的 `GrpcManager::Stub` 调用 `SetMonitorInfo`。
- manager 使用生成的 `GrpcManager::Service` 接收 `SetMonitorInfo`。
- protobuf 的字段对齐核心是字段编号，不是字段名。

需要注意的是，当前 worker 创建 channel 时使用的是 [worker/src/rpc/monitor_pusher.cpp:15](../../worker/src/rpc/monitor_pusher.cpp#L15) 的 `grpc::InsecureChannelCredentials()`。这只说明通信协议结构能对齐，不代表传输层有 TLS 加密。

