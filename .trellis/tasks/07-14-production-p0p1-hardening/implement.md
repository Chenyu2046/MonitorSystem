# Execution plan

| Order | Child task | Scope | Required proof |
| --- | --- | --- | --- |
| 1 | P0 secure transport and secrets | manager startup, worker channel, runbook | fail-closed config paths, focused source checks, CR |
| 2 | P0 SQL injection removal | manager query/host persistence | all RPC-derived SQL strings audited, focused check, CR |
| 3 | P1 concurrent durable ingestion | manager RPC/host manager | synchronized state and non-OK persistence result, CR |
| 4 | P1 bounded queries and host state | manager RPC/query manager | limits/rejection checks, CR |
| 5 | P1 network-unit and interface correctness | manager/worker metric flow | kB/s agreement and multi-interface behavior, CR |

At every row: inspect applicable Trellis specs, implement the smallest
root-cause fix, run the planned verification, request a read-only independent
subagent review, fix every blocking finding, re-run verification, commit, and
push. Update the parent plan if the actual scope or risk changes.
