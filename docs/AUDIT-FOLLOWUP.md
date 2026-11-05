# Audit remediation ledger

Scope: every actionable finding in the 2026-09-22 technical audit. A checked item
requires implemented behavior and verification; this document is not acceptance evidence by itself.

- [x] H1: typed integer validation in the Node client and boundary regressions.
- [x] H2: exact response framing, schemas, correlation and malformed-peer tests.
- [x] H3: constant-time level aggregates, independent checks and service-depth measurements.
- [x] H4: bounded reusable book snapshots, concurrent pagination and cost tests.
- [x] H5: admission is bounded (one frame in flight per connection, capped connections), the state lock owns the writer, Ping and Metrics no longer take it, and group commit was evaluated and deferred on measurements. See `tests/under_failure.mjs`, DESIGN.md and ROADMAP.md.
- [x] H6: recoverable accept errors, resource backoff, classification tests, and a real descriptor limit with more connections than it allows.
- [x] H7: failed replay poisons the journal; subsequent writes preserve the file.
- [x] H8: signal-safe flag, SIGTERM with a client logged in and another mid-frame, and the TSan job.
- [x] Remove redundant quotes and linear scanning of retained feed events.
- [x] State equivalence covers ownership, retry records and retained events: `state_hash` from the live server, from `exchange inspect` and from a restart must match.
- [x] Bound CLI input before allocation/tokenization; centralize executable version.
- [x] Safe deployment: owner-only journal, loopback unless `--allow-remote`, fixture-shaped tokens refused on a reachable address. TLS itself is listed below as not built.
- [x] Quotas per account and per source address, given back on close; Metrics can be restricted to one account.
- [x] `exchange inspect` recovers older outcomes and an account's executions from the journal; the concurrent-session contract is in PROTOCOL.md.
- [x] Backup and restore, verified by `exchange inspect` and the `state_hash` on restart (OPERATIONS.md). Checkpoints and rotation are listed below as not built.
- [x] `exchange inspect` is read-only beside a live writer. Tokens rotate freely; changed balances or limits are refused, and both behaviours are tested.
- [x] Storage errors, short writes and allocation failures are injected at meaningful boundaries: `tests/storage_tests.cpp`.
- [x] Independent risk/admission oracle and strengthened crash/parser assertions.
- [x] Same-account and reconnect tests (`tests/concurrency.mjs`), every split position of a response (`tests/client.mjs`), the TSan job, and seeded libFuzzer in CI.
- [x] Decouple production tools from test fixtures; handle process spawn failures.
- [x] `BUILD_TESTING` and `MERIDIAN_BUILD_BENCHMARKS` are optional, warnings are PRIVATE, actions are pinned by commit, and `tools/build-info.mjs` records build metadata.
- [x] Load measurements report business outcomes and a controlled accepted-operation mix.
- [ ] Representative long/repeated benchmarks, queue/storage timings and depth/subscriber cases. **Still open.** Depth has repeated measurements, and the rest needs a quiet dedicated machine: hosted CI numbers would not be representative of anything.
- [x] `docs/OPERATIONS.md`, DESIGN.md and PROTOCOL.md updated; EMPIEZA-AQUI.md explains what changed and why.
- [ ] Final cross-platform tests and requirement-by-requirement completion audit.

## Not built, on purpose

These are real limits, written down rather than implied away. Each is a decision for a single-node simulator and would be the first thing to revisit for a deployed service.

- **TLS.** Terminate it in front. A reachable address has to be asked for with `--allow-remote`.
- **Checkpoints and journal rotation.** Replay of the measured 20,000-record journal took about 76 ms, so nothing measured justifies them yet. The journal grows by 60 bytes per fresh request; monitor the disk.
- **Group commit and a dedicated writer thread.** Synchronous durability costs throughput (200-247 requests/s measured under overload), and group commit is how to buy it back, but no workload here needs it. A thread would add a second place to wait without bounding anything new.
- **Replication and consensus.** Outside the single-node contract, as ROADMAP.md says.

No new ML system, graphical UI or microservice split is required: those were not defects.
The owner selected MIT explicitly; LICENSE and release packaging now include it.

## Verification checkpoint (Windows, current working tree)

- 13/13 CTest groups passed (43.13 s); earlier 12-group build plus live demo passed.
- Actual client module: six Node regression groups, including every split position in a response.
- Aggregates: 80,000 differential commands now compare quotes with an independent vector reference;
  full-width overflow/recovery is exercised. The service-depth profile covers 1k/10k/100k same-price orders,
  three repetitions each; all pages copy exactly N orders. See bench/results/audit-service-depth.
- Snapshots: 1,000 one-order pages copy the full book only once; mutation, expiry, eviction and byte budget are checked.
- Storage: injected short writes, ENOSPC, flush and sync errors poison the live service;
  replay/retry settles each trade once. bad_alloc is injected after matching, during settlement and before result caching.
- Same-account TCP clients: identical requests execute once; conflicting payloads have one winner;
  reconnect and process restart recover the exact last outcome.
- Four one-second load smoke scenarios: zero business rejections and transport failures, including shedding.
- Hosted CI for commit 136c012 passed Windows, Linux, ASan/UBSan and the protocol fuzzer:
  https://github.com/DanielPastor05/meridian-exchange/actions/runs/35783689448
- The independent JavaScript vector/BigInt oracle passed 4,000 generated risk/matching/accounting transitions.
- WSL is not installed on this host. The new TSan job and subsequent changes still need hosted verification.

## Verification checkpoint (v1.1.0 candidate)

- `tests/under_failure.mjs`: a writer stopped before its append leaves Ping and Metrics answering; a reachable address needs
  `--allow-remote` and real tokens; account and origin quotas are enforced and given back; `exchange inspect` rebuilds outcomes
  and both sides of an execution, beside a live writer and without changing the file; the live, inspected and restarted `state_hash`
  agree; changed limits are refused while a rotated token is not. On POSIX also SIGTERM with clients connected and a real
  descriptor limit, which Windows cannot express.
- Local build with Clang 20 under the project's warning flags, Windows: 13/13 test groups.
- Hosted CI is the evidence for MSVC, GCC, ASan/UBSan and TSan: see the pull request that merges this ledger.

One item above remains open, on purpose: representative benchmarks need a quiet dedicated machine.
