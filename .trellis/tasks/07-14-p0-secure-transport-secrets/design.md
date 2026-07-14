# Design

Use one small header-only runtime configuration helper so manager and worker
use identical environment/file validation without a new target or dependency.

- Manager reads `MONITOR_DB_HOST`, `MONITOR_DB_USER`, `MONITOR_DB_PASSWORD`,
  and `MONITOR_DB_NAME` before constructing either MySQL user.
- Manager TLS reads `MONITOR_GRPC_TLS_CA_FILE`,
  `MONITOR_GRPC_TLS_CERT_FILE`, and `MONITOR_GRPC_TLS_KEY_FILE`; it requires
  and verifies client certificates.
- Worker reads the same three TLS-file variables. Its certificate must be a
  client certificate trusted by the manager CA policy.
- The only compatibility bypass is an exact environment value
  `MONITOR_ALLOW_INSECURE_GRPC=1`; it is visibly logged and documented as
  local-only.

No certificate generation, secret storage backend, or protocol change is in
scope. These changes make those external deployment responsibilities required
instead of silently weakening production security.
