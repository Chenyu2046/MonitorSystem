# Implementation and verification

1. Add connection-aware escaping helpers in `HostManager` and `QueryManager`.
2. Apply them to all telemetry and request-derived SQL string values.
3. Test escaped special characters with a focused source/executable check and
   run `git diff --check`.
4. Request independent read-only security CR; resolve blockers, recheck,
   commit, and push.
