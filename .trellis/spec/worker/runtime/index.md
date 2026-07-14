# Worker Runtime Guidelines

## Scope

`worker/` is the Linux-side collector and gRPC Push client. Entry point:
`worker/src/main.cpp`; collection orchestration:
`worker/src/monitor/metric_collector.cpp`; transport:
`worker/src/rpc/monitor_pusher.cpp`.

## Rules

- Keep every collector behind `MonitorInter::UpdateOnce` and make a missing
  optional kernel/eBPF capability degrade without terminating the Worker.
- Preserve `NetInfo` units: byte rates are `kB/s` and packet rates are
  `packets/s` (`proto/net_info.proto`). A unit change requires an end-to-end
  Manager and storage review.
- Do not run unquoted shell commands with externally controlled values. The
  eBPF/TC lifecycle must detach only hooks owned by this process.
- Keep Push failures visible and bounded: no infinite busy retry, no silent
  data loss claim, and no full metric dump at normal production log level.
- Worker arguments and runtime resources need explicit failure handling;
  `std::stoi`, thread creation, gRPC calls, `/proc`, device, and eBPF failures
  must not crash the process or be reported as successful telemetry.

## Verification

Build the Worker on the target Linux distribution. For network changes, test
both eBPF and `/proc/net/dev` paths, TC attach/detach cleanup, unit agreement,
and a controlled `iperf3` workload.
