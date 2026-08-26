# Resume-safe metrics

Metric: Global vs PerCPU kernel update hot-path cost reduction
Environment: Ubuntu 24.04.4, Linux 6.8.0-138-generic, 4 vCPU, VirtualBox NAT, enp0s3, Windows iperf3 3.21 client
Method: Benchmark-only TC ingress; Global `BPF_MAP_TYPE_HASH` atomic add vs PerCPU `BPF_MAP_TYPE_PERCPU_HASH` local add; BPF runtime delta from `bpftool prog show -j` after 5s warmup and during 5s measured UDP window
Runs: 18 matched combinations × 5 rounds × 2 variants = 180 valid rounds
Median: Combination-level `bpf_cost_reduction_pct` -9.89%; by CPU limit 1/2/4: -11.64%/-4.71%/-15.96%
P95: Combination-level cost-reduction distribution P95 +1.19%; only 3/18 combinations were positive
Evidence: [`comparison.csv`](comparison.csv), [`global.csv`](global.csv), [`percpu.csv`](percpu.csv), [`raw/`](raw/)
Resume-safe wording: “在 Ubuntu 24.04 / Linux 6.8 / 4 vCPU VirtualBox 环境的 180 轮 TC eBPF A/B 中，PerCPU Hash 相对 Global Hash 未显示稳定 kernel update hot-path 降本；组合级 median 差异为 -9.89%。”
Limitation: Guest taskset 只约束 iperf3 server；未严格隔离 BPF CPU、IRQ、softirq、VirtualBox vCPU 或 NAT；不能外推为裸机/生产 NIC 结论。

Metric: Global vs PerCPU throughput gain
Environment: Same as above
Method: Guest iperf3 server received PPS; Windows iperf3 3.21 UDP client; same payload/stream/CPU matrix and warmup
Runs: 18 matched combinations, 5 rounds per variant
Median: Combination-level `throughput_gain_pct` -0.02%; by CPU limit 1/2/4: -0.03%/-0.02%/0.00%
P95: Not used as a primary claim; the combination comparison CSV contains every matched result
Evidence: [`comparison.csv`](comparison.csv)
Resume-safe wording: “同一 VirtualBox/NAT 流量矩阵下，PerCPU 相对 Global 的接收 PPS median gain 约 -0.02%，未观察到稳定吞吐提升。”
Limitation: NAT/guest softirq and the 10 Mbps-per-stream traffic cap may dominate before map-update contention becomes visible.

Metric: Production restoration
Environment: Same VM, remote repository main at `8b141c35687f8e82f243b24d214ee5ffd32bf96e`
Method: Restart production Worker; verify Manager process, MySQL/SSH active, production `tc_ingress` and `tc_egress`, and clean production-source diff
Runs: One post-run restoration verification
Median: N/A
P95: N/A
Evidence: [`restore-after.txt`](restore-after.txt)
Resume-safe wording: “压测结束后已恢复 production Worker、Manager、MySQL 及 enp0s3 production TC ingress/egress；生产采集源文件未修改。”
Limitation: This is a restoration check, not a new production performance measurement.
