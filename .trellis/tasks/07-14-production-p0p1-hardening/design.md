# Production P0/P1 hardening design

## Boundary

The manager is the trust boundary: every incoming gRPC payload is untrusted
until TLS identity checks, size/name validation, and database-safe conversion
have completed. The worker is a client and must refuse an unauthenticated
manager by default.

## Sequencing

1. **P0 transport and secrets**: environment-supplied database settings and
   mutually authenticated TLS; default to fail-closed.
2. **P0 SQL safety**: centralize string escaping/binding at the MySQL boundary
   and remove every RPC-derived raw value from SQL text.
3. **P1 persistence integrity**: synchronize sampling state, report a failed
   write to gRPC, and keep the smallest safe connection lifecycle.
4. **P1 resource and correctness limits**: cap host/query resources and repair
   network unit and multi-interface handling.

Each step is independently reversible as one commit. It must not rely on an
unreviewed later step for its safety property.

## Security and compatibility

- TLS trust roots, certs, keys, database host/user/password/name are injected
  by environment variables; no secret is committed.
- `MONITOR_ALLOW_INSECURE_GRPC=1` is allowed only as an explicit local
  development compatibility escape hatch and is documented as forbidden in a
  production deployment.
- Existing protobuf messages and database table columns remain unchanged.
- SQL safety uses the MySQL client library already linked by the project;
  prepared statements are preferred where a query needs typed parameters,
  otherwise the minimal centralized `mysql_real_escape_string` boundary is
  acceptable for string fields.

## Validation and rollback

- Use static source checks plus the strongest available CMake/test build. The
  present Windows workspace lacks a native Linux build environment, so any
  unavailable runtime validation is recorded precisely in the child task.
- A rejected CR finding is fixed and re-reviewed before the child commit.
- Roll back one child by reverting its single commit and restore its documented
  environment behavior; no schema migration is introduced by this plan.
