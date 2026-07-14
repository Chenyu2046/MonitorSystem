# Manager Production Guidelines

## Scope

`manager/` receives `MonitorInfo`, calculates scores, persists time-series
data to MySQL, and serves query RPCs. Entry point: `manager/src/main.cpp`;
ingest path: `manager/src/rpc/grpc_server.cpp` ->
`manager/src/host_manager.cpp`; query path:
`manager/src/rpc/query_service.cpp` -> `manager/src/query_manager.cpp`.

## Production Rules

- Treat all RPC fields as untrusted. Production endpoints require TLS,
  authenticated Worker identity, authenticated query access, request-size and
  rate limits, and bounded pagination/time ranges.
- Never build SQL by concatenating host names, interface names, or request
  fields. Use prepared statements and parameter binding for reads and writes.
- Credentials must come from deployment configuration or a secret manager, not
  source files. MySQL connection, query, and transaction failures must be
  observable and must not be returned to a Worker as a successful ingest.
- Shared mutable sampling state belongs to a synchronized owner. The gRPC
  callback may run concurrently; global maps and database writes need explicit
  locking/queue ownership.
- Persisting telemetry must be decoupled from the RPC handler with a bounded
  queue, retry/backoff policy, and overload behavior. Define retention and
  backup/recovery before production rollout.

## Verification

Run integration tests against MySQL, concurrent ingest/race checks, failed
database behavior, authorization tests, and query-limit tests. Validate score
and storage units using a known multi-interface fixture.
