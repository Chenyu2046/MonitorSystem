# P1 durable concurrent ingestion

## Goal

Make concurrent Push ingestion race-free and prevent failed MySQL persistence
from being acknowledged as a successful gRPC push.

## Requirements

- All mutable sampling maps and MySQL write operations must have a single
  synchronized owner during an ingest.
- `HostManager` must return persistence success/failure to `GrpcServerImpl`.
- Every INSERT failure must be observable and leave the RPC non-OK.
- Preserve the protobuf contract and avoid a speculative queue/pool rewrite.

## Acceptance Criteria

- [ ] Concurrent calls cannot mutate sample maps or write SQL concurrently.
- [ ] MySQL init/connect/BEGIN/INSERT/COMMIT failure returns a non-OK RPC.
- [ ] Successful data is committed atomically across all relevant tables.
- [ ] Independent CR verifies callback ordering and failure semantics.

## Notes

- CR loop: the initial implementation was rejected for global sample ownership
  and per-request database connections. The accepted revision owns state and a
  reusable MySQL connection in `HostManager`, uses current-host rollback only,
  and distinguishes failed from unknown COMMIT outcomes.
- Verified by static contract checks and independent CR. Linux CMake/MySQL
  compilation and failure-injection integration remain required because this
  Windows workspace lacks the target toolchain.
