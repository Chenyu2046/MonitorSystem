# Design

Use one `HostManager` ingest mutex as the smallest correct owner for the
existing static sample maps and per-ingest MySQL connection. It serializes the
complete calculate/write/update sequence; a queue or connection pool is out of
scope until measured throughput requires one.

Change the internal callback from `void` to `bool`. The gRPC handler invokes it
before recording the in-memory latest value and returns `INTERNAL` when the
transaction fails. Use `START TRANSACTION`, check every MySQL command, then
`COMMIT`; failure rolls back and does not advance sample state.
