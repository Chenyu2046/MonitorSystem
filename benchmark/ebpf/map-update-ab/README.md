# eBPF kernel update hot-path A/B

This directory contains benchmark-only TC ingress programs. It does not modify
`worker/src/ebpf/net_stats.bpf.c` or `worker/src/monitor/net_ebpf_monitor.cpp`.

`global_update.bpf.c` uses a shared `BPF_MAP_TYPE_HASH` value and verifier-
supported atomic additions. `percpu_update.bpf.c` uses a
`BPF_MAP_TYPE_PERCPU_HASH` value and local additions. Both variants have the
same ifindex key, four-field `net_stats` value, lookup/branch path, ingress
hook, and `TC_ACT_OK` return.

The matrix controller runs a stable Windows iperf3 3.21 UDP client through the
VirtualBox NAT port forward. Ubuntu iperf3 parallel UDP is intentionally not
used. Guest `taskset` is applied to the iperf3 server as a best-effort workload
affinity; it does not strictly isolate VM vCPUs, IRQs, or the TC hook.

Build inside Ubuntu:

```bash
cd ~/MonitorSystem
bash benchmark/ebpf/map-update-ab/build.sh
```

The controller and evidence format are documented in the generated
`FINAL_REPORT.md` under `benchmark/evidence/linux-ebpf-map-update-ab-YYYYMMDD/`.
