# Shared Production Guide

Before implementing a change, identify whether it affects `worker`,
`manager`, or `proto`; load every affected package spec. Keep edits narrow,
preserve user changes, and verify with the real target runtime where Linux,
eBPF, MySQL, or gRPC behavior is involved.

For production changes, record the trust boundary, rollback path, verification
command, and any unverified runtime assumption in the task artifacts.
