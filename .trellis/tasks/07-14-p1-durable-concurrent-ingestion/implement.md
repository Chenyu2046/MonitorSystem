# Implementation and verification

1. Make the callback and persistence methods return `bool`; map failure to
   non-OK gRPC status before updating accepted host state.
2. Synchronize ingest state and make five-table persistence transactional with
   explicit checks and rollback.
3. Use focused source checks for return propagation, transaction coverage, and
   synchronization; run available build checks.
4. Independent CR, remediate blockers, commit, and push.
