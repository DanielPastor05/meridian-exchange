# Completion checklist

Complete all six agreed stages, with evidence from the final revision.

- [ ] Bounded binary TCP gateway: authenticated sessions, independent clients, fragmented frames, slow consumers, bounded resources.
- [ ] Persistent retry safety: request sequencing, cached outcomes, conflict/stale/gap handling, reconnect and crash recovery without duplicate execution.
- [ ] Representative benchmarks: large books and mixed traffic, per-operation distributions, open-loop offered load and burst/overload results, core versus durable end-to-end timing, raw data and environment.
- [ ] Account risk: ownership, order-size/notional/position limits, cash and inventory reservations, partial-fill accounting, self-trade prevention and kill switch.
- [ ] Failure verification: kill at append/sync/apply/response boundaries, acknowledged-history verification, corruption/truncation, Linux/GCC, Windows/MSVC, Clang sanitizers, bounded parser fuzzing.
- [ ] Published repository: reviewed commit history, successful hosted CI, versioned runnable demo, build/run/protocol/design/performance documentation and release artifacts.
- [ ] Market-data events with sequence numbers and gap recovery via snapshot.
- [ ] Full-book crossing admission fixed and differential tests updated; allocation/lookup costs measured before optimization.

Conditional scale features from the roadmap (checkpoint/rotation, replication, lock-free structures) require measured need. Record the measurement and decision.

Each checked item must identify an executable test, measurement, hosted run or inspected artifact. A configured workflow is not a passing run. Baseline claims do not establish completion of new work.
