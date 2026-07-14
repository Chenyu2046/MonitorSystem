# P1 worker runtime boundaries

## Goal

Avoid invalid CLI input crashing the Worker, make SIGINT/SIGTERM shut it down
through `MonitorPusher::Stop`, and remove per-sample full-metric console dumps
from both Worker and Manager.

## Acceptance Criteria

- [x] Invalid, non-positive, or partially numeric interval input exits with usage.
- [x] SIGINT and SIGTERM cause the main loop to call `pusher.Stop()`.
- [x] Normal samples do not print every CPU, network, disk, memory, and IRQ value.
- [x] Static verification and independent CR pass.

## Non-goals

No new logging dependency or retry/WAL mechanism is introduced.
