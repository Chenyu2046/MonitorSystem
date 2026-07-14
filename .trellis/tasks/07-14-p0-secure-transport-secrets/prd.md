# P0 secure transport and secrets

## Goal

Remove the default plaintext gRPC and committed database credentials from the
manager/worker production path.

## Requirements

- Require manager certificate, private key, and trusted client CA for gRPC.
- Require worker certificate, private key, and trusted manager CA for gRPC.
- Require database host, user, password, and database name from environment.
- Permit insecure gRPC only when `MONITOR_ALLOW_INSECURE_GRPC=1` is explicitly
  set for local development.
- Keep the protobuf contract unchanged and document deployment variables.

## Acceptance Criteria

- [ ] Source contains no default `monitor123` or `Insecure*Credentials` path
  reachable without the explicit escape hatch.
- [ ] Missing required settings fail startup with an actionable error.
- [ ] mTLS server configuration requires and verifies a client certificate.
- [ ] Worker uses client TLS credentials by default.
- [ ] A focused test/check covers required environment and file loading logic.

## Notes

- Mid-task review: the original plan remained bounded to startup configuration,
  TLS identity, and documentation. The first CR found missing RPC authorization
  and a null-stub path; both were fixed before the second CR approved the diff.
- Verification: `git diff --check` and executable source assertions for
  fail-closed configuration, authorization coverage, and host/SAN binding pass.
  Real Linux CMake build and mTLS handshake tests remain required because this
  Windows workspace has no CMake or target gRPC/MySQL dependencies.
