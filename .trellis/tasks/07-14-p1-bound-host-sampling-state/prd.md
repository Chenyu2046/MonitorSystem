# P1 bound host sampling state

## Goal

Bound every HostManager per-host sampling cache, not only the gRPC response
cache, so authenticated churn cannot exhaust Manager memory.

## Acceptance Criteria

- [x] A new host beyond 256 tracked hosts receives `RESOURCE_EXHAUSTED`.
- [x] A host inactive for over 60 seconds loses every HostManager sampling map entry.
- [x] Background cleanup and ingest do not race on sampling state.
- [x] Static verification and independent CR pass.
