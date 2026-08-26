# Global Hash vs Per-CPU Hash：kernel update hot-path A/B

## 结论

在 Ubuntu 24.04.4 / Linux 6.8.0-138-generic / 4 vCPU / VirtualBox NAT / `enp0s3`
上，按本实验的 180 个有效窗口，PerCPU Hash 没有得到“降低 kernel hot-path
共享更新竞争”的可重复支持。

关键结论是：

- Global 90 轮、PerCPU 90 轮，18 个组合均为双方各 5 轮；失败 raw 为 0。
- 组合级 median 的 `bpf_cost_reduction_pct` 在 18 个组合中只有 3 个为正；总体 median 为 `-9.89%`，即 PerCPU 的观测平均 BPF cost 整体更高，而不是更低。
- 按 CPU 聚合 6 个 payload/stream 组合：1 CPU 为 `-11.64%`，2 CPU 为 `-4.71%`，4 CPU 为 `-15.96%`。没有出现随 1→2→4 CPU 单调放大的 PerCPU 优势。
- 组合级 `throughput_gain_pct` 总体 median 为 `-0.02%`；1/2/4 CPU 的 median 分别为 `-0.03%`、`-0.02%`、`0.00%`。吞吐基本持平，不能据此宣称 PerCPU 带来吞吐收益。
- 组合级 guest CPU difference 的总体 median 为 `-0.81 pp`（定义为 Global CPU - PerCPU CPU），说明 PerCPU 没有稳定降低 CPU；1/2/4 CPU 的 median 分别为 `-0.93/-0.81/-0.82 pp`。

因此，本次结果更接近“在当前 VirtualBox/NAT、10 Mbps/stream、5 秒窗口和这两个
等价 ingress 程序下，PerCPU 没有展示预期的 hot-path 优势”。这是否适合生产，仍
要结合 production 已有 runtime/吞吐证据和 userspace aggregation 成本综合判断；本
实验本身不支持把 PerCPU 的选择表述成已被该 A/B 证明的性能收益。

## 实验边界与控制变量

- 只使用 benchmark-only 文件：`benchmark/ebpf/map-update-ab/`。
- 两个程序都只挂载 `enp0s3` ingress，ifindex=2，lookup 同一 ifindex key，value 都是 production 的四个 `u64` 字段，均返回 `TC_ACT_OK`。
- Global 使用 `BPF_MAP_TYPE_HASH` 和 verifier 通过的 atomic add；PerCPU 使用 `BPF_MAP_TYPE_PERCPU_HASH` 和本地加法。没有 spin-lock Global 变体。
- 每轮先用同 variant 挂载程序，再执行 5 秒 warmup；warmup 后读取 BPF runtime before，再执行 5 秒 measured window，最后读取 after。
- 流量由 Windows iperf3 3.21 作为 client，经 VirtualBox NAT port-forward 到 guest；Ubuntu iperf3 3.16 并行 UDP 路径未使用。
- 矩阵为 64/512/1500B × 1/8 streams × 1/2/4 CPU × 5 rounds × 2 variants = 180 轮。
- `taskset` 只绑定 guest iperf3 server：1 CPU=`0`，2 CPU=`0,1`，4 CPU=`0-3`。它没有严格绑定 BPF 执行 CPU，也没有隔离 VirtualBox vCPU、IRQ、softirq 或 NAT host path。

## 主要数据

| CPU limit | 组合数 | BPF cost reduction median | 组合范围 | throughput gain median | CPU difference median |
|---:|---:|---:|---:|---:|---:|
| 1 | 6 | -11.64% | -21.46% .. +0.26% | -0.03% | -0.93 pp |
| 2 | 6 | -4.71% | -19.07% .. +1.19% | -0.02% | -0.81 pp |
| 4 | 6 | -15.96% | -29.65% .. +2.30% | 0.00% | -0.82 pp |

这里负的 cost reduction 表示 `percpu_avg_cost` 高于 `global_avg_cost`。组合级比较
严格使用：

```text
bpf_cost_reduction_pct = (global_avg_cost - percpu_avg_cost) / global_avg_cost * 100
throughput_gain_pct = (percpu_pps - global_pps) / global_pps * 100
cpu_difference_pp = global_cpu_pct - percpu_cpu_pct
```

代表性组合：

- 64B/1 stream：1 CPU `-0.88%`，2 CPU `+1.19%`，4 CPU `-9.89%`。
- 512B/1 stream：1/2/4 CPU 分别 `-21.46%/-19.07%/-15.96%`。
- 1500B/1 stream：1/2/4 CPU 分别 `-20.03%/-1.72%/-29.65%`。
- 1500B/8 stream：1/2/4 CPU 分别 `-11.64%/-4.71%/+2.30%`。

## 对问题的直接回答

### Global 与 PerCPU 是否存在可重复差异？

存在可重复的“程序 runtime 观测不同”，但方向不是 PerCPU 更快：多数组合
`percpu_avg_bpf_cost_ns` 更高。差异并未随 CPU 从 1 到 4 单调增强，因此不能解释成
共享 value 竞争被 PerCPU 稳定消除。

### 差异主要体现在 runtime / PPS / CPU 哪一个？

主要体现在 runtime，且是 PerCPU 侧偏高；PPS 近似持平，CPU 没有稳定优势。18 个
组合中只有 3 个 cost reduction 为正，6 个 throughput gain 为正，5 个 CPU difference
为正。

### 差异是否随 1→2→4 CPU 增大？

否。按 CPU 的 cost reduction median 为 `-11.64% -> -4.71% -> -15.96%`，不是单调
增大；吞吐 median 仍在零附近。当前 workload/VM 没有形成可观测的共享 value 高竞争
梯度。

### 当前 VirtualBox/NAT 是否成为更早瓶颈？

很可能是重要限制，但本报告只作边界判断，不把它当作已被单独隔离证明的根因：
PPS/吞吐在不同 map variant 间基本持平，而 guest CPU、softirq、NAT 转发和 IRQ
调度共同参与了测量；4 vCPU VM 也无法严格把 BPF 执行限定在测试 CPU 集合。因而
本结果不能外推到裸机、多队列 NIC 或更高 event-rate 的生产网络。

### production 选择 PerCPU Hash 是否得到实验支持？

就“PerCPU 降低 kernel hot-path 更新成本”这一具体命题而言：没有得到支持；结果更
偏向否定或至少无法确认。production 仍已恢复运行，且本实验没有修改生产采集逻辑。
此前已有的 production runtime、ON/OFF 吞吐和 userspace aggregation 证据应保持独立，
不能与本报告的 benchmark-only update A/B 混写。

## 恢复与完整性

实验结束后已：

- 清理 benchmark runner 和 benchmark clsact；
- 重启 production Worker；
- 确认 Manager 进程存在、MySQL/SSH systemd 服务为 active；
- 确认 `enp0s3` ingress 为 `tc_ingress`、egress 为 `tc_egress`，两个 production tag 与原程序一致；
- 确认远端仓库仍在 `main`、HEAD=`8b141c35687f8e82f243b24d214ee5ffd32bf96e`，两份 production source `git diff` 为 clean，且 `git hash-object` 与 HEAD blob 一致。

详细恢复输出见 [`restore-after.txt`](restore-after.txt)，逐轮 raw 和 CSV 见本目录，
程序/脚本源代码见 [`benchmark/ebpf/map-update-ab/`](../../ebpf/map-update-ab/)。
