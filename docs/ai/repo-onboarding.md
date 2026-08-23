# 1. Repo Overview

- 项目一句话定位  
  这是一个 Linux 分布式服务器性能监控系统，采用 `Worker 主动 Push -> Manager 接收/评分/入库/查询` 的运行模式。

- 主要能力  
  `worker` 侧采集 CPU/Load/内存/磁盘/网络/软中断等指标，通过 gRPC 推送给 `manager`；`manager` 侧负责接收、计算健康评分、写入 MySQL，并通过另一组 gRPC 查询接口对外提供历史/趋势/异常/详情查询。

- 技术栈判断  
  语言是 `C++17`；RPC 使用 `gRPC + Protocol Buffers`；数据采集混合使用 `procfs`、Linux 内核模块、`eBPF/libbpf`；持久化使用 `MySQL`；目标运行环境明显偏 `Linux`。  
  证据：`CMakeLists.txt`、`proto/CMakeLists.txt`、`worker/CMakeLists.txt`、`manager/CMakeLists.txt`、`README.md`。

- 构建系统判断  
  顶层构建系统是 `CMake`，其中：
  - `proto/` 先生成 protobuf/gRPC 代码
  - `worker/` 构建 `worker` 可执行文件，并可选触发 eBPF skeleton / 内核模块构建
  - `manager/` 构建 `manager` 可执行文件，并按 `mysqlclient` 是否可用决定是否启用 MySQL

# 2. Directory Map

| 目录 | 作用 | 重要程度 | 后续是否需要深入 |
|---|---|---|---|
| `proto/` | 定义 Push 数据协议和 Query 查询协议，是 manager/worker 的共享契约层 | 高 | 是 |
| `worker/src/main.cpp` | Worker 进程入口，负责启动推送线程 | 高 | 是 |
| `worker/src/monitor/` | 各类监控器实现，是真实采集路径核心 | 高 | 是 |
| `worker/src/rpc/` | Push 逻辑与历史 pull 模式残留代码 | 高 | 是 |
| `worker/src/kmod/` | CPU 统计和软中断内核模块源码，向 `/dev/*` 暴露 mmap 读接口 | 高 | 是 |
| `worker/src/ebpf/` | 网络流量 eBPF 程序与 skeleton 生成路径 | 高 | 是 |
| `worker/scripts/` | 内核模块加载/卸载辅助脚本 | 中 | 是 |
| `manager/src/main.cpp` | Manager 进程入口，串起接收服务、HostManager、QueryManager、QueryService | 高 | 是 |
| `manager/src/rpc/` | gRPC Push 接收和 QueryService 查询服务实现 | 高 | 是 |
| `manager/src/host_manager.cpp` | Push 数据进入系统后的核心处理器：计算评分、维护在线状态、写 MySQL | 高 | 是 |
| `manager/src/query_manager.cpp` | 查询服务对应的 MySQL 访问层 | 高 | 是 |
| `manager/sql/` | MySQL 表结构定义 | 高 | 是 |
| `manager/include/` | manager 核心接口声明 | 中 | 选择性深入 |
| `worker/include/` | worker 采集器与 RPC 接口声明 | 中 | 选择性深入 |

# 3. Build & Run Map

说明：

- 构建入口文件  
  - 顶层：`CMakeLists.txt`
  - 协议生成：`proto/CMakeLists.txt`
  - Worker：`worker/CMakeLists.txt`
  - Manager：`manager/CMakeLists.txt`

- 依赖项  
  - 必需：`CMake >= 3.10`、`C++17`、`protobuf`、`grpc++`
  - Manager 可选但实际运行基本必需：`mysqlclient`
  - Worker 网络增强可选：`libbpf`、`libelf`、`zlib`
  - Worker 内核模块路径：内核头文件 `linux-headers` / `kernel-devel`
  - 运行期外部依赖：MySQL、`/proc/*`、`/dev/cpu_stat_monitor`、`/dev/cpu_softirq_monitor`、网络 TC hook / eBPF

- 编译命令候选  
  这些命令有真实构建文件支撑，不是纯猜测：

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

  Worker 内核模块：

```bash
cd worker/src/kmod
make
```

  Worker eBPF 程序：

```bash
cd worker/src/ebpf
make
```

- 启动命令候选  

```bash
./build/manager/manager
sudo ./build/worker/worker <manager_ip>:50051 [interval_seconds]
```

  内核模块辅助脚本：

```bash
sudo bash worker/scripts/load_modules.sh load
```

- 配置文件位置  
  这个仓库没有独立配置文件体系，配置硬编码在源码里：
  - Manager gRPC 监听地址：`manager/src/main.cpp`
  - Manager MySQL 连接参数：`manager/src/main.cpp`、`manager/src/host_manager.cpp`
  - Worker 默认 manager 地址与推送间隔：`worker/src/main.cpp`
  - SQL 建表：`manager/sql/init_server_performance.sql`

- 可能的启动失败点  
  - `manager` 未编进 MySQL 支持：`manager/CMakeLists.txt` 中 `pkg_check_modules(MYSQL mysqlclient)` 找不到
  - `manager` 运行期连不上 MySQL：`QueryManager::Init()` 失败
  - `worker` eBPF 依赖不全：`worker/CMakeLists.txt` 会降级到 `net_monitor.cpp`
  - `worker` 未加载内核模块：`CpuStatMonitor` / `CpuSoftIrqMonitor` 打开 `/dev/*` 失败后静默不产出对应指标
  - 启动 worker 未使用 root：eBPF TC hook、`tc qdisc add`、读取某些内核能力可能失败
  - protobuf / gRPC 代码未生成：`proto/CMakeLists.txt` 失败会导致后续 target 无法链接

# 4. Architecture Map

```mermaid
flowchart LR
    A[被监控 Linux 主机] --> B[worker/src/main.cpp]
    B --> C[MonitorPusher]
    C --> D[MetricCollector]
    D --> E[CpuLoad/CpuStat/CpuSoftIrq/Mem/Disk/Net/HostInfo Monitors]
    E --> F[/proc/*]
    E --> G[/dev/cpu_stat_monitor]
    E --> H[/dev/cpu_softirq_monitor]
    E --> I[eBPF TC Hook + BPF Map]
    C --> J[gRPC GrpcManager.SetMonitorInfo]
    J --> K[manager/src/rpc/grpc_server.cpp]
    K --> L[HostManager::OnDataReceived]
    L --> M[CalcScore]
    L --> N[WriteToMysql]
    N --> O[(MySQL server_performance + detail tables)]
    P[Query Client] --> Q[gRPC QueryService]
    Q --> R[QueryServiceImpl]
    R --> S[QueryManager]
    S --> O
    O --> T[QueryResponse]
```

# 5. Startup Flow

这里实际有两条启动链：`manager` 启动链和 `worker` 启动链。

## 5.1 Manager: main -> ready

### 第 1 步：进入 `main`
- 函数/文件位置  
  `manager/src/main.cpp:20`
- 做了什么  
  解析监听地址参数，默认监听 `0.0.0.0:50051`
- 初始化了哪些资源  
  仅初始化命令行参数和日志输出
- 失败会怎样  
  参数解析很轻，没有显式失败路径

### 第 2 步：构建 Push 接收服务 `GrpcServerImpl`
- 函数/文件位置  
  `manager/src/main.cpp` 中构造 `monitor::GrpcServerImpl service`
- 做了什么  
  准备 gRPC 服务对象，用于接收 worker 上报的 `MonitorInfo`
- 初始化了哪些资源  
  gRPC Service 对象本身；真正监听 socket 还未建立
- 失败会怎样  
  当前代码没有单独失败分支

### 第 3 步：创建 `HostManager` 并绑定接收回调
- 函数/文件位置  
  `manager/src/main.cpp:35-40`  
  `manager/src/host_manager.cpp:95` `HostManager::Start()`  
  `manager/src/host_manager.cpp:107` `HostManager::ProcessLoop()`
- 做了什么  
  - 创建 `HostManager mgr`
  - 把 `service.SetDataReceivedCallback()` 绑定到 `mgr.OnDataReceived(info)`
  - 调用 `mgr.Start()` 启动后台线程
- 初始化了哪些资源  
  - `HostManager` 内部 `running_`
  - 后台线程 `thread_`
  - `host_scores_` 在线主机状态容器
- 失败会怎样  
  - 线程创建失败没有显式捕获，理论上可能抛异常并终止进程
  - 后台线程只负责每 60 秒清理陈旧主机，不影响 gRPC 启动

### 第 4 步：初始化 `QueryManager`
- 函数/文件位置  
  `manager/src/main.cpp:45-54`  
  `manager/src/query_manager.cpp:14` `QueryManager::Init()`
- 做了什么  
  初始化单个 MySQL 连接，并设置字符集为 `utf8mb4`
- 初始化了哪些资源  
  `MYSQL* conn_`、`initialized_`
- 失败会怎样  
  - 只打印警告
  - QueryService 仍然会注册，但每个查询接口会因为 `query_manager_` 未初始化或空结果而不可用

### 第 5 步：构建 `QueryServiceImpl`
- 函数/文件位置  
  `manager/src/main.cpp:57`
- 做了什么  
  创建查询 gRPC 服务，将 `QueryManager*` 注入
- 初始化了哪些资源  
  一个轻量 service 对象，内部只保存裸指针 `QueryManager*`
- 失败会怎样  
  当前没有显式失败路径

### 第 6 步：构建并启动 gRPC Server
- 函数/文件位置  
  `manager/src/main.cpp:60-70`
- 做了什么  
  - `builder.AddListeningPort(...)`
  - `builder.RegisterService(&service)`
  - `builder.RegisterService(&query_service)`
  - `builder.BuildAndStart()`
  - `server->Wait()`
- 初始化了哪些资源  
  - 监听 socket
  - gRPC server runtime
  - Push 接收服务和 Query 查询服务都被挂上同一个 server
- 失败会怎样  
  - 代码没有检查 `BuildAndStart()` 返回空指针
  - 如果端口绑定失败或服务初始化失败，存在空指针后续使用风险，待确认 gRPC 在该版本下的失败表现

## 5.2 Worker: main -> ready

### 第 1 步：进入 `main`
- 函数/文件位置  
  `worker/src/main.cpp:19`
- 做了什么  
  解析 manager 地址和推送间隔，默认 `localhost:50051` 和 `10 秒`
- 初始化了哪些资源  
  参数字符串和间隔整数
- 失败会怎样  
  `std::stoi` 对非法参数可能抛异常，当前未捕获

### 第 2 步：构造 `MonitorPusher`
- 函数/文件位置  
  `worker/src/rpc/monitor_pusher.cpp:8-19`
- 做了什么  
  - 创建 gRPC channel/stub
  - 创建 `MetricCollector`
- 初始化了哪些资源  
  - `GrpcManager::Stub`
  - `MetricCollector`
  - `MetricCollector` 构造时会实例化所有监控器
- 失败会怎样  
  - gRPC channel 创建一般不会立刻失败
  - `MetricCollector` 构造阶段如果 eBPF 加载失败，`NetEbpfMonitor` 内部会降级为“不工作状态”，但对象仍然存在

### 第 3 步：`MetricCollector` 初始化采集器集合
- 函数/文件位置  
  `worker/src/monitor/metric_collector.cpp:22-39`
- 做了什么  
  依次创建：
  - `CpuLoadMonitor`
  - `CpuStatMonitor`
  - `CpuSoftIrqMonitor`
  - `MemMonitor`
  - `NetEbpfMonitor` 或 `NetMonitor`
  - `DiskMonitor`
  - `HostInfoMonitor`
- 初始化了哪些资源  
  - 各 monitor 自己的缓存状态
  - 若启用 eBPF，`NetEbpfMonitor` 会尝试装载 BPF skeleton、附加 TC hook、持有 map fd
- 失败会怎样  
  - 大多数 monitor 采用“静默失败后跳过指标”策略
  - `NetEbpfMonitor` 会输出失败日志，但不会阻止 worker 继续运行

### 第 4 步：启动推送线程
- 函数/文件位置  
  `worker/src/main.cpp:39-40`  
  `worker/src/rpc/monitor_pusher.cpp:26`
- 做了什么  
  调用 `pusher.Start()`，启动 `PushLoop`
- 初始化了哪些资源  
  推送线程 `thread_`
- 失败会怎样  
  线程创建失败无捕获；否则进入常驻循环

### 第 5 步：主线程阻塞保持进程存活
- 函数/文件位置  
  `worker/src/main.cpp:44-47`
- 做了什么  
  主线程每 60 秒 sleep，一直存活
- 初始化了哪些资源  
  无新增资源
- 失败会怎样  
  只有被信号打断才会退出；当前没有优雅 shutdown 逻辑

# 6. Core Flows

## 6.1 采集并上报链路

入口函数 -> 中间模块 -> 核心函数 -> 下游依赖 -> 返回结果  
`worker/src/main.cpp::main` -> `MonitorPusher::PushLoop` -> `MonitorPusher::PushOnce` -> `MetricCollector::CollectAll` -> 各 `MonitorInter::UpdateOnce` -> `/proc/*`、`/dev/cpu_*`、eBPF BPF map -> `GrpcManager::Stub::SetMonitorInfo` -> 返回 gRPC `Status`

补充说明：
- `MetricCollector::CollectAll()` 位于 `worker/src/monitor/metric_collector.cpp:51`
- Push RPC 发起点位于 `worker/src/rpc/monitor_pusher.cpp:181`
- 这是运行时最核心的正向数据流

## 6.2 Manager 接收、评分、入库链路

入口函数 -> 中间模块 -> 核心函数 -> 下游依赖 -> 返回结果  
`GrpcServerImpl::SetMonitorInfo` -> `DataReceivedCallback` -> `HostManager::OnDataReceived` -> `CalcScore` + `WriteToMysql` -> MySQL `server_performance` / detail tables -> gRPC 返回 `Status::OK`

补充说明：
- Push 服务入口：`manager/src/rpc/grpc_server.cpp:7`
- 主处理函数：`manager/src/host_manager.cpp:126`
- 评分函数：`manager/src/host_manager.cpp:316`
- 入库函数：`manager/src/host_manager.cpp:382`

## 6.3 Query 查询链路

入口函数 -> 中间模块 -> 核心函数 -> 下游依赖 -> 返回结果  
`QueryServiceImpl::<具体 RPC>` -> `ConvertTimeRange` / 参数校验 -> `QueryManager::<具体查询>` -> MySQL SQL 执行 -> Protobuf Response 填充 -> gRPC 返回

补充说明：
- Query RPC 实现集中在 `manager/src/rpc/query_service.cpp`
- MySQL 查询实现集中在 `manager/src/query_manager.cpp`
- 这是对外读路径，和 Push 写路径共享同一数据库

# 7. Core Classes / Functions

| 类/函数 | 文件 | 职责 | 为什么重要 | 建议 onboarding 优先级 |
|---|---|---|---|---|
| `main` | `manager/src/main.cpp` | 串起 manager 全部启动过程 | 是 manager 启动总入口 | P0 |
| `main` | `worker/src/main.cpp` | 串起 worker 全部启动过程 | 是 worker 启动总入口 | P0 |
| `monitor::MonitorPusher` | `worker/include/rpc/monitor_pusher.h` / `worker/src/rpc/monitor_pusher.cpp` | 周期采集并 Push 到 manager | 是 worker 运行时主循环 | P0 |
| `MetricCollector::CollectAll` | `worker/src/monitor/metric_collector.cpp` | 驱动所有 monitor 填充 `MonitorInfo` | 决定采集内容与组合顺序 | P0 |
| `HostManager::OnDataReceived` | `manager/src/host_manager.cpp` | Push 数据进入系统后的核心处理 | 所有上报数据都经过这里 | P0 |
| `HostManager::CalcScore` | `manager/src/host_manager.cpp` | 计算健康评分 | 决定业务上“好机器/差机器”的核心规则 | P1 |
| `HostManager::WriteToMysql` | `manager/src/host_manager.cpp` | 把主表和详情表写入 MySQL | 是最重的副作用函数，也是风险最高的函数之一 | P0 |
| `GrpcServerImpl::SetMonitorInfo` | `manager/src/rpc/grpc_server.cpp` | gRPC Push 接收入口 | 连接网络入口与内部处理回调 | P1 |
| `QueryServiceImpl` 各 RPC | `manager/src/rpc/query_service.cpp` | 对外查询接口适配层 | 是 manager 对外 API 面 | P1 |
| `QueryManager` 各查询函数 | `manager/src/query_manager.cpp` | MySQL 查询实现 | 决定查询语义、分页、时间范围和异常逻辑 | P1 |

# 8. Data & State Map

说明：

- 核心数据结构  
  - `monitor::proto::MonitorInfo`：worker -> manager 的统一采集数据载体，定义于 `proto/monitor_info.proto`
  - `HostScore`：manager 内存态缓存，保存 `MonitorInfo + score + timestamp`，定义于 `manager/include/host_manager.h`
  - `PerformanceRecord` / `AnomalyRecord` / `ServerScoreSummary` / `*DetailRecord`：manager 查询返回的内存态结构，定义于 `manager/include/query_manager.h`

- 全局状态 / 单例 / Manager  
  - `HostManager::host_scores_`：当前在线主机分数快照，受 `mtx_` 保护
  - `QueryManager::conn_`：单个 MySQL 连接，受 `mtx_` 保护
  - `HostManager` 文件内多个 `static std::map`：保存上次采样，用于计算变化率  
    包括 `last_perf_samples`、`last_net_samples`、`last_softirq_samples`、`last_mem_samples`、`last_disk_samples`
  - Worker 各 monitor 内部缓存：
    - `CpuStatMonitor::cpu_stat_map_`
    - `CpuSoftIrqMonitor::cpu_softirqs_`
    - `NetMonitor::last_net_info_`
    - `NetEbpfMonitor::cache_`
    - `DiskMonitor` 文件级 `last_samples` / `last_time`

- 配置对象  
  当前没有集中配置对象，配置分散在源码常量：
  - Manager 监听地址：`kDefaultListenAddress`
  - MySQL 参数：`kDefaultMysqlHost/User/Pass/Db` 和 `host_manager.cpp` 内部常量
  - Worker 默认地址 / 间隔：`kDefaultManagerAddress`、`kDefaultPushInterval`

- DB / 缓存 / 文件状态  
  - MySQL 主表：`server_performance`
  - MySQL 详情表：`server_net_detail`、`server_softirq_detail`、`server_mem_detail`、`server_disk_detail`
  - 文件系统输入：
    - `/proc/loadavg`
    - `/proc/meminfo`
    - `/proc/net/dev`
    - `/proc/diskstats`
  - 设备输入：
    - `/dev/cpu_stat_monitor`
    - `/dev/cpu_softirq_monitor`
  - eBPF 状态：
    - BPF map `net_stats_map`
    - TC ingress/egress hooks 挂在各网卡

- 状态流转关系  
  `内核/系统指标` -> `worker monitor 缓存` -> `MonitorInfo` -> `GrpcServerImpl` -> `HostManager 在线快照/评分` -> `MySQL 历史表` -> `QueryManager 查询结果`

# 9. Risk Points

从工程风险角度看，这个仓库的主要风险不在“功能看不懂”，而在运行时和工程质量边界。

- 并发风险  
  - `GrpcServerImpl::SetMonitorInfo()` 可能被并发调用，并在校验后调用 `callback_(*request)` 进入 `HostManager`
  - `HostManager::OnDataReceived()` 会更新多个文件级 `static std::map`，但这些 map 没有统一锁保护；如果 gRPC server 并发接收多个 worker，上述变化率缓存存在数据竞争风险
  - `QueryManager` 通过 `mtx_` 串行化所有 MySQL 查询，线程安全较保守，但查询吞吐会被单连接瓶颈限制

- 生命周期风险  
  - `manager/src/main.cpp` 中 `QueryServiceImpl` 持有裸指针 `QueryManager*`，当前对象生命周期由 `main` 栈帧保证；如果未来改成异步关闭或更复杂结构，需要重新审视
  - `worker/src/main.cpp` 中 `MonitorPusher pusher` 启动线程后主线程永久 sleep，没有优雅停止逻辑，析构路径基本不可达
  - `HostManager` 后台线程只在析构时 `join`，当前也几乎不可达

- 资源泄漏风险  
  - `HostManager::WriteToMysql()` 每次上报都 `mysql_init + mysql_real_connect + mysql_close` 一次，虽然单次资源会关闭，但连接创建开销大，且失败重试策略粗糙
  - `NetEbpfMonitor` 会创建 TC qdisc 并 attach ingress/egress；析构时 detach hook，但默认不删除 `clsact` qdisc，属于轻度残留风险
  - 大多数 `/dev/*` mmap 采集路径在成功路径会 `munmap + close`，这一点是好的

- 错误处理风险  
  - 多个 monitor 采用静默失败，worker 仍会推送“部分字段缺失”的 `MonitorInfo`，没有统一健康告警
  - `manager` 启动时未校验 `BuildAndStart()` 返回值
  - `worker/src/main.cpp` 对 `std::stoi` 未捕获异常
  - `QueryManager` 和 `HostManager::WriteToMysql()` 大量手写 SQL 字符串拼接，没有参数化，也没有转义

- 配置风险  
  - MySQL 用户名/密码/库名硬编码在源码中，部署切换必须重新编译或改源码
  - 同一套 MySQL 默认值出现在 `manager/src/main.cpp` 和 `manager/src/host_manager.cpp` 两处，存在配置漂移风险
  - 是否启用 eBPF、是否启用 MySQL 由编译阶段决定，运行期不可切换

- 可测试性风险  
  - 仓库缺少成体系单元测试/集成测试
  - 采集逻辑高度依赖 Linux `/proc`、`/dev`、TC hook、BPF map、MySQL，mock 难度较高
  - `worker/src/ebpf/test_net_ebpf.cpp` 更像手工验证程序，不是自动测试体系

# 10. Recommended Reading Order

给出建议阅读顺序如下：

第 1 步看什么文件  
先看启动总入口：
- `manager/src/main.cpp`
- `worker/src/main.cpp`

第 2 步看什么文件  
再看协议和共享数据契约：
- `proto/monitor_info.proto`
- `proto/query_api.proto`
- `manager/sql/init_server_performance.sql`

第 3 步追哪条调用链  
优先追最核心的正向链路：
- `worker/src/rpc/monitor_pusher.cpp::PushOnce`
- `worker/src/monitor/metric_collector.cpp::CollectAll`
- `manager/src/rpc/grpc_server.cpp::SetMonitorInfo`
- `manager/src/host_manager.cpp::OnDataReceived`

第 4 步精读哪些函数  
建议精读下面这些函数：
- `manager/src/host_manager.cpp::OnDataReceived`
- `manager/src/host_manager.cpp::CalcScore`
- `manager/src/host_manager.cpp::WriteToMysql`
- `manager/src/query_manager.cpp::QueryPerformance`
- `manager/src/query_manager.cpp::QueryLatestScore`
- `worker/src/monitor/net_ebpf_monitor.cpp::InitEbpf`
- `worker/src/monitor/cpu_stat_monitor.cpp::UpdateOnce`
- `worker/src/monitor/cpu_softirq_monitor.cpp::UpdateOnce`

---

## 附：本轮已确认的几个关键判断

- 当前架构是 `Push 模式`。
  证据：`worker/src/rpc/monitor_pusher.cpp` 调用 `SetMonitorInfo()`，`manager/src/rpc/grpc_server.cpp` 实现 `SetMonitorInfo()`。

- `worker` 的网络采集存在双路径：  
  优先 `eBPF`，依赖不满足时回退到 `/proc/net/dev`。  
  证据：`worker/CMakeLists.txt`、`worker/src/monitor/metric_collector.cpp`、`worker/src/monitor/net_ebpf_monitor.cpp`、`worker/src/monitor/net_monitor.cpp`

- `manager` 的查询和写入都依赖 MySQL，但写入和查询没有共用同一个连接管理策略。  
  证据：  
  - 查询侧：`QueryManager::Init()` 初始化单连接  
  - 写入侧：`HostManager::WriteToMysql()` 每次调用单独创建和关闭连接
