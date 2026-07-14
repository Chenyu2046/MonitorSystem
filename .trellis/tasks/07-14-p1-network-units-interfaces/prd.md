# P1 network units and interfaces

## Goal

Make network rates consistently `kB/s` from Worker collection through Manager
scoring, primary-table persistence, and operational logs, while aggregating all
reported non-loopback interfaces.

## Acceptance Criteria

- [x] The manager does not convert protobuf `kB/s` values a second time.
- [x] Manager totals and scoring sum every `NetInfo` item.
- [x] Logs label the protocol values as `kB/s`.
- [ ] A focused static check and independent CR pass.

## Constraint

Keep the protobuf wire contract and existing MySQL columns unchanged.

## Migration note

Rows written before this change are not comparable with new aggregate rates:
the old manager stored only the first interface after a second division by
1024. Preserve the deployment timestamp as a query boundary, or recompute
historical rates from raw source data before mixing them in a trend.
