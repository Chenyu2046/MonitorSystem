# P0 eliminate SQL injection

## Goal

Prevent RPC-derived telemetry names and query server names from changing MySQL
SQL syntax.

## Requirements

- Escape every string inserted into host-manager write SQL with the live MySQL
  connection charset.
- Escape `server_name` in every QueryManager read path before it is appended to
  a query.
- Do not add a new database dependency or change the schema/protobuf contract.

## Acceptance Criteria

- [ ] Host, network, disk, and CPU identifiers cannot terminate SQL string
  literals.
- [ ] All QueryManager `server_name` SQL sites use one connection-aware helper.
- [ ] A focused executable check covers quotes, backslashes, NUL, and control
  characters in the escaping boundary.
- [ ] Independent CR finds no RPC-controlled raw string interpolation into SQL.

## Notes

- CR approved the connection-aware escaping boundary. `git diff --check` and
  static coverage assertions pass. A real MySQL integration test with quotes,
  backslashes, NUL, and UTF-8 input remains required on Linux because this
  workspace has neither CMake nor the target MySQL runtime.
