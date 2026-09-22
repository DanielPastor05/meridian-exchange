# v1.0.0 release acceptance record

This historical acceptance record does not certify the audit remediation working tree. See AUDIT-FOLLOWUP.md for the full remaining scope and current verification.

The six requested implementation stages are complete. Every acceptance below points to executable evidence; publication of the versioned assets is the final release operation.

- [x] Bounded binary TCP gateway. tests/network.mjs exercises account authentication, concurrent clients, fragmentation, malformed/oversized frames, slow-reader eviction, the configured connection cap and incomplete-frame deadlines.
- [x] Persistent retry safety. tests/service_tests.cpp and tests/crash.mjs check exact last-outcome replay, payload conflicts, stale/gapped sequences, poisoned sessions and restart without duplicate trade settlement.
- [x] Representative measurements. bench/profile.cpp covers 1k/10k/100k books, four operation distributions, counted allocations and randomized ID lookup. tools/load.mjs schedules independent demand and reports shedding, burst overload, generator lateness, socket/durable timing and restart. All eight gzip CSVs were independently reconciled with summary.json.
- [x] Account risk. 10,000 generated accounting requests plus explicit limit/overflow/ownership/partial-fill/reservation/self-trade/kill checks in tests/service_tests.cpp.
- [x] Failure verification. External process kills at seven boundaries; corruption at every byte of a 152-byte fixture; every 1..59-byte torn tail; reference matching across 80,000 commands. Hosted Windows/MSVC, Linux/GCC, Clang ASan/UBSan and seeded libFuzzer have passed: [first all-green run](https://github.com/DanielPastor05/meridian-exchange/actions/runs/35776351168).
- [x] Public reproducibility. [Repository](https://github.com/DanielPastor05/meridian-exchange), complete protocol/design/performance docs, executable assertion-based demo, CI-built archives with source identity, and checksummed source/binary assets through the [release page](https://github.com/DanielPastor05/meridian-exchange/releases). Verify the final version's workflow and assets there.
- [x] Sequenced market data. tests/network.mjs and service_tests.cpp check ring expiry, atomic top-of-book snapshot recovery and continued sequencing; book pages reject a changed version.
- [x] Full-capacity crossing admission. Explicit partial-maker and remainder cases plus the independent reference. Allocation/lookup measurements precede any proposed container redesign.

The project is complete within its documented single-node simulator scope. Group commit, checkpoints/rotation, replication and alternative data structures are conditional extensions, with measurements and decisions recorded in ROADMAP.md. This does not claim a production trading venue, physical power-loss certification or a percentile among job candidates.
