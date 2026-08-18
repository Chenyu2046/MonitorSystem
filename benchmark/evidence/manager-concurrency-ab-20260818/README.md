# KernScope Manager Legacy vs Sharded Performance Report

本目录保存 Legacy 与 Current Sharded Manager 的正式 A/B 证据。本轮没有修改 Manager 核心并发架构。

## 1. Tested Commits

| Architecture | Commit | Meaning |
| --- | --- | --- |
| Legacy | `5c29c4a452a095b77c5e46f5b9d25c7629e6461c` | global `processing_mtx_` + synchronous `WriteToMysql` |
| Current Sharded | `dcdf9791c1893eaecc0b4b993abbe7a248c4a8fb` | shard queues/workers + single persistence worker |

Git history confirms `5c29c4a` is the parent of `c608138` (`feat(manager): shard host processing and serialize persistence`). It is the verified Legacy baseline, not a guessed SHA.

## 2. Environment and Controls

| Item | Value |
| --- | --- |
| Host | Windows 11, AMD Ryzen 7 5800H, 8 cores / 16 logical processors, 13.86 GiB |
| Docker | Docker Desktop Engine 29.6.2, Compose v5.3.1 |
| MySQL | `mysql:8.0`, database `monitor_db` |
| Manager image | `monitor-system-benchmark:local` |
| Arrival models | default整秒突发；`--stagger-start` 首轮错峰 |
| Interval | 1000 ms |
| Current default shards | `min(max(2, hardware_concurrency / 2), 8)` = 8 on this host |
| Current shard queue | capacity 256, max 64 MiB |
| Current persistence queue | capacity 1024, max 128 MiB |

Both architectures used the same Docker/MySQL image, payload shape, interval, duration, host count and independent `run-id`. MySQL readiness was checked with `SELECT 1` before each Manager start.

## 3. Reproduction Commands

Current Sharded, from repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\benchmark\run-manager-concurrency-ab.ps1 `
  -Architecture Sharded -RepositoryPath (Get-Location).Path `
  -EvidencePath .\benchmark\evidence\manager-concurrency-ab-20260818\manual\current `
  -Workers 75 -DurationSeconds 600 -IntervalMs 1000 -StaggerStart `
  -RunId sharded-75host-600s-repro
```

Legacy reproduction must first checkout the verified Legacy commit. Running the current branch's `run-windows-benchmark.ps1` does not reproduce Legacy:

```powershell
git switch --detach 5c29c4a452a095b77c5e46f5b9d25c7629e6461c
powershell -ExecutionPolicy Bypass -File .\benchmark\run-manager-concurrency-ab.ps1 `
  -Architecture Legacy -RepositoryPath (Get-Location).Path `
  -EvidencePath .\benchmark\evidence\manager-concurrency-ab-20260818\manual\legacy `
  -Workers 75 -DurationSeconds 600 -IntervalMs 1000 -StaggerStart `
  -RunId legacy-75host-600s-repro
git switch codex/kernscope-manager-concurrency
```

`summary.txt`, `result.csv`, `resource.csv`, `manager-stats.txt` and `loadgen.log` are kept per run. `aggregate-manager-concurrency-ab.ps1` creates the aggregate tables; `sha256-manifest.txt` records SHA-256 for every raw CSV/resource/stats/summary file, including failures.

## 4. Staggered A/B Results: 60 s

Values are medians of valid PASS repetitions. The 75-host 600-second run is reported separately. Current accepted latency is T0; Legacy latency is synchronous RPC-to-MySQL and is not a T0/T1/T2 comparison.

| Hosts | Legacy pass/fail | Current pass/fail | Legacy total s | Current total s | Reduction | Throughput ratio | Current accepted P95/P99 us | Queue mean/max us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1/0 | 1/0 | 61.240 | 61.296 | -0.09% | 0.999x | 1,079 / 16,510 | 79.07 / 109 |
| 10 | 3/0 | 2/1 | 62.280 | 62.284 | -0.01% | 1.000x | 1,035.5 / 2,010 | 74.455 / 188 |
| 25 | 1/0 | 1/0 | 62.493 | 62.301 | 0.31% | 1.003x | 967 / 1,751 | 76.15 / 298 |
| 50 | 3/0 | 3/0 | 62.419 | 62.437 | -0.03% | 1.000x | 938 / 1,699 | 71.63 / 298 |
| 75 | 3/0 | 3/0 | 63.418 | 62.488 | 1.47% | 1.015x | 926 / 1,727 | 68.34 / 745 |
| 100 | 3/0 | 3/0 | 78.698 | 69.490 | 11.70% | 1.133x | 915 / 1,604 | 69.96 / 876 |

The Current 10-host stagger `rep1` failure is retained: 600 RPCs were accepted, 580 rows persisted, `queue_full=0`, `persistence_rejected=0`; Manager had started before MySQL accepted its connection. It is classified as startup/environment failure, not capacity evidence. The runner now waits for MySQL readiness.

## 5. Staggered 75-host Long Stability: 600 s

| Architecture | Samples / accepted / persisted | queue_full / persistence_rejected | Total s | Persisted/s | P50 / P95 / P99 us |
| --- | --- | --- | ---: | ---: | --- |
| Legacy | 45,000 / 45,000 / 45,000 | N/A / N/A | 604.192 | 74.480 | synchronous 12,310 / 374,248 / 800,992 |
| Current | 45,000 / 45,000 / 45,000 | 0 / 0 | 604.399 | 74.454 | accepted 687 / 1,079 / 1,403 |

Current queue delay mean/max was 74.07 / 3,646 us; shard peak depth/bytes 1 / 472; persistence peak depth/bytes 80 / 60,744. Manager CPU average/peak was 7.75% / 10.56%, peak RSS 20.30 MiB; MySQL CPU average/peak was 24.78% / 33.35%, peak RSS 461.20 MiB.

## 6. Burst A/B Results: 60 s

| Hosts | Legacy pass/fail | Current pass/fail | Legacy total s | Current total s | Reduction | Current accepted P95/P99 us | Queue mean/max us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 3/0 | 3/0 | 61.438 | 61.332 | 0.17% | 2,165 / 8,450 | 72.56 / 294 |
| 25 | 1/0 | 1/0 | 61.588 | 61.443 | 0.24% | 2,873 / 9,002 | 71.41 / 613 |
| 50 | 3/0 | 3/0 | 61.911 | 63.035 | -1.82% | 4,388 / 19,350 | 77.16 / 903 |
| 75 | 3/0 | 3/0 | 62.217 | 62.931 | -1.15% | 7,050 / 33,149 | 85.63 / 2,107 |
| 100 | 3/0 | 3/0 | 70.305 | 71.075 | -1.10% | 8,727 / 24,234 | 83.99 / 1,322 |
| 150 | 1/0 | 0/1 | 105.997 | N/A | N/A | N/A | N/A |
| 200 | 1/0 | 0/1 | 133.910 | N/A | N/A | N/A | N/A |

Legacy persisted all tested burst rows through 200 hosts. Current 150-host burst accepted/persisted 8,098/9,000 and recorded `queue_full=902`; Current 200-host burst accepted/persisted 8,466/12,000 and recorded `queue_full=3,534`. Both had `persistence_rejected=0`. Failed raw samples remain under their per-run directories.

## 7. Capacity Boundary

Under this workload and PASS rule, Current Sharded completed every tested staggered run through 100 hosts and every repeated burst run through 100 hosts. The 150-host and 200-host burst runs crossed the shard queue capacity boundary. This is a measured boundary, not a universal production capacity claim. Legacy completed the tested 200-host burst but its synchronous P99 reached 2.329 s at 150 hosts and 2.839 s at 200 hosts.

## 8. Comparable Metrics and Interpretation

Comparable measures are exact persisted rows, total completion time after Manager readiness, persisted reports/s, pass/fail boundary, and same-environment resource samples. The clearest current result is the 100-host staggered median: 69.490 s versus Legacy 78.698 s, 11.70% lower total completion and 1.133x persisted throughput.

There is no universal speedup: at 50/75/100 hosts in burst mode, Current total completion was 1.82%/1.15%/1.10% slower because the single DB Writer drained persistence work after load generation. Current accepted P99 is reported separately and is not a replacement for Legacy synchronous P99.

## 9. Resume-safe Claims

- Current 75-host, 600-second stagger completed 45,000 accepted, processed, persistence tasks and persisted rows; `queue_full=0`, `persistence_rejected=0`; accepted P99 1.403 ms.
- Current 100-host stagger passed 3/3 repetitions; median total completion reduction was 11.70% under the same 60-second workload.
- Current burst passed 3/3 at 100 hosts; 150 hosts recorded 902 queue-full rejections and 200 hosts recorded 3,534.
- Do not claim an “X-fold latency reduction”: T0 accepted, Legacy synchronous RPC, T1 processed and T2 persisted are different stages, and T1/T2 lack per-request correlation IDs.

## 10. Raw Evidence and Integrity

- Aggregate tables: `stagger-ab-summary.csv`, `burst-ab-summary.csv`, `long-stability-summary.csv`
- Failure list: `failures.csv`
- Hashes: `sha256-manifest.txt`
- Raw evidence: separate `stagger/` and `burst/` trees, each with `Legacy` and `Sharded` subtrees

Representative Current 75-host long-stability `result.csv` SHA-256 is `E7CA4E73AAAFD72408F5BA173289D9A347032AC97BD569D94FED614F5730B9EF`. The manifest is authoritative for every run; failed CSVs were not deleted or excluded.

## 11. Not Verified

- Per-request T1 processed and T2 persisted P50/P95/P99: the current output has no correlation ID/timestamp pair.
- Physical-machine or production-cluster capacity: these are Windows Docker tests using synthetic MonitorInfo payloads, not real Worker collection.
- WAL, retry/repair, exactly-once or at-least-once semantics: outside this benchmark scope.

## 12. Remaining Bottleneck

The measured remaining bottleneck is the single DB Writer/MySQL drain under concentrated arrivals. Current 100-host burst passed but peaked at persistence depth 839 and required about 10 s of drain; 150/200-host bursts filled the shard queue and produced `queue_full`. No DB pool, WAL, Kafka/Redis, dynamic shard resizing or other architecture change was made for this evidence run.
