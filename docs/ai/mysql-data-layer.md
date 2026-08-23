# MonitorSystem MySQL 数据层说明

本文基于当前 `main` 分支源码，说明 MonitorSystem 的 MySQL 数据库结构、数据写入链路、查询入口和当前边界。

## 1. 数据库总体结构

建库脚本位于 [`manager/sql/init_server_performance.sql`](../../manager/sql/init_server_performance.sql)。当前源码定义一个数据库和五张业务表：

```text
monitor_db
├── server_performance       主机级性能汇总和评分
├── server_net_detail        网卡级网络明细
├── server_softirq_detail    CPU 核级软中断明细
├── server_mem_detail        主机级内存明细
└── server_disk_detail       磁盘级 IO 明细
```

这是一个按时间追加监控快照的时序模型。五张表都使用自增 `id` 作为主键，并通过 `server_name` 和 `timestamp` 做逻辑关联；当前没有外键约束，也没有独立的服务器主数据表。

## 2. 五张表的结构

### 2.1 `server_performance`

定义位置：[`init_server_performance.sql:8`](../../manager/sql/init_server_performance.sql#L8)

这是主表，每次 Manager 收到一批 Worker 数据后，写入一条主机级快照。

字段按职责分为：

| 字段组 | 主要字段 | 含义 |
|---|---|---|
| 标识 | `id`、`server_name`、`timestamp` | 主机和采样时间 |
| CPU | `cpu_percent`、`usr_percent`、`system_percent`、`nice_percent`、`idle_percent`、`io_wait_percent`、`irq_percent`、`soft_irq_percent` | CPU 使用率及时间构成 |
| Load | `load_avg_1`、`load_avg_3`、`load_avg_15` | 1/5/15 分钟负载 |
| 内存 | `mem_used_percent`、`total`、`free`、`avail` | 主机级内存概况 |
| 磁盘 | `disk_util_percent` | 所有磁盘中的最大利用率 |
| 网络 | `send_rate`、`rcv_rate` | 主表中的网络速率摘要 |
| 评分 | `score` | Manager 计算的综合健康分 |
| 变化率 | CPU、Load、内存、磁盘、网络对应的 `*_rate` 字段 | 相邻采样之间的相对变化 |

索引为：

```text
idx_server_time(server_name, timestamp)
idx_score(score)
```

当前代码中，主表的写入入口是 [`HostManager::WriteToMysql()`](../../manager/src/host_manager.cpp#L435)，`server_performance` 的 INSERT 从 [`host_manager.cpp:513`](../../manager/src/host_manager.cpp#L513) 开始。

需要特别注意：当前 Manager 读取 `info.cpu_stat(0)` 填充主表 CPU 字段，因此这里保存的是 Worker 上报列表第一个 CPU 项；当前 Worker 内核模块写入的是 `cpu0`、`cpu1` 等每核数据，并没有额外的整机汇总项。

### 2.2 `server_net_detail`

定义位置：[`init_server_performance.sql:66`](../../manager/sql/init_server_performance.sql#L66)

这是网卡级明细表，数据粒度是：

```text
一台服务器 + 一块网卡 + 一个时间点
```

主要字段：

- 标识：`server_name`、`net_name`、`timestamp`
- 错误和丢包：`err_in`、`err_out`、`drop_in`、`drop_out`
- 流量速率：`rcv_bytes_rate`、`snd_bytes_rate`
- 包速率：`rcv_packets_rate`、`snd_packets_rate`
- 变化率：上述速率、错误、丢包字段对应的 `*_rate`

索引为：

```text
idx_server_net_time(server_name, net_name, timestamp)
```

写入位置：[`host_manager.cpp:567`](../../manager/src/host_manager.cpp#L567)。

查询位置：[`QueryManager::QueryNetDetail()`](../../manager/src/query_manager.cpp#L627)。

### 2.3 `server_softirq_detail`

定义位置：[`init_server_performance.sql:96`](../../manager/sql/init_server_performance.sql#L96)

这是 CPU 核级软中断明细表，数据粒度是：

```text
一台服务器 + 一个 CPU 核 + 一个时间点
```

主要字段：

- 标识：`server_name`、`cpu_name`、`timestamp`
- 软中断计数：`hi`、`timer`、`net_tx`、`net_rx`、`block`、`irq_poll`、`tasklet`、`sched`、`hrtimer`、`rcu`
- 变化率：每个软中断计数对应的 `*_rate`

索引为：

```text
idx_server_cpu_time(server_name, cpu_name, timestamp)
```

写入位置：[`host_manager.cpp:613`](../../manager/src/host_manager.cpp#L613)。

查询位置：[`QueryManager::QuerySoftIrqDetail()`](../../manager/src/query_manager.cpp#L867)。

这张表保存的是每核软中断信息，不等于完整的每核 CPU user/system/iowait 明细表。

### 2.4 `server_mem_detail`

定义位置：[`init_server_performance.sql:128`](../../manager/sql/init_server_performance.sql#L128)

这是主机级内存明细表，保存比 `server_performance` 更完整的内存字段：

- 基础内存：`total`、`free`、`avail`
- 文件缓存：`buffers`、`cached`、`swap_cached`
- 活跃状态：`active`、`inactive`、`active_anon`、`inactive_anon`、`active_file`、`inactive_file`
- 内核和页状态：`dirty`、`writeback`、`anon_pages`、`mapped`、`kreclaimable`、`sreclaimable`、`sunreclaim`
- 变化率：上述字段对应的 `*_rate`

索引为：

```text
idx_server_time(server_name, timestamp)
idx_mem_used(total, free, avail)
```

其中 `idx_mem_used` 这个名字与实际索引列并不完全一致，实际索引的是 `total`、`free`、`avail`。

写入位置：[`host_manager.cpp:663`](../../manager/src/host_manager.cpp#L663)。

查询位置：[`QueryManager::QueryMemDetail()`](../../manager/src/query_manager.cpp#L788)。

### 2.5 `server_disk_detail`

定义位置：[`init_server_performance.sql:178`](../../manager/sql/init_server_performance.sql#L178)

这是磁盘级明细表，数据粒度是：

```text
一台服务器 + 一块磁盘或分区 + 一个时间点
```

字段分为：

| 字段组 | 主要字段 |
|---|---|
| 标识 | `server_name`、`disk_name`、`timestamp` |
| 累计计数器 | `reads`、`writes`、`sectors_read`、`sectors_written` |
| IO 时间 | `read_time_ms`、`write_time_ms`、`io_in_progress`、`io_time_ms`、`weighted_io_time_ms` |
| 计算指标 | `read_bytes_per_sec`、`write_bytes_per_sec`、`read_iops`、`write_iops`、`avg_read_latency_ms`、`avg_write_latency_ms`、`util_percent` |
| 变化率 | 计算指标对应的 `*_rate` |

索引为：

```text
idx_server_disk_time(server_name, disk_name, timestamp)
```

写入位置：[`host_manager.cpp:713`](../../manager/src/host_manager.cpp#L713)。

查询位置：[`QueryManager::QueryDiskDetail()`](../../manager/src/query_manager.cpp#L708)。

## 3. 写入链路

当前 Push 模式的数据链路是：

```text
Worker
  -> MonitorInfo protobuf
  -> gRPC SetMonitorInfo
  -> GrpcServerImpl::SetMonitorInfo
  -> HostManager::OnDataReceived
  -> HostManager::CalcScore
  -> HostManager::WriteToMysql
  -> MySQL 五张表
```

关键入口：

1. [`GrpcServerImpl::SetMonitorInfo()`](../../manager/src/rpc/grpc_server.cpp#L7) 接收 Worker 上报，并触发 Manager 回调。
2. [`HostManager::OnDataReceived()`](../../manager/src/host_manager.cpp#L174) 构造主机标识、计算评分、计算变化率，并调用写库函数。
3. [`HostManager::WriteToMysql()`](../../manager/src/host_manager.cpp#L435) 依次 INSERT 主表、网络、软中断、内存和磁盘明细表。

写入连接位于 `host_manager.cpp` 文件级静态状态中，通过 `GetMysqlConnection()` 建立，并由 `CloseMysqlConnection()` 关闭。连接参数从 `MONITOR_MYSQL_HOST`、`MONITOR_MYSQL_USER`、`MONITOR_MYSQL_PASSWORD`、`MONITOR_MYSQL_DATABASE` 等环境变量读取，源码中不应在文档或日志中暴露密码值。

## 4. 查询链路

查询链路是：

```text
客户端
  -> QueryService gRPC
  -> QueryServiceImpl
  -> QueryManager
  -> MySQL SELECT
  -> protobuf response
```

查询接口定义位于 [`proto/query_api.proto:332`](../../proto/query_api.proto#L332)，具体对应关系如下：

| gRPC 接口 | QueryManager 方法 | 主要数据表 |
|---|---|---|
| `QueryPerformance` | `QueryPerformance()` | `server_performance` |
| `QueryTrend` | `QueryTrend()` | `server_performance` |
| `QueryAnomaly` | `QueryAnomaly()` | `server_performance` |
| `QueryScoreRank` | `QueryScoreRank()` | `server_performance` |
| `QueryLatestScore` | `QueryLatestScore()` | `server_performance` |
| `QueryNetDetail` | `QueryNetDetail()` | `server_net_detail` |
| `QueryDiskDetail` | `QueryDiskDetail()` | `server_disk_detail` |
| `QueryMemDetail` | `QueryMemDetail()` | `server_mem_detail` |
| `QuerySoftIrqDetail` | `QuerySoftIrqDetail()` | `server_softirq_detail` |

`QueryManager` 的 MySQL 连接初始化和关闭位于 [`query_manager.cpp:14`](../../manager/src/query_manager.cpp#L14) 和 [`query_manager.cpp:53`](../../manager/src/query_manager.cpp#L53)。它维护一条查询连接，并通过成员 mutex 串行保护查询过程。

## 5. 当前数据库操作特征

### 5.1 主要是追加写入

当前写入代码使用 `INSERT INTO`，每次采样追加新记录；没有按主机更新最新行的逻辑。

### 5.2 当前没有业务 UPDATE/DELETE

源码中的数据库操作主要是：

- 建库建表：`CREATE DATABASE`、`CREATE TABLE IF NOT EXISTS`
- 写入：`INSERT INTO`
- 查询：`SELECT`、`COUNT`、`AVG`、`MAX`

Manager 过期主机清理只清理内存中的 `host_scores_`，不会删除 MySQL 历史数据。

### 5.3 建表和运行时写入是分开的

`manager/sql/init_server_performance.sql` 负责初始化表结构；Manager 运行时不会自动执行建表或数据库迁移脚本。已有数据库如果缺表或字段不匹配，需要人工执行 SQL 处理。

### 5.4 MySQL 是可选编译能力

[`manager/CMakeLists.txt`](../../manager/CMakeLists.txt) 通过 `ENABLE_MYSQL` 和 `mysqlclient` 依赖决定是否启用 MySQL。未找到 MySQL 依赖时，Manager 会关闭 MySQL 支持；此时查询和持久化不能按正常数据库路径工作。

### 5.5 写库失败不会反馈给 Worker

`GrpcServerImpl::SetMonitorInfo()` 调用 `HostManager::OnDataReceived()` 后，当前仍可能返回 gRPC `OK`。因此 MySQL 连接失败或 INSERT 失败时，Worker 可能仍认为本次 Push 成功。

## 6. 当前数据模型边界

当前数据库能够回答：

- 某台服务器在某个时间段的整体性能趋势
- CPU、内存、磁盘、网络、软中断是否超过阈值
- 各网卡、各磁盘、各 CPU 核软中断的详细变化
- 各服务器的综合评分和排名

当前数据库不能直接回答：

- 哪个进程导致 CPU 升高
- 哪个线程消耗 CPU
- 每个 CPU 核完整的 user/system/iowait 历史明细
- 一次采样对应的原始 `MonitorInfo` 全量快照
- MySQL 写入是否与 Worker Push 严格一一对应

特别是 CPU 根因分析，目前只有 `server_performance` 的 CPU 分类指标和 `server_softirq_detail` 的软中断明细，没有进程级 CPU 表，也没有独立的 CPU 每核明细表。

## 7. 证据路径汇总

- 建库建表：[`manager/sql/init_server_performance.sql`](../../manager/sql/init_server_performance.sql)
- Manager 启动和 QueryManager 初始化：[`manager/src/main.cpp`](../../manager/src/main.cpp#L47)
- gRPC Push 接收：[`manager/src/rpc/grpc_server.cpp`](../../manager/src/rpc/grpc_server.cpp#L7)
- 主机数据处理和评分：[`manager/src/host_manager.cpp`](../../manager/src/host_manager.cpp#L174)
- MySQL 写入：[`manager/src/host_manager.cpp`](../../manager/src/host_manager.cpp#L435)
- MySQL 查询：[`manager/src/query_manager.cpp`](../../manager/src/query_manager.cpp#L112)
- 查询 gRPC 服务：[`manager/src/rpc/query_service.cpp`](../../manager/src/rpc/query_service.cpp#L30)
- 查询协议：[`proto/query_api.proto`](../../proto/query_api.proto#L332)

