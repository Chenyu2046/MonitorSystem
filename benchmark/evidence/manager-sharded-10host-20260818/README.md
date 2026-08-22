# Current Sharded Manager Smoke Evidence

This evidence belongs to the Current Sharded Manager baseline and was generated
from commit `7ff8d142e5699e68c51f8402adeb17cd00414d94`.

## Run metadata

| Item | Value |
| --- | --- |
| commit | `7ff8d142e5699e68c51f8402adeb17cd00414d94` |
| branch | `codex/kernscope-manager-concurrency` |
| workers | 10 Hosts |
| duration | 30 seconds |
| interval | 1000 ms per Host |
| arrival model | default burst: all Hosts use the default整秒到达模型; no `--stagger-start` |
| MySQL | `mysql:8.0`, linux/amd64, image ID `7dcddc01f13b` |
| Docker Engine | 29.6.2 |
| Docker Compose | v5.3.1 |
| benchmark image | `monitor-system-benchmark:local`, digest `sha256:f98a71a2375c9e53df0ca2bd575d14c159c0f4cfc6abbbb3b862a8592ae0b543` |

## Reproduction command

Run from the repository root in PowerShell:

```powershell
pwsh -NoProfile -File .\benchmark\run-windows-benchmark.ps1 `
  -Workers 10 -DurationSeconds 30 -IntervalMs 1000 -DrainTimeoutSeconds 60
```

The script rebuilds the MySQL-enabled benchmark image, starts MySQL and
Manager, runs the load generator, waits for exact persistence by run ID, then
stops Manager and validates the graceful-shutdown counters.

## Results

| Metric | Result |
| --- | ---: |
| accepted | 300 |
| processed | 300 |
| persistence_tasks | 300 |
| persistence_rejected | 0 |
| persisted rows | 300 |
| queue_full | 0 |
| accepted P50 / P95 / P99 | 1451 / 2171 / 7096 us |
| queue delay mean / max | 80.14 / 395 us |
| shard queue peak depth / bytes | 2 / 852 |
| persistence queue peak depth / bytes | 9 / 6419 |

Acceptance checks passed:

```text
accepted == processed == persistence_tasks == persisted == 300
persistence_rejected == 0
queue_full == 0
```

## Evidence files

- `result.csv`: the complete 300-row load-generator result, including all
  successful samples.
- `manager-stats.txt`: the exact run ID, persistence count, shutdown counters,
  queue statistics, and accepted latency percentiles.

`result.csv` SHA256:

```text
B92BAED901A15B977053CC2E609DE37729D758E71EC0A9A8D4480A3EC902683E
```
