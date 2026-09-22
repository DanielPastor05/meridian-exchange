# Audit remediation ledger

Scope: every actionable finding in the 2026-09-22 technical audit. A checked item
requires implemented behavior and verification; this document is not acceptance evidence by itself.

- [x] H1: typed integer validation in the Node client and boundary regressions.
- [x] H2: exact response framing, schemas, correlation and malformed-peer tests.
- [ ] H3: constant-time level aggregates, independent checks and service-depth measurements.
- [x] H4: bounded reusable book snapshots, concurrent pagination and cost tests.
- [ ] H5: bounded admission, writer ownership, independent metrics and durable batching evaluation.
- [ ] H6: recoverable accept errors, resource backoff and injected error tests.
- [x] H7: failed replay poisons the journal; subsequent writes preserve the file.
- [ ] H8: signal-safe thread communication and shutdown/concurrency verification.
- [x] Remove redundant quotes and linear scanning of retained feed events.
- [ ] Expand state equivalence diagnostics to ownership, retry records and retained events.
- [x] Bound CLI input before allocation/tokenization; centralize executable version.
- [ ] Safe deployment: protected transport, private files and non-fixture credentials.
- [ ] Fair resource quotas for account/session/origin and operational authorization.
- [ ] Recovery of older outcomes and private executions; explicit concurrent-session contract.
- [ ] Bounded storage/recovery: verified checkpoints, rotation, backup and restore.
- [ ] Read-only account journal inspection and safe configuration evolution.
- [ ] Inject storage errors, partial writes and allocation failures in meaningful boundaries.
- [ ] Independent risk/admission oracle and strengthened crash/parser assertions.
- [ ] Concurrent same-account/reconnect tests, incremental framing, TSan and broader fuzzing.
- [x] Decouple production tools from test fixtures; handle process spawn failures.
- [ ] Optional tests/tools in CMake, private warnings, reproducible CI/build metadata.
- [x] Load measurements report business outcomes and a controlled accepted-operation mix.
- [ ] Representative long/repeated benchmarks, queue/storage timings and depth/subscriber cases.
- [ ] Operational runbook, design/protocol updates and author-facing change studies.
- [ ] Final cross-platform tests and requirement-by-requirement completion audit.

No new ML system, graphical UI or microservice split is required: those were not defects.
The owner selected MIT explicitly; LICENSE and release packaging now include it.

## Verification checkpoint (Windows, current working tree)

- 13/13 CTest groups passed (43.13 s); earlier 12-group build plus live demo passed.
- Actual client module: six Node regression groups, including every split position in a response.
- Aggregates: 80,000 differential commands now compare quotes with an independent vector reference;
  full-width overflow/recovery is exercised. Deep service benchmarking is still pending.
- Snapshots: 1,000 one-order pages copy the full book only once; mutation, expiry, eviction and byte budget are checked.
- Storage: injected short writes, ENOSPC, flush and sync errors poison the live service;
  replay/retry settles each trade once. bad_alloc is injected after matching, during settlement and before result caching.
- Same-account TCP clients: identical requests execute once; conflicting payloads have one winner;
  reconnect and process restart recover the exact last outcome.
- Four one-second load smoke scenarios: zero business rejections and transport failures, including shedding.
- WSL is not installed on this host. Linux, ASan/UBSan, TSan and hosted checks are still pending.

Next architectural block: isolate the single writer behind bounded admission, publish independent operational metrics,
and preserve durable acknowledgement semantics while evaluating group commit. Then add protected transport,
resource fairness, recoverable outcome/execution history, checkpoints/rotation and offline inspection/migration.
