# 性能数据测量方法与复现说明

本文解释简历中出现的性能数字是如何得到的，包括测试对象、输入负载、成功判定、
统计公式、复跑命令和不能外推的边界。结果汇总见
[`TEST_RESULTS_2026-08-12.md`](TEST_RESULTS_2026-08-12.md)，原始延迟样本保存在
`benchmark/results/`。

## 1. 数字与实验的对应关系

| 简历数字 | 对应实验 | 原始证据 | 能说明什么 | 不能说明什么 |
| --- | --- | --- | --- | --- |
| 6 类指标 | Worker 的 CPU、内存、磁盘、软中断、网络、主机信息采集路径 | `worker/src/monitor/metric_collector.cpp` | 当前指标采集覆盖范围 | 每一项指标在任意内核/负载下都已完成精度验证 |
| 87.11 Gb/s、24.82 万 packets/s | TC eBPF 挂载 A/B 的最高 eBPF 轮次 | WSL2 中 `iperf3` JSON、BPF map dump、`/proc/net/dev` | eBPF 计数在高吞吐虚拟链路下可工作 | 物理网卡线速或生产环境 NIC 能力 |
| 3 轮 RX/TX 0 偏差 | eBPF map 增量与同一 veth 的 `/proc/net/dev` 增量 | 每轮 before/after 快照 | 同一统计口径的字节一致性 | eBPF 与 iperf 应用层字节数完全相同 |
| CPU 中位数相差 0.51 个百分点 | TC filter 未挂载/挂载 A/B 的 `mpstat` | 3 轮 A/B 的 `mpstat 1 10` | 当前受控 WSL2 流量下未观察到明显全机 CPU 差异 | eBPF 程序独占 CPU 开销为 0.51% |
| 75 节点、10 分钟、45,000 次 | Manager -> MySQL 错峰稳定性压测 | `hard-stagger-75w-600s.csv` + MySQL 精确计数 | 中心端同步处理与写库的端到端稳定性 | 真实 Worker 采集成本、跨机器网络延迟或生产集群容量 |
| 100% 成功/精确落库 | 成功 gRPC 样本数与按 `run-id` 查询到的 MySQL 行数相等 | CSV + `server_performance` 查询 | 每条成功 gRPC 对应一条本轮持久化记录 | 断网重试、WAL 补传或 exactly-once 语义 |
| P99 676.736 ms | 45,000 条成功请求延迟排序后的第 44,550 条 | CSV `latency_us` | 该场景下端到端调用尾延迟 | 仅 eBPF 或仅 MySQL 的延迟 |
| Manager 8.68% CPU / 13.3 MiB | 稳定性轮次每秒 Docker stats 采样 | 运行期 `docker stats --no-stream` | 该容器/主机组合的资源占用 | 其他机器、不同 Docker 配额或单进程精确资源画像 |

## 2. 公共环境与原则

### 2.1 Windows Docker 的中心端测试环境

| 项目 | 值 |
| --- | --- |
| 宿主机 | Windows 11 家庭中文版 10.0.26200，64 位 |
| CPU | AMD Ryzen 7 5800H，8 核 / 16 逻辑处理器 |
| 内存 | 13.86 GiB |
| 容器运行时 | Docker Desktop Engine 29.6.2 |
| Manager 镜像 | `monitor-system-benchmark:local` |
| 数据库 | `mysql:8.0`，数据库名 `monitor_db` |
| 被测链路 | 压测客户端 -> gRPC `SetMonitorInfo` -> `HostManager` -> MySQL |

压测客户端是项目内的 C++ 程序
[`push_benchmark.cpp`](src/push_benchmark.cpp)。它构造合法的 `MonitorInfo`，但不调用
真实 Worker 的 `/proc`、内核模块或 eBPF 采集。因此 Manager 数据用于证明**中心端
接收、计算、同步持久化**能力，不能用于证明 Worker 采集性能。

### 2.2 WSL2 的 eBPF 测试环境

| 项目 | 值 |
| --- | --- |
| 发行版 | WSL2 Ubuntu 24.04，根文件系统位于 `F:\\WSL\\Ubuntu-24.04\\ext4.vhdx` |
| 内核 | `6.18.33.2-microsoft-standard-WSL2` |
| BTF | `/sys/kernel/btf/vmlinux` |
| 流量链路 | 临时 network namespace + veth pair |
| 流量工具 | `iperf3`，TCP，4 并发流，单轮 10 s |
| eBPF 程序 | `worker/src/ebpf/net_stats.bpf.c`，TC ingress/egress |

veth 是内存中的虚拟网卡对，排除了物理网卡、驱动、交换机和真实外网的影响。其价值是
可控地向项目 TC hook 提供高吞吐、高包速流量；其吞吐数字不能当成真实 NIC 线速。

## 3. Manager -> MySQL 端到端测试

### 3.1 被测路径

负载发生器为每一个模拟节点创建一个 gRPC channel/stub，并以固定周期调用
`GrpcManager::SetMonitorInfo`。Manager 接收后进入 `HostManager::OnDataReceived`，
计算主机状态与变化率，随后调用 `WriteToMysql` 写入 MySQL。当前压测部署设置
`MONITOR_VERBOSE_METRICS=0`，避免逐条控制台输出占用 I/O；gRPC 返回仍发生在
`OnDataReceived` 完成之后，未改为异步确认。

```text
模拟 Worker 线程
  -> SetMonitorInfo(gRPC)
  -> Manager::OnDataReceived
  -> 评分 / 变化率计算
  -> WriteToMysql
  -> gRPC OK + CSV 记录延迟
```

每条消息包含 CPU、load、内存、网络与磁盘字段，字段构造可见于
[`MakeRequest`](src/push_benchmark.cpp)。每个模拟 Worker 保持一个固定请求内容和
独立 hostname，hostname 包含 `run-id`，用于避免不同轮次混入同一 SQL 计数。

### 3.2 两种到达模型

| 模型 | 实现 | 代表的含义 |
| --- | --- | --- |
| 整秒突发 | 默认；所有线程同时开始第一条请求 | 所有节点定时任务相位相同的最坏集中到达 |
| 首轮错峰 | `--stagger-start`；第 `i` 个节点延迟 `i × interval / workers` 启动 | 周期性上报在一个周期内相位分散的部署模型 |

错峰只改变首轮相位，之后每个节点仍以 1 s 周期上报。因此 75 节点、600 s 的期望
样本数为：

```text
75 workers × 600 seconds × 1000 ms / 1000 ms = 45,000 samples
```

错峰模型与整秒突发模型不能混为同一基线。前者用于稳定部署下的持续能力，后者用于
定位集中突发时的排队边界。

### 3.3 延迟、成功率和持久化口径

单条延迟从 gRPC 调用前的 `steady_clock` 取样开始，到 `SetMonitorInfo` 返回结束，
以微秒写入 CSV：

```text
latency_us = steady_clock_after_rpc - steady_clock_before_rpc
```

只有 gRPC `status.ok()` 的样本参与 P50/P95/P99。对排序后的成功延迟数组（长度为
`N`），分位点使用：

```text
index(p) = ceil(N × p) - 1
Pp = sorted_latency[index(p)]
```

例如 45,000 条成功样本的 P99 索引为 `ceil(45000 × 0.99) - 1 = 44,549`，即排序后
第 44,550 条样本（从 1 开始计数）。

本项目把“端到端持久化成功”定义为以下三项同时成立：

1. CSV 中该轮每条样本的 gRPC 状态为 `OK`；
2. 成功样本数等于期望样本数；
3. `server_performance` 中 hostname 前缀匹配本轮 `run-id` 的行数，等于成功样本数。

SQL 核验示例：

```sql
SELECT COUNT(*)
FROM server_performance
WHERE server_name LIKE 'hard-stagger-75w-600s-worker-%';
```

这不是消息队列语义测试：Worker 当前没有 WAL、补发队列或幂等序列号，故不宣称
at-least-once 或 exactly-once。

`run-windows-benchmark.ps1` 的通用自动检查使用“落库行数不少于成功样本数”，用于
快速发现明显落库缺失；本文列出的 75 节点正式结果在脚本外额外执行了上面的前缀 SQL，
并确认行数**严格等于** 45,000。因此，复跑正式简历数据时应保留严格相等的人工 SQL
核验，而不是只依赖通用脚本的下限检查。

### 3.4 75 节点、10 分钟稳定性实验

#### 输入

| 参数 | 值 |
| --- | --- |
| `workers` | 75 |
| `duration-seconds` | 600 |
| `interval-ms` | 1000 |
| 到达模型 | 首轮错峰 |
| 预期样本 | 45,000 |
| 输出 CSV | `benchmark/results/hard-stagger-75w-600s.csv` |

测试前强制重建 Manager，避免此前过载轮次的 in-flight 回调、主机状态和队列影响下一轮。
MySQL 保持运行，以便验证长期写入的持续状态。

#### 手工复跑命令

在仓库根目录 PowerShell 中：

```powershell
docker compose -f docker-compose.benchmark.yml up -d mysql manager
docker compose -f docker-compose.benchmark.yml up -d --no-deps --force-recreate manager

docker compose -f docker-compose.benchmark.yml run --rm loadgen `
  --workers 75 --duration-seconds 600 --interval-ms 1000 `
  --stagger-start --run-id hard-stagger-75w-600s `
  --output /results/hard-stagger-75w-600s.csv

powershell -ExecutionPolicy Bypass -File .\benchmark\summarize-results.ps1 `
  -CsvPath .\benchmark\results\hard-stagger-75w-600s.csv

docker compose -f docker-compose.benchmark.yml exec -T mysql `
  mysql -N -s -ubenchmark -pbenchmark_only_password monitor_db `
  -e "SELECT COUNT(*) FROM server_performance WHERE server_name LIKE 'hard-stagger-75w-600s-worker-%';"
```

正式复跑时必须换一个全新 `run-id` 与 CSV 文件名，防止 SQL 统计历史数据。

#### 资源采样

压测期间循环执行以下命令采样：

```powershell
docker stats --no-stream --format '{{.CPUPerc}}|{{.MemUsage}}' `
  monitor_system-manager-1
docker stats --no-stream --format '{{.CPUPerc}}|{{.MemUsage}}' `
  monitor_system-mysql-1
```

从全部采样点计算平均 CPU、最大 CPU 和最大 RSS 显示值。该轮得到 274 条容器采样
记录（Manager 与 MySQL 合计；采样间隔受两次 `docker stats` 调用耗时影响，并非严格
1 秒）：Manager 平均 CPU 8.68%、峰值 11.61%、峰值内存 13.3 MiB；MySQL 平均 CPU
29.92%、峰值 40.06%、峰值内存 485.6 MiB。Docker CPU 百分比由容器运行时计算，
受宿主机核数、Docker 配额和同时运行的工作负载影响，不应用于不同机器之间的绝对比较。

#### 结果复核

| 项目 | 值 |
| --- | --- |
| CSV 样本 / gRPC 成功 | 45,000 / 45,000 |
| MySQL 精确落库 | 45,000 |
| P50 / P95 / P99 | 13.119 / 459.494 / 676.736 ms |
| Manager 最终状态 | `running` |
| MySQL 最终状态 | `healthy` |

### 3.5 集中突发容量边界

100 和 300 节点测试保持 1 s 周期、60 s 时长，但不启用 `--stagger-start`。它们的
作用是验证“所有节点同一时刻到达”时全局串行计算和同步 MySQL 写入的排队效应：

| 场景 | 成功 / 落库 | P99 | 解释 |
| --- | --- | --- | --- |
| 100 节点整秒突发 | 6,000 / 6,000 | 1,300.236 ms | 未丢失，但已经越过 1 s 尾延迟目标 |
| 300 节点整秒突发 | 18,000 / 18,000 | 3,883.510 ms | 排队显著，不能称为稳定档 |
| 500 节点整秒突发 | 中止 | 无完整终态 | 8 分钟内未完成，停止前写入 17,521 行，不纳入结果 |

500 节点中止后，Manager 继续处理在途工作；后续独立稳定性轮次前重建 Manager。
因此“中止后未立即恢复”是当前同步串行设计的过载现象记录，不是 500 节点成功率。

## 4. TC eBPF 网络采集 A/B 测试

### 4.1 被测实现

内核态程序 `worker/src/ebpf/net_stats.bpf.c` 在 TC ingress 和 egress 两个 hook 上
读取 `skb->ifindex` 与 `skb->len`，按 ifindex 写入 `BPF_MAP_TYPE_PERCPU_HASH`。
每个 CPU 有独立 `net_stats` value，避免热点网卡在高 PPS 下对同一个计数值进行跨 CPU
竞争。用户态 `NetEbpfMonitor::ReadAggregatedStats` 使用 `bpf_map_lookup_elem` 读取
所有 possible CPU 的 value，并对 bytes 和 packets 求和。

```text
TC ingress / egress
  -> skb->len
  -> Per-CPU Hash Map[ifindex]
  -> 用户态读取所有 CPU 槽位
  -> 聚合得到 RX/TX 字节数与包数
```

这里测的是 map 里的累计计数。速率由相邻两次读取的差值除以时间窗得到；协议字段的
字节速率在 Worker 中转换为 kB/s。A/B 的准确性比较使用累计字节增量，避免采样时钟和
单位换算影响结论。

### 4.2 隔离拓扑与流量

每一整组测试创建临时网络命名空间和 veth：

```text
WSL root namespace                         ebpfbench-ns
10.201.1.1/24                              10.201.1.2/24
ebpfbench-veth0  <-------- veth ------->   ebpfbench-veth1
       ^
       | TC ingress + TC egress（仅 eBPF 组）
```

在 network namespace 内启动 `iperf3 -s -1`，root namespace 端执行：

```bash
iperf3 -c 10.201.1.2 -t 10 -P 4 -J
```

`-P 4` 表示 4 条并发 TCP 流；`-J` 输出机器可解析的 JSON，其中 `end.sum_sent` 的
`bits_per_second` 是本文“iperf 吞吐”。该数字是应用层发送速率，不和 TC 的 `skb->len`
直接比较；TCP/IP 头、ACK 和控制流量会使网卡/TC 的字节增量不同。

### 4.3 A/B 控制变量与采样顺序

共执行 3 对交替轮次：

```text
baseline_1 -> ebpf_1 -> baseline_2 -> ebpf_2 -> baseline_3 -> ebpf_3
```

baseline 轮次删除 ingress/egress BPF filter，eBPF 轮次把项目编译出的
`tc_ingress`、`tc_egress` 挂到同一 veth。每轮均：

1. 读取 veth 在 `/proc/net/dev` 中的 RX/TX 字节基线；
2. eBPF 轮次额外 dump map 作为 BPF 基线；
3. 启动 namespace 内 iperf server；
4. 同时启动 `mpstat 1 10`；
5. 执行 10 秒 4 流 iperf client；
6. 等待 server 和 `mpstat` 退出，读取 after 快照；
7. eBPF 轮次对 Per-CPU map 各槽位求和，再计算 before/after 差值。

因为实验交替进行，短时间内的 WSL 调度和温度变化对两个组的影响相对更接近；但没有
固定 CPU 亲和性、关闭 turbo 或做置信区间分析，所以不能把组间 3.78% 吞吐差异解释为
eBPF 带来的性能提升。

### 4.4 统计公式

对每个方向 `d ∈ {RX, TX}`：

```text
proc_delta_d = proc_after_d - proc_before_d
ebpf_delta_d = Σ(cpu_value_after_d) - Σ(cpu_value_before_d)
difference_pct_d = |ebpf_delta_d - proc_delta_d| / proc_delta_d × 100
```

对每组 3 个吞吐或 CPU 值取中位数。`mpstat` 的 WSL 总 CPU 使用率按：

```text
cpu_used_pct = 100 - %idle
```

它包含 iperf client、内核网络栈、BPF 执行和 WSL 中其他进程，因此只用于观察“整机
观测扰动”，不用于归因 eBPF 程序自身耗时。

### 4.5 A/B 结果与解释

| 指标 | baseline 中位数 | eBPF 中位数 | 结论 |
| --- | --- | --- | --- |
| iperf 吞吐 | 81.00 Gb/s | 84.06 Gb/s | 3 轮范围重叠，不能宣称 eBPF 提升吞吐 |
| WSL 总 CPU 使用率 | 43.51% | 44.02% | 中位数相差 0.51 个百分点 |
| RX/TX 字节差异 | 不适用 | 3/3 轮均 0 B（0%） | map 聚合与同一网卡 procfs 计数一致 |

三个 eBPF 轮次的吞吐分别约为 81.04、87.11、84.06 Gb/s，最高轮次统计到
2,482,267 个 TX 包，即约 248,227 packets/s；三轮累计 7,174,893 个 TX 包。小报文
补充轮使用 `iperf3 -l 128`，在约 1.66 Gb/s 下统计到 68,476 个 TX 包，同样保持
RX/TX 0 偏差。

### 4.6 手工复跑骨架

以下命令展示核心步骤；需要 root 权限、`clang`、`bpftool`、`tc`、`iperf3` 和
`sysstat`。临时接口、namespace 和 BPF pin 必须在结束时清理。

```bash
# 1. 创建隔离网络
ip netns add ebpfbench-ns
ip link add ebpfbench-veth0 type veth peer name ebpfbench-veth1
ip link set ebpfbench-veth1 netns ebpfbench-ns
ip addr add 10.201.1.1/24 dev ebpfbench-veth0
ip link set ebpfbench-veth0 up
ip netns exec ebpfbench-ns ip link set lo up
ip netns exec ebpfbench-ns ip addr add 10.201.1.2/24 dev ebpfbench-veth1
ip netns exec ebpfbench-ns ip link set ebpfbench-veth1 up

# 2. 编译并加载项目 BPF 程序（BTF/工具路径依发行版调整）
mount -t bpf bpf /sys/fs/bpf 2>/dev/null || true
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
  -I/usr/include/x86_64-linux-gnu -Iworker/src/ebpf \
  -c worker/src/ebpf/net_stats.bpf.c -o /tmp/net_stats.bpf.o
bpftool prog loadall /tmp/net_stats.bpf.o /sys/fs/bpf/ebpfbench-progs \
  pinmaps /sys/fs/bpf/ebpfbench-maps
tc qdisc add dev ebpfbench-veth0 clsact
tc filter add dev ebpfbench-veth0 ingress bpf da \
  pinned /sys/fs/bpf/ebpfbench-progs/tc_ingress
tc filter add dev ebpfbench-veth0 egress bpf da \
  pinned /sys/fs/bpf/ebpfbench-progs/tc_egress

# 3. 基线/after 快照与流量
cat /proc/net/dev | grep ebpfbench-veth0
bpftool map dump pinned /sys/fs/bpf/ebpfbench-maps/net_stats_map -j
ip netns exec ebpfbench-ns iperf3 -s -1 &
mpstat 1 10 &
iperf3 -c 10.201.1.2 -t 10 -P 4 -J

# 4. 清理
tc qdisc del dev ebpfbench-veth0 clsact
rm -rf /sys/fs/bpf/ebpfbench-progs /sys/fs/bpf/ebpfbench-maps
ip link del ebpfbench-veth0
ip netns del ebpfbench-ns
```

## 5. 简历中如何正确引用

可写：

> 基于 TC eBPF + Per-CPU Hash Map 实现网络字节/包速率采集；在 WSL2 隔离 veth 的
> 3 轮 A/B 压测中，最高覆盖 87.11 Gb/s、24.82 万 packets/s，eBPF 聚合值与
> `/proc/net/dev` 的 RX/TX 字节增量均为 0 偏差。

> 在 75 节点、1s 周期、10 分钟端到端稳定性压测中，完成 45,000 次
> `gRPC -> Manager -> MySQL` 同步持久化，成功率与精确落库率均为 100%，P99 为
> 676.7ms；Manager 平均 CPU 8.68%、峰值内存 13.3MiB。

不可写：

- “eBPF 提升网络吞吐 3.78%”：A/B 范围重叠，差异没有归因条件；
- “eBPF 只有 0.51% CPU 开销”：0.51 是 WSL 全机 CPU 中位数差；
- “网卡性能达到 87.11 Gb/s”：该值来自 WSL2 veth；
- “系统支持 300 节点稳定运行”：300 节点整秒突发的 P99 是 3.88 s；
- “45,000 条数据 exactly-once”：测试只证明该轮成功请求与落库行数相等。

## 6. 复跑检查清单

1. 每轮生成新 `run-id`，并保存 CSV、终端输出和 Docker/WSL 版本；
2. Manager 测试前确认 MySQL 为 `healthy`、Manager 为 `running`；
3. 长压或过载实验后重建 Manager，再跑下一轮；
4. P50/P95/P99 只对 gRPC 成功样本计算；
5. 每轮用 SQL 按 `run-id` 计数，不能使用整张表总行数；
6. eBPF 轮次保存 map 与 `/proc` 的 before/after 快照，并聚合所有 possible CPU；
7. 结束时删除 TC `clsact`、BPF pin、veth 和 network namespace；
8. 任何失败、中止或未完成轮次都单列为边界，不放入成功率或吞吐结论。
