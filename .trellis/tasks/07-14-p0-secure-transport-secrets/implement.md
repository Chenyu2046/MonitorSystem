# Implementation and verification

1. Add the shared configuration helper and wire it into manager/worker startup.
   Verify: focused source-level executable check plus CMake configuration if
   the environment provides dependencies.
2. Pass one environment-sourced database configuration to `QueryManager` and
   `HostManager`; remove both embedded credential copies.
   Verify: search confirms the secret/defaults are gone.
3. Update the Linux runbook with mTLS/database setup and local-only bypass.
4. Ask a read-only subagent to security-review the final diff. Fix blocking
   findings and repeat the targeted verification before commit/push.
