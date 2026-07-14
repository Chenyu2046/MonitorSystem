# Protocol Contract Guidelines

## Scope

`proto/` is the shared Worker/Manager contract. Changes generate code for both
executables through `proto/CMakeLists.txt`.

## Rules

- Treat every field-number, type, and unit change as a compatibility change.
  Never reuse a field number; add a new optional field and retain old readers
  during rollout.
- Document units next to numeric fields. `NetInfo.send_rate` and
  `NetInfo.rcv_rate` are `kB/s`; packet rates are `packets/s`.
- Do not introduce secrets, raw command text, or unbounded opaque payloads in
  telemetry messages.
- Contract changes require Worker and Manager build/test coverage plus a mixed
  version compatibility decision before merge.
