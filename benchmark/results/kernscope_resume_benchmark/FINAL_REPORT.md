# Resume Finalists

# KernScope Resume-grade Performance & Diagnostic Benchmark Report

## 1. 报告结论

本轮 benchmark 的目标不是制造一个单一的“性能提升百分比”，而是验证 KernScope 在 WSL2 下能否完成真实的 eBPF attach、采集 OnCPU/OffCPU 证据、把 CPU 证据送入 Worker→gRPC→Manager→RootCauseEngine 链路，并用可重复的 workload 给出可解释的分类结果。

结论如下：

1. eBPF Worker 的 libbpf 兼容性问题已修复。ENABLE_EBPF configure/build 通过，5/5 个诊断 BPF object 编译通过，CTest 3/3 通过，TCP/BlockIO/Scheduler/OnCPU/OffCPU attach smoke 均成功。
2. OnCPU profiling 在 5 个真实 workload runs 上得到 100% top-1 hotspot accuracy、100% stack capture、100% symbolization；样本速率均值 989.54 samples/s。
3. OffCPU lock contention 在 9 个正向 mutex 场景和 6 个负向 sleep/disk 场景上，按预注册的 20% attribution threshold 得到 TP=9、FN=0、FP=0、TN=6，precision/recall 均为 100%，FPR 为 0%。
4. 完整端到端 RCA 只完成了受限 CPU-vs-Normal 矩阵：20 runs，accuracy 90%、precision 83.33%、recall 100%、FPR 20%。10 个 CPU 正向样本全部识别正确，但 10 个 Normal 样本中有 2 个误报，不能隐藏。
5. 网络 loopback workload 已完成 3 轮，iperf3 吞吐均值 106.141 Gb/s；但当前 harness 没有暴露 KernScope eBPF byte count，因此不能宣称网络计数准确率、计数一致性或探针 overhead。

本报告中的 profiling 结果分为两类：OnCPU/OffCPU 是 standalone ProbeController + 真实内核 eBPF workload；CPU-vs-Normal 是真实 Worker→gRPC→Manager→EvidenceBuilder→RootCauseEngine 链路。二者不能混写成一个“完整 RCA 准确率”。

## 2. 测试范围与环境

### 2.1 仓库与代码版本

- Repository：`Chenyu2046/MonitorSystem`
- Branch：`feature/kernscope-observability`
- Benchmark 起始基线：`2099b10aec92408af16a9aded3c5fb8e94fb1bf1`
- 本轮代码修复：`worker/src/diagnostics/probe_controller.cpp`、`worker/src/monitor/cpu_stat_monitor.cpp`、`worker/src/rpc/monitor_pusher.cpp`、`manager/src/host_manager.cpp` 及 CPU monitor header。
- 诊断日志开关：`KERNSCOPE_DIAGNOSTIC_LOG=1`；默认关闭，打开后记录 state、active probes、evidence 和 root causes。

### 2.2 执行环境

| 项目 | 值 |
|---|---|
| Host | Windows 11 + WSL2 Ubuntu 24.04 |
| Kernel | `6.18.33.2-microsoft-standard-WSL2` |
| CPU | AMD Ryzen 7 5800H，8 cores / 16 logical CPUs |
| WSL memory | 6.7 GiB visible memory，2.0 GiB swap |
| clang | Ubuntu clang 18.1.3 |
| CMake | 3.28.3 |
| libbpf | pkg-config 1.3.0 |
| bpftool | direct binary v7.4.0，libbpf v1.4 |
| BTF | `/sys/kernel/btf/vmlinux`，6,677,359 bytes |
| Docker | Engine 29.6.2，Manager/MySQL services available |

WSL2 不是 bare-metal 环境，因此所有结论都带有 WSL2 边界；特别是调度、CPU 频率、loopback 网络和 privileged eBPF 行为不能直接外推为物理机生产结果。

完整环境记录：`environment.md`、`raw/environment/environment-command.txt`、`raw/environment/environment-final-command.txt`。

## 3. 本轮修复了什么，以及修复效果

### 3.1 libbpf API 兼容性修复

原始 ENABLE_EBPF build 在当前 libbpf 头文件下失败，原因是旧代码调用了已不可用的 `bpf_object__find_map_fd(...)`。本轮将 7 处 map lookup 统一替换为官方 API：

```cpp
bpf_object__find_map_fd_by_name(loaded.object, "map_name")
```

覆盖的 map：

- `tcp_diag_map`
- `block_io_stats_map`
- `sched_stats_map`
- `oncpu_stack_counts`
- `oncpu_stack_traces`
- `offcpu_aggregate`
- `offcpu_stack_traces`

修复后的验证结果：

- ENABLE_EBPF CMake configure：PASS
- ENABLE_EBPF full build：PASS
- 诊断 BPF object compile：5/5 PASS
- CTest：3/3 PASS
- 真实 privileged attach smoke：TCP、BlockIO、Scheduler、OnCPU、OffCPU 均 `last_error=0`

这证明修复解决了“编译/API 兼容性”和“基本 attach readiness”问题，但不等价于所有生产内核、所有 BTF、所有 map 数据语义都已验证。

### 3.2 WSL2 CPU 采集 fallback

WSL2 环境没有 `/dev/cpu_stat_monitor`，这会让 Worker 没有 CPU evidence，导致端到端 CPU RCA 无法形成闭环。本轮保留原有内核设备路径优先级；当设备不可用时，改为读取真实 `/proc/stat` 的 per-CPU counter，并用相邻采样的 delta 计算：

- CPU busy percentage
- user/system/nice/idle/iowait/irq/softirq percentage

这不是 fake data，也不是固定填充值；数据来自当前 WSL2 内核的 `/proc/stat`。该 fallback 直接支持了后续 CPU-vs-Normal E2E 测试。

### 3.3 状态与证据可观测性

在 Worker 和 Manager 增加默认关闭的诊断日志：

- Worker：timestamp、`NORMAL/SUSPECT/DIAGNOSTIC/PROFILING/COOLDOWN`、active probes、`probes_ready`
- Manager：diagnostic state、evidence type/severity、root cause type/confidence

日志只在 `KERNSCOPE_DIAGNOSTIC_LOG=1` 时启用，不改变默认生产日志量。它用于验证状态机和证据链，不是 benchmark 数值本身。

## 4. 测试一：构建、单测与 attach smoke

### 4.1 测试目的

验证修复后的 ENABLE_EBPF Worker 能够在当前 WSL2 toolchain 下完成 configure、编译、单测，并且在实际 privileged 环境中 attach 诊断 probes。

### 4.2 测试方法

使用 WSL2 构建目录和当前 BTF/bpftool：

- `VMLINUX_H=/mnt/f/.../worker/src/ebpf/vmlinux.h`
- `BPFTOOL=/usr/lib/linux-tools/6.8.0-137-generic/bpftool`
- `-DBUILD_MANAGER=ON`
- `-DBUILD_BENCHMARK=OFF`
- `-DENABLE_EBPF=ON`
- `-DENABLE_MYSQL=OFF`
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo`
- `-g -fno-omit-frame-pointer`

之后运行 CTest，并用独立 attach smoke harness 依次请求 suspect、diagnostic、profiling_oncpu、profiling_offcpu 状态。

### 4.3 结果

| 检查项 | 结果 |
|---|---:|
| ENABLE_EBPF configure/build | PASS |
| BPF objects | 5/5 PASS |
| CTest | 3/3 PASS |
| TCP attach | PASS，last_error=0 |
| BlockIO attach | PASS，last_error=0 |
| Scheduler attach | PASS，last_error=0 |
| OnCPU attach | PASS，last_error=0 |
| OffCPU attach | PASS，last_error=0 |

原始证据：`raw/worker-ebpf-fixed-configure.log`、`raw/worker-ebpf-fixed-build-procfs.log`、`raw/ctest-ebpf-fixed-after-procfs.log`、`raw/probe-attach-smoke.log`。

## 5. 测试二：OnCPU profiling

### 5.1 测试目的

验证 OnCPU eBPF probe 能否在真实 CPU workload 上捕获 stack，并把用户态地址解析到预先已知的热点函数；同时观察每轮采样吞吐和 unknown 样本。

### 5.2 workload 设计

每轮持续约 10 秒，启动 10 个真实用户线程：

- 7 个线程循环执行 `BusyFunctionA`，期望占比约 70%
- 2 个线程循环执行 `BusyFunctionB`，期望占比约 20%
- 1 个线程执行 `BackgroundNoise`，期望占比约 10%

测试程序使用 O2、debug symbols、frame pointer 和 `rdynamic`，通过 `dladdr` 找到目标函数符号，再按实际观测到的 BusyFunction symbol 选择目标 TGID。采样结果只统计目标 TGID，避免把 WSL2 中其他进程的 perf sample 混入准确率。

### 5.3 判定方法

- top-1 hotspot：每轮最高频已标注热点必须为 `BusyFunctionA`
- stack capture：valid stack samples / total samples
- symbolization：成功解析的 frames / 总 frames
- unknown：捕获到但无法映射为三类预注册 workload function 的样本
- 不使用人工填充、随机数或离线伪造 stack

### 5.4 每轮结果

| run | total samples | valid stacks | symbolized frames | A/B/noise/unknown | samples/s | top1 |
|---|---:|---:|---:|---|---:|---|
| 01 | 9,900 | 9,900 | 212/212 | 6,930 / 1,980 / 990 / 0 | 989.893 | BusyFunctionA |
| 02 | 9,899 | 9,899 | 212/212 | 6,929 / 1,980 / 990 / 0 | 989.828 | BusyFunctionA |
| 03 | 9,897 | 9,897 | 184/184 | 6,929 / 1,979 / 989 / 0 | 988.314 | BusyFunctionA |
| 04 | 9,898 | 9,898 | 220/220 | 6,928 / 1,980 / 990 / 0 | 989.748 | BusyFunctionA |
| 05 | 9,900 | 9,900 | 220/220 | 6,928 / 1,980 / 990 / 2 | 989.913 | BusyFunctionA |

### 5.5 结果与效果

- top-1 hotspot accuracy：100%，5/5
- stack capture success：100%，5/5
- stack symbolization success：100%，5/5
- samples/sec：mean 989.54，median 989.83，p95 989.91，min 988.31，max 989.91
- unknown hotspot sample rate：mean 0.004%，p95 0.02%

效果说明：在可控、符号已知的真实 CPU workload 上，OnCPU 采集和归因链路稳定，热点排序与预期负载一致。这里验证的是 profiling correctness 和采样稳定性；没有对照“无 probe vs 有 probe”的 CPU overhead，因此不能写成性能提升百分比。

原始证据：`raw/oncpu/run_01/result-final.log` 至 `raw/oncpu/run_05/result-final.log`；汇总：`summary/oncpu_profile.csv`、`summary/oncpu_profile_runs.csv`。

## 6. 测试三：OffCPU lock contention

### 6.1 测试目的

验证 OffCPU eBPF probe 能否从真实阻塞 workload 中识别 mutex/futex 相关等待，并区分 sleep 与 disk I/O 等负向场景。

### 6.2 workload 设计

正向场景：

- LOW：8 个 mutex threads，持锁 1 ms
- MEDIUM：12 个 mutex threads，持锁 3 ms
- HIGH：16 个 mutex threads，持锁 8 ms

负向场景：

- SLEEP_NEGATIVE：8 个 sleep threads
- DISK_NEGATIVE：4 个 `pread/fadvise` threads，访问 `/tmp/kernscope-offcpu-disk.bin`

每个场景 3 runs，共 15 runs。最终 narrow classifier 只把 `futex` 或 `mutex` 相关符号归入 lock-related，避免把宽泛的包含 “lock” 子串的内核符号误判成锁竞争。

### 6.3 预注册分类规则

在查看最终结果前固定阈值：

```text
lock_attribution_pct >= 20%  -> LOCK_CONTENTION
lock_attribution_pct <  20%  -> NEGATIVE
```

同一个阈值用于 low、medium、high、sleep 和 disk，未按场景调参。

### 6.4 聚合结果

| scenario | runs | attribution mean | median | p95 | min–max | classification |
|---|---:|---:|---:|---:|---:|---|
| LOW | 3 | 43.11% | 44.10% | 44.29% | 40.91–44.31% | positive |
| MEDIUM | 3 | 38.48% | 38.68% | 38.83% | 37.92–38.85% | positive |
| HIGH | 3 | 36.99% | 37.14% | 37.23% | 36.59–37.24% | positive |
| SLEEP_NEGATIVE | 3 | 10.86% | 8.10% | 16.42% | 7.15–17.35% | negative |
| DISK_NEGATIVE | 3 | 4.35% | 5.63% | 5.72% | 1.68–5.73% | negative |

### 6.5 混淆矩阵与效果

| | 预测 lock | 预测 negative |
|---|---:|---:|
| 实际 lock | TP=9 | FN=0 |
| 实际 negative | FP=0 | TN=6 |

- precision：100%
- recall：100%
- false-positive rate：0%
- stack capture：除 HIGH-03 为 99.934% 外，其余 runs 为 100%；symbolization 全部为 100%
- 所有最终 narrow runs 的 attach `last_error=0`

效果说明：在预先定义的 mutex/sleep/disk 场景边界内，OffCPU lock attribution 能把正向锁竞争和两类负向 workload 分开。它证明的是 OffCPU evidence/classifier 的有效性，不证明 Worker 已经能在完整 RCA 链路中自动产生 Lock root cause。

原始证据：`raw/offcpu/*-narrow-*`；汇总：`summary/offcpu_lock.csv`、`summary/offcpu_lock_runs.csv`。

## 7. 测试四：端到端 CPU-vs-Normal RCA

### 7.1 测试目的

验证真实 Worker 采集到的 CPU evidence 能否经过 gRPC 发送到 Manager，并由 EvidenceBuilder 和 RootCauseEngine 识别 CPU saturation；同时设置 no-load Normal 负向集，测量误报。

### 7.2 链路与运行方式

测试链路：

```text
real workload -> Worker monitor -> diagnostic state/probes
              -> gRPC -> Manager -> EvidenceBuilder
              -> RootCauseEngine -> GetIncidents query
```

运行参数：

- Manager：本地 no-MySQL build，监听 `127.0.0.1:50053`
- Worker：ENABLE_EBPF=ON，1 秒采集/推送间隔，开启 `KERNSCOPE_DIAGNOSTIC_LOG=1`
- CPU 正向组：16 个 `taskset yes` 负载，10 runs
- Normal 负向组：无外部 CPU load，10 runs
- 每轮运行约 15 秒；CPU-04 为较长观察轮，其余最终计入 runs 为 CPU-04..CPU-13
- 初始 CPU-01..CPU-03 在 procfs fallback 和 Manager diagnostic logging 完成前执行，不计入最终矩阵

### 7.3 结果

| group | runs | TP | FP | FN | TN | accuracy | precision | recall | FPR |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU saturation | 10 | 10 | 0 | 0 | — | 100% detection | — | 100% | — |
| Normal no-load | 10 | — | 2 | — | 8 | — | — | — | 20% |
| CPU vs Normal | 20 | 10 | 2 | 0 | 8 | 90% | 83.33% | 100% | 20% |

10/10 CPU saturation runs 返回 `CPU_SATURATION` root cause。Normal 组 10 轮中 8 轮为 TN，2 轮产生 CPU false positive；这两个误报已经保留在报告和 confusion matrix 中，没有删除或重新定义为“异常”。

### 7.4 效果说明

CPU 正向链路的 recall 为 100%，说明 `/proc/stat` fallback 产生的 CPU evidence 能进入 Manager 并触发 RootCauseEngine；但 Normal FPR 为 20%，说明当前 anomaly threshold、WSL2 背景噪声或基线建模仍不足以支撑“无误报”的生产结论。

因此本轮只能宣称 restricted CPU-vs-Normal RCA 结果，不能宣称完整 CPU/Disk/Network/Lock/Normal 五类 RCA accuracy。Disk、Network、Lock 的端到端 labelled ground-truth set 尚未采集。

原始证据：`raw/rca/cpu-e2e-cpu-04..cpu-13/`、`raw/rca/cpu-e2e-normal-01..normal-10/`；汇总：`summary/rca_accuracy.csv`、`summary/rca_confusion_matrix.md`。

## 8. 测试五：网络 workload

### 8.1 测试方法

在 WSL2 loopback `127.0.0.1` 上执行 3 轮：

- iperf3 server：single-run mode
- client：`-t 10 -P 4 -J`
- 每轮独立端口、独立 server/client log 和 JSON
- 同时记录 `/sys/class/net/lo/statistics/rx_bytes` 与 `tx_bytes` 的前后差值

### 8.2 结果

| run | iperf3 duration | throughput | client status | RX/TX delta |
|---|---:|---:|---:|---:|
| 01 | 10.00 s | 103.812 Gb/s | 0 | 130,015,592,520 / 130,015,592,520 bytes |
| 02 | 10.00 s | 110.161 Gb/s | 0 | 137,958,021,128 / 137,958,021,128 bytes |
| 03 | 10.00 s | 104.450 Gb/s | 0 | 130,812,494,756 / 130,812,494,756 bytes |

统计：mean 106.141 Gb/s，min 103.812 Gb/s，max 110.161 Gb/s。

### 8.3 结果边界

这组数据证明 loopback 网络 workload 和 iperf3 采集成功；它不证明 KernScope 网络 probe 的 byte accounting 正确，因为当前 harness 没有从 eBPF map 导出 probe byte count。`summary/network.csv` 标记为 `PARTIAL_NOT_VERIFIED`，不把它晋级为网络监控准确率或 overhead 结果。

原始证据：`raw/network/run_01..03/iperf3.json`、各目录 `server.log`、`client.log`、`run.meta`。

## 9. 稳定性与 supporting evidence

这些结果不是本轮五个 Resume Finalists，但保留用于说明系统路径的工程表现。

### 9.1 gRPC/Manager outage recovery

使用真实 no-eBPF Worker，本地 Manager 做 3 轮故障注入：warmup 15 秒、Manager outage 30 秒、restart/recovery observation 30 秒。总计 225 个采样点，Worker alive 最小值为 1。

| run | recovery latency | failed/dropped log lines | queue depth |
|---|---:|---:|---|
| grpc-30s-01 | 16,530.607 ms | 46 | NOT EXPOSED |
| grpc-30s-02 | 17,458.119 ms | 47 | NOT EXPOSED |
| grpc-30s-03 | 4,048.953 ms | 33 | NOT EXPOSED |
| mean | 12,679.226 ms | 42/run | NOT EXPOSED |

效果：Worker 在 Manager outage 期间保持存活并最终恢复；但由于 runtime 没有暴露 queue depth，不能宣称无损缓冲、无丢失或持久队列保证。

### 9.2 Docker Manager synthetic ingestion

Docker Manager/MySQL 路径使用合成 gRPC client 做 3 轮，每轮 10 workers、30 秒、1 秒 interval：

- 3×300 reports，成功率 900/900
- MySQL 按 run_id 精确核对为 300、300、300 行
- 该 client 不是真实 Worker，因此其 p50/p95/p99 只能说明 Manager synthetic ingestion path，不能写成真实采集链路性能

### 9.3 adaptive state overhead

此前 adaptive runs 记录过 normal、diagnostic-trigger、profiling-intended 的 CPU/RSS，但当时 eBPF OFF 且 state transition 未独立观测；绝对 CPU 数值也低于本次 WSL2 采样分辨率。本轮虽然补充了状态日志，但尚未重新完成独立 state-observed 的 60 秒多轮 overhead matrix，因此不发布 overhead 百分比。

## 10. 总体效果评价

| 能力 | 评价 | 证据强度 |
|---|---|---|
| eBPF build/attach readiness | 已从 API 编译阻塞恢复，当前 WSL2 attach smoke 全通过 | 高，针对当前环境 |
| OnCPU profiling | 热点排序、stack capture、symbolization 稳定 | 高，5 个真实 workload runs |
| OffCPU lock attribution | 正向锁竞争与 sleep/disk 负向场景分离良好 | 高，但限定在 standalone classifier |
| CPU E2E RCA | 正向 recall 100%，但 Normal FPR 20% | 中，受限二分类矩阵 |
| Network monitoring | 负载和 iperf 吞吐可复现，probe accounting 未闭环 | 低/部分验证 |
| outage recovery | Worker 存活并恢复，但 queue depth 未暴露 | 中，3 个故障注入 runs |
| adaptive overhead | 当前证据不足，不发布百分比 | 未验证 |

本轮没有可用于“优化前后性能提升”的 baseline/control 对照，因此不声称 KernScope 性能提升了多少。已测出的数字主要说明 profiling correctness、分类边界、链路可用性和当前误报边界。

## 11. Resume-grade 结果

建议只使用以下四条带限定的结果：

- Validated real WSL2 eBPF OnCPU profiling across 5 runs with 100% top-1 hotspot accuracy, 100% stack capture, and 100% symbolization; measured 989.54 samples/s mean under a 10-thread workload.
- Built a real OffCPU lock-contention classifier across 9 positive and 6 negative runs, achieving 100% precision, 100% recall, and 0% false-positive rate at a pre-registered 20% attribution threshold.
- Integrated and validated a real Worker→gRPC→Manager→EvidenceBuilder→RootCauseEngine CPU-vs-Normal path across 20 runs: 90% accuracy, 83.33% precision, 100% recall, and 20% false-positive rate.
- Restored ENABLE_EBPF compatibility with libbpf 1.3/1.4 APIs; passed 5/5 BPF object builds, 3/3 CTest checks, and privileged attach smoke for TCP, block I/O, scheduler, OnCPU, and OffCPU probes.

使用边界：前两条是 standalone eBPF benchmark；第三条只针对 CPU-vs-Normal；不写完整五类 RCA、不写网络 byte-accounting accuracy、不写未经对照测量的 overhead reduction。

## 12. 原始证据索引

- 环境：`environment.md`、`raw/environment/`
- 构建与单测：`raw/worker-ebpf-fixed-configure.log`、`raw/worker-ebpf-fixed-build-procfs.log`、`raw/ctest-ebpf-fixed-after-procfs.log`
- attach：`raw/probe-attach-smoke.log`
- OnCPU：`raw/oncpu/`、`summary/oncpu_profile.csv`、`summary/oncpu_profile_runs.csv`
- OffCPU：`raw/offcpu/`、`summary/offcpu_lock.csv`、`summary/offcpu_lock_runs.csv`
- E2E RCA：`raw/rca/`、`summary/rca_accuracy.csv`、`summary/rca_confusion_matrix.md`
- Network：`raw/network/`、`summary/network.csv`
- outage/synthetic/adaptive supporting evidence：`raw/grpc-reliability/`、`raw/manager-synthetic-*.log`、`raw/adaptive/`
