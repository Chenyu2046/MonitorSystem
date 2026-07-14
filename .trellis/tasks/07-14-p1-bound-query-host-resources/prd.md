# P1 bound query and host resources

## Goal

Bound production query, RPC, and in-memory host-resource consumption without
changing the published query semantics.

## Requirements

- Reject time ranges over 31 days and avoid unaggregated trend scans.
- Clamp every paginated query to page <= 10,000 and page_size <= 1,000 in both
  the RPC boundary and data layer.
- Cap worker ingestion payloads and the manager host cache.
- Keep `QueryLatestScore` response records bounded while calculating cluster
  statistics across all servers.

## Acceptance Criteria

- [x] All seven paginated endpoints normalize and echo effective pagination.
- [x] Trend responses echo the enforced minimum 300-second interval.
- [x] Score records are limited to 1,000; cluster aggregate values remain full-set.
- [x] Host payload/cache bounds are enforced before cache insertion.
- [ ] Linux MySQL/gRPC build and integration test pass in a compatible runtime.

## Notes

- The local Windows environment lacks the Linux MySQL/gRPC build chain; static
  checks and independent code review are the available local gate.
