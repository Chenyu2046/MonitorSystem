# Legacy Manager Windows Docker 基准测试记录（2026-08-12）

> 本文件全部结果来自当前 Sharded Manager 之前的历史全局串行 + 同步 MySQL
> Manager，统一归档为 `Legacy Manager Baseline`。不得把这里的延迟、容量或资源
> 数字重新标记为当前 `codex/kernscope-manager-concurrency` 的 T0/T1/T2 结果。

## 结论

本次历史记录中唯一有效的 Legacy 基线为：在 Windows Docker Linux
运行环境中，模拟 10 个节点以 1 秒周期向 Manager 推送监控数据，连续
30 秒完成 300 次真实 gRPC -> Manager -> MySQL 上报与持久化；请求成功率
为 100%，Legacy synchronous P50/P95/P99 分别为 81.615/157.418/201.028 ms。

这是一组稳定性与端到端延迟基线，不是系统峰值吞吐或 eBPF 性能数据。

## Legacy 有效基线

| 项目 | 值 |
| --- | --- |
| 测试日期 | 2026-08-12 |
| 模拟节点数 | 10 |
| 每节点上报间隔 | 1000 ms |
| 配置时长 | 30 s |
| 请求样本数 | 300 |
| 成功请求 | 300 |
| 失败请求 | 0 |
| 成功率 | 100% |
| 实测完成速率 | 10.2936 reports/s |
| Legacy synchronous P50 | 81,615 us（81.615 ms） |
| Legacy synchronous P95 | 157,418 us（157.418 ms） |
| Legacy synchronous P99 | 201,028 us（201.028 ms） |
| gRPC 状态码 | 300 个 `OK`（0） |
| `server_performance` 落库行数 | 300 |
| 已落库的不同节点数 | 10 |
| Manager 最终状态 | `running`, `exit=0` |

`10.2936 reports/s` 是此固定施压条件下的实际完成速率（10 个节点每秒各
1 次），不能解释为系统可承受的最大吞吐。

## 测试环境与范围

| 项目 | 值 |
| --- | --- |
| 宿主系统 | Windows 11 家庭中文版 10.0.26200，64 位 |
| CPU | AMD Ryzen 7 5800H，8 核 / 16 逻辑处理器 |
| 内存 | 13.86 GiB |
| Docker Desktop Engine | Client/Server 29.6.2 |
| 服务镜像 | `monitor-system-benchmark:local`、`mysql:8.0` |
| 服务状态 | MySQL `healthy`；Manager `running` |
| 被测链路 | 压测客户端 -> gRPC `SetMonitorInfo` -> `HostManager` -> MySQL |
| 未覆盖的范围 | 真实 Worker 采集、TC/eBPF 内核路径、Linux 主机资源上限、峰值压测 |

压测客户端构造合法的 `MonitorInfo` 后直接调用 Manager 的 gRPC 服务；因此
测得的是中心端接收、指标计算与 MySQL 写入的端到端延迟，不包含真实机器
的 `/proc` 采集或 eBPF 数据采集成本。

## 结果证据

- 原始结果：`benchmark/results/push-10w-30s-1000ms.csv`
- CSV SHA-256：`F986D6880EF3D545C9D41D47CD1FD98026DFE9CEAD62FEE5490F744D2F0B27F4`
- 汇总来源：`benchmark/summarize-results.ps1`
- 落库核验：`SELECT COUNT(*) FROM server_performance WHERE server_name LIKE 'benchmark-worker-%';` 返回 `300`；不同 `server_name` 返回 `10`。

## 可复跑命令

在仓库根目录的 Windows PowerShell 中执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmark\run-windows-benchmark.ps1 `
  -Workers 10 -DurationSeconds 30 -IntervalMs 1000
```

脚本会构建/启动 MySQL 与 Manager，运行负载发生器，输出 CSV、延迟分位数，
并校验 `server_performance` 的行数不少于成功上报数。每次正式对比应保存
新生成的 CSV，并同时记录容器版本、机器资源和配置参数。

## 试跑记录（不可用于简历）

| 轮次 | 条件 | 客户端结果 | 服务端/持久化结果 | 结论 |
| --- | --- | --- | --- | --- |
| 1 | 10 节点、1 秒周期、30 秒 | 300 样本，0 成功，全部 `UNAVAILABLE`（14） | Manager 收到首批 10 个请求后以 `exit=139` 退出 | 无效：定位到多个 gRPC 回调并发读写共享历史采样 `map` 的数据竞争。 |
| 2 | 修复并发数据竞争后，相同施压条件 | 300/300 成功；P50/P95/P99 为 19.976/46.690/61.135 ms | 错配到 `monitor_benchmark`，其中不存在 `server_performance` 表；未形成有效落库证据 | 无效：不能以 gRPC 成功代替持久化成功。 |
| 3 | 修复并发数据竞争、将 Compose 数据库与建表脚本统一为 `monitor_db` 后，相同施压条件 | 300/300 成功；Legacy synchronous P50/P95/P99 为 81.615/157.418/201.028 ms | 精确落库 300 行，Manager 保持运行 | 有效 Legacy 基线。 |

轮次 1 的修复是在 `HostManager::OnDataReceived` 中保护整段“变化率计算 +
落库”共享状态更新，避免共享历史采样表的并发访问。它优先保证正确性，且会
让该处理段串行化；若后续目标是提升吞吐，应另行以异步写库或按主机分片锁为
优化方向，并用新的基线数据证明改进。

## 简历使用边界

可写：

> Legacy Manager 在 Windows Docker Linux 基准环境中模拟 10 节点以 1s 周期上报，连续
> 30s 完成 300 次 gRPC -> MySQL 同步指标写入，成功率 100%，Legacy synchronous
> P50/P95/P99 为 81.6/157.4/201.0 ms。

不可写：

- “系统峰值吞吐为 10.29 QPS”
- “eBPF 路径延迟 P99 为 201 ms”
- “已完成 Linux 生产环境压力测试”

## WSL2 TC/eBPF 高流量计数对比

在同一台 Windows 主机的 WSL2 Ubuntu 24.04 中完成了项目
`worker/src/ebpf/net_stats.bpf.c` 的真实内核功能验证。Ubuntu 根文件系统位于
`F:\\WSL\\Ubuntu-24.04\\ext4.vhdx`，不使用 C 盘作为发行版数据目录。

| 项目 | 结果 |
| --- | --- |
| WSL 内核 | `6.18.33.2-microsoft-standard-WSL2` |
| BTF 来源 | `/sys/kernel/btf/vmlinux` |
| 编译结果 | 成功生成 eBPF ELF 与 44,507 字节 skeleton |
| 加载与 JIT | `tc_ingress`、`tc_egress` 均成功加载并显示为 `jited` |
| 附着目标 | 隔离 veth `monbench-veth0` 的 TC ingress/egress |
| 流量发生器 | `iperf3`，4 条 TCP 并发流，持续 10 s |
| 被测链路 | WSL2 内隔离 network namespace + veth（非物理网卡） |
| 应用层发送量 / 速率 | 101,051,269,120 B / 80.83 Gb/s |
| `/proc/net/dev` 增量 | RX 97,261,991 B；TX 101,203,905,754 B |
| eBPF `net_stats_map` 增量 | RX 97,261,991 B（1,473,644 包）；TX 101,203,905,754 B（2,301,313 包） |
| eBPF 与 `/proc` 字节差异 | RX 0 B（0%）；TX 0 B（0%） |
| 清理 | 测试退出后分离两个 hook，并显式删除 `clsact`；最终未检测到 `clsact` 规则 |

这项验证证明当前 WSL2 可以用于项目的 TC/eBPF 编译、加载、附着、map 读取和
清理测试；在该受控高流量 veth 链路中，eBPF map 与 `/proc/net/dev` 对同一网卡的
RX/TX 字节增量完全一致。此前 HTTPS 小流量样例（约 117.7 KB/s RX）仅保留为
功能冒烟记录，不作为性能或吞吐结论。

这不是物理 NIC 线速，也没有测量 eBPF 相对 `/proc` 的 CPU 开销或真实 Linux
生产内核兼容性；这些仍需在原生 Linux 主机上单独复测。

### eBPF 挂载前后 A/B 复测（2026-08-13）

在同一 WSL2 隔离 veth 上交替执行“未挂载 TC filter”与“挂载项目
`net_stats.bpf.c`”的 3 轮 A/B 测试。每轮使用 `iperf3` 4 路 TCP、持续 10 s，
并用 `mpstat` 记录整个 WSL 虚拟机 CPU；该 CPU 数据包含 iperf、网络栈和其他 WSL
活动，不能解释为 eBPF 程序独占 CPU。

| 指标 | 未挂载 TC eBPF（3 轮） | 挂载 TC eBPF（3 轮） | 观察 |
| --- | --- | --- | --- |
| iperf 吞吐中位数 | 81.00 Gb/s | 84.06 Gb/s | 组间吞吐范围重叠，不能把 +3.78% 归因于 eBPF |
| WSL 总 CPU 使用率中位数 | 43.51% | 44.02% | 中位数相差 0.51 个百分点 |
| 最高 eBPF 轮次吞吐 | — | 87.11 Gb/s | 对应 TX 248,227 packets/s |
| eBPF 与 `/proc/net/dev` 字节差异 | — | 3/3 轮 RX/TX 均为 0 B（0%） | 覆盖约 81.04/87.11/84.06 Gb/s 三档实际吞吐 |

3 个 eBPF 轮次累计统计到 7,174,893 个 TX 包（单轮分别为 2,304,572、
2,482,267、2,388,054），最高单轮约 24.82 万 TX packets/s；每轮 eBPF map 聚合值
与同一接口的 `/proc/net/dev` 增量完全一致。这里的结论是：项目的 Per-CPU Map
计数逻辑在该受控高吞吐/高 PPS 虚拟链路下保持一致，且未观察到可归因的吞吐下降；
并非“eBPF 提升网络吞吐”。

可用于简历的有边界表述：

> 基于 TC eBPF + Per-CPU Hash Map 实现网络字节/包速率采集；在 WSL2 隔离 veth
> 的 3 轮 A/B 压测中，最高覆盖 **87.11 Gb/s、24.82 万 packets/s**，eBPF 聚合值与
> `/proc/net/dev` 的 RX/TX 字节增量均为 **0 偏差**；挂载前后 WSL 总 CPU 使用率中位数
> 仅相差 **0.51 个百分点**。该数据为虚拟链路验证，不代表物理 NIC 线速。

## Legacy Manager -> MySQL 容量扫描

在相同 Windows Docker Linux 基准环境下，以每个模拟节点每秒 1 次上报、每档
持续 30 秒的方式逐级增加并发节点数。每轮都使用独立 `run-id`，只统计该轮的
`server_performance` 落库行；合格条件为：成功率至少 99%、落库数等于成功请求
数、Manager 保持运行、P99 小于 1 秒。

| 目标节点/施压速率 | 样本/成功/落库 | 实测完成速率 | P50 / P95 / P99 | 判定 |
| --- | --- | --- | --- | --- |
| 25 / 25 reports/s | 750 / 750 / 750 | 25.5543 reports/s | 310.942 / 726.421 / 780.119 ms | 通过 |
| 30 / 30 reports/s | 900 / 900 / 900 | 30.0587 reports/s | 353.021 / 887.944 / 944.150 ms | 通过 |
| 35 / 35 reports/s | 1,050 / 1,050 / 1,050 | 35.6149 reports/s | 256.120 / 474.641 / 534.698 ms | 通过 |
| 50 / 50 reports/s | 1,500 / 1,500 / 1,500 | 50.4936 reports/s | 364.001 / 665.181 / 709.451 ms | 通过 |
| 60 / 60 reports/s | 1,800 / 1,800 / 1,800 | 60.4212 reports/s | 421.073 / 783.653 / 837.441 ms | 通过 |
| 65 / 65 reports/s，轮次 1 | 1,950 / 1,950 / 1,950 | 65.2755 reports/s | 458.172 / 863.474 / 927.306 ms | 通过 |
| 65 / 65 reports/s，轮次 2 | 1,950 / 1,950 / 1,950 | 65.2455 reports/s | 471.078 / 875.436 / 934.008 ms | 通过 |
| 70 / 70 reports/s | 2,100 / 2,100 / 2,100 | 68.4449 reports/s | 627.008 / 1,054.247 / 1,129.145 ms | 不通过：P99 > 1 s |
| 75 / 75 reports/s | 2,250 / 2,250 / 2,250 | 73.3895 reports/s | 912.567 / 1,049.534 / 1,087.213 ms | 不通过：P99 > 1 s |

因此，在这台机器、该 Docker 运行环境和历史“每条上报同步计算并写入 MySQL”
实现下，**最高已验证稳定档为 65 reports/s**：两个独立的 30 秒轮次各完成
1,950 次端到端上报，成功率和落库率均为 100%，P99 分别为 927.306 ms 与
934.008 ms。70 与 75 reports/s 虽未丢失数据，但尾延迟已越过 1 秒，不纳入
稳定吞吐声明。

原始容量扫描结果：

- `benchmark/results/capacity-25w-1000ms.csv`
- `benchmark/results/capacity-30w-1000ms.csv`
- `benchmark/results/capacity-35w-1000ms.csv`
- `benchmark/results/capacity-50w-1000ms.csv`
- `benchmark/results/capacity-60w-1000ms.csv`
- `benchmark/results/capacity-65w-1000ms.csv`
- `benchmark/results/capacity-65w-1000ms-rep2.csv`
- `benchmark/results/capacity-70w-1000ms.csv`
- `benchmark/results/capacity-75w-1000ms.csv`

可用于简历的有边界表述：

> 在 Windows Docker Linux 环境下进行 Manager -> MySQL 容量扫描：65 个模拟
> 节点以 1s 周期并发上报，两个独立 30s 轮次各完成 1,950 次端到端写入，成功率
> 与落库率均为 100%，P99 分别为 927/934 ms；70 reports/s 时 P99 超过 1s，
> 因此将 65 reports/s 作为 Legacy synchronous persistence 的历史稳定档，不代表当前
> Sharded Manager 的容量。

## Legacy Manager -> MySQL 优化后历史复测

在保持“gRPC 返回成功前完成指标计算和 MySQL 同步写入”的口径下，进行了两项
有明确生产语义的改动：Manager 复用已建立的 MySQL 连接，且在压测部署中关闭逐条
指标的控制台详细打印；负载端新增可选的首轮错峰发送，用来模拟节点周期的相位分散，
避免所有 Worker 在整秒同时涌入。默认突发模式仍保留，因此这是额外的对比场景，不能
与旧的整秒突发扫描混为同一配置。

合格条件：每轮成功率 100%、该 `run-id` 的 MySQL 落库数等于成功请求数、Manager
持续运行、P99 小于 1 s。每个模拟节点保持 1 s 上报周期，单轮持续 60 s。

| 场景 | 样本 / 成功 / 精确落库 | 实测完成速率 | P50 / P95 / P99 | 判定 |
| --- | --- | --- | --- | --- |
| 优化前：65 节点、整秒突发，轮次 1 | 1,950 / 1,950 / 1,950 | 65.2755 reports/s | 458.172 / 863.474 / 927.306 ms | 通过 |
| 优化前：65 节点、整秒突发，轮次 2 | 1,950 / 1,950 / 1,950 | 65.2455 reports/s | 471.078 / 875.436 / 934.008 ms | 通过 |
| 优化后：75 节点、首轮错峰，轮次 1 | 4,500 / 4,500 / 4,500 | 74.9773 reports/s | 17.579 / 97.778 / 128.965 ms | 通过 |
| 优化后：75 节点、首轮错峰，轮次 2 | 4,500 / 4,500 / 4,500 | 74.9943 reports/s | 16.999 / 116.514 / 146.459 ms | 通过 |
| 优化后：78 节点、首轮错峰 | 4,680 / 4,680 / 4,680 | 77.0042 reports/s | 757.624 / 963.449 / 994.945 ms | 临界，通过但不作为稳定档 |
| 优化后：80 节点、首轮错峰 | 4,800 / 4,800 / 4,800 | 74.0639 reports/s | 未单列 / 未单列 / 1,286.075 ms | 不通过：P99 > 1 s |

因此，优化后的**最高复测稳定档为约 75 reports/s**：两轮各 60 s、各完成 4,500
次真实 `gRPC -> Manager -> MySQL` 同步持久化，成功率与精确落库率均为 100%，P99
分别为 128.965 ms 和 146.459 ms。相较旧整秒突发的 65 reports/s 稳定档，施压速率
提高约 15.4%，P99 从约 0.93 s 降至约 0.13--0.15 s；由于同时改变了连接复用、日志
开关与首轮相位，不能将该改善归因于单一因素。

原始复测结果：

- `benchmark/results/optimized-stagger-75w-60s-1000ms.csv`
- `benchmark/results/optimized-stagger-75w-60s-1000ms-rep2.csv`
- `benchmark/results/optimized-stagger-78w-60s-1000ms.csv`
- `benchmark/results/optimized-stagger-80w-60s-1000ms.csv`

可用于简历的有边界表述：

> 在 Windows Docker Linux 基准环境中，模拟 75 节点按 1 s 周期错峰上报；两轮各
> 60 s 均完成 4,500 次 `gRPC -> Manager -> MySQL` 同步持久化，成功率与精确落库率
> 100%，端到端 P99 为 129/146 ms。78 reports/s 已接近 P99 1 s 边界，因此以约
> 75 reports/s 作为 Legacy synchronous persistence 的历史可复测稳定档，不代表当前
> Sharded Manager 的容量。

## Legacy 扩展容量与 10 分钟稳定性复测（2026-08-13）

为避免只用 30 或 60 秒样本描述系统，本轮从干净重建的 Manager 开始，增加了
100/300 节点的整秒突发容量档，以及 75 节点的 10 分钟错峰稳定性档。所有完整档
均以客户端 CSV 成功记录、按独立 `run-id` 查询的 MySQL 行数和容器存活三项同时
核验；因此“成功”仍表示 Manager 已同步处理并写入 MySQL，而非仅 gRPC 收到请求。

| 场景 | 预期样本 / 成功 / 精确落库 | P50 / P95 / P99 | 结论 |
| --- | --- | --- | --- |
| 100 节点、1 s 周期、60 s、整秒突发 | 6,000 / 6,000 / 6,000 | 1,152.852 / 1,225.919 / 1,300.236 ms | 无丢失，但 P99 超过 1 s，不是稳定档 |
| 300 节点、1 s 周期、60 s、整秒突发 | 18,000 / 18,000 / 18,000 | 3,547.552 / 3,767.315 / 3,883.510 ms | 无丢失，但全局串行路径发生明显排队 |
| Legacy 75 节点、1 s 周期、600 s、首轮错峰 | 45,000 / 45,000 / 45,000 | Legacy synchronous 13.119 / 459.494 / 676.736 ms | 通过：旧架构持续 10 分钟仍保持 P99 < 1 s |

75 节点、10 分钟轮次还以 Docker `stats` 按秒采集容器资源，得到：

| 组件 | 平均 CPU | CPU 峰值 | 内存峰值 |
| --- | --- | --- | --- |
| Manager | 8.68% | 11.61% | 13.3 MiB |
| MySQL | 29.92% | 40.06% | 485.6 MiB |

500 节点、60 秒整秒突发档在外层 8 分钟窗口内未完成，停止前已有 17,521 行测试
数据写入；该档没有完整 CSV 和终态核验，**明确标记为中止，不纳入任何吞吐或成功率
结论**。它只说明历史全局串行“指标计算 + 多表同步写库”实现不适合直接承接该级别
的集中突发。

本轮可用于简历的 Pico 风格表述：

> Legacy Manager 在 **75 节点、1 s 周期、10 分钟**的历史端到端稳定性压测中，完成
> **45,000 次** `gRPC -> Manager -> MySQL` 同步持久化，**成功率与精确落库率均为
> 100%**，Legacy synchronous P50/P95/P99 为 **13.1/459.5/676.7 ms**；Manager
> 平均 CPU **8.68%**、峰值内存 **13.3 MiB**。另在 **100/300 节点**整秒突发对照下
> 记录 Legacy P99 **1.30/3.88 s**，仅说明旧同步写入架构的历史容量边界。
