# Production P0/P1 hardening

## Goal

Make the confirmed P0 and P1 defects safe for a production deployment without
changing the protobuf wire contract. Each defect is a separately reviewable
child task: implementation, targeted verification, independent CR, then
commit and push to `codex/production-hardening`.

## Requirements

- P0 transport and credentials: the manager and worker must not default to
  plaintext gRPC or embedded database credentials. Production startup must fail
  closed when required TLS or database settings are absent. A clearly named
  local-development escape hatch may exist, but cannot be the default.
- P0 SQL safety: RPC-derived names and query inputs must not form executable SQL
  through string concatenation.
- P1 ingestion integrity: concurrent RPC ingestion must not race on sampling
  state; a database write failure must be returned to the caller rather than
  acknowledged as success.
- P1 bounds: query page size/time range and in-memory host cardinality must be
  bounded; malformed and oversized identifiers must be rejected before storage.
- P1 metric correctness: manager-side network handling must consistently use
  the protobuf `kB/s` unit and handle every reported interface deliberately.
- Update the production runbook with the exact required environment variables,
  migration/rollback notes, and tests that can be run on Linux.

## Acceptance Criteria

- [ ] No production default uses insecure gRPC credentials or source-embedded
  database secrets.
- [ ] Static security checks show that RPC-controlled strings are escaped or
  bound before reaching MySQL SQL text.
- [ ] Failed persistence is observable by the worker through a non-OK gRPC
  result, and concurrent ingestion state is synchronized.
- [ ] Query and host-memory resource limits are enforced and covered by a
  focused executable check where the local toolchain permits it.
- [ ] Network rate storage, scoring, and logs agree on `kB/s` and do not
  accidentally discard non-primary interfaces.
- [ ] Every child task has a CR result with no unresolved blocking finding and
  is committed and pushed.

## Constraints and Decisions

- The user explicitly authorized an autonomous loop: no human review gate is
  required between child tasks; the mandatory gate is independent subagent CR.
- This task does not claim certificate issuance, secret distribution, database
  migration execution, or a Linux deployment. It makes those deployment inputs
  explicit and fails closed when they are missing.
- Keep the existing proto fields and MySQL schema compatible. Prefer existing
  C++/gRPC/MySQL APIs and narrow changes over new dependencies or a new service.
- P0 transport/secrets mid-task review: initial mTLS-only change was expanded
  within the same trust-boundary scope to add SAN authorization after CR found
  that any CA-trusted client could otherwise access either service. The child
  remains P0 and the parent stays Level 3.
