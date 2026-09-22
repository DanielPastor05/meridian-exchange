# Portfolio milestones

The current deliverable is a tested core and recovery baseline. Complete one
stage and publish its evidence before increasing scope.

## 1. Explain and measure the baseline

- Reproduce tests and benchmark on your own machine. Record compiler, CPU, build
  flags, power policy and raw repetitions.
- Add larger books and realistic mixes of cancels, partial fills and price moves.
  Distinguish latency by operation type and run a load generator independently.
- Profile allocation, cache misses and branch misses. Change one measured
  bottleneck and retain the before/after data plus negative results.
- Explain FIFO preservation, the full-book admission rule, and why a persisted
  command may lack a response. Implement one behavior change yourself with a
  regression test.

Completion: a concise performance report that can be reproduced from one commit.

## 2. Add a bounded TCP gateway and market-data stream

- Define versioned binary messages with explicit byte order, lengths, request
  sequence and error codes. Include parsing fuzz tests and bounded buffers.
- Preserve a single writer for each book. Measure whether SPSC queues help before
  replacing a simpler transport arrangement.
- Handle fragmented reads, slow clients, disconnects and session retries with an
  explicit idempotency policy. A rejected duplicate must never execute again.
- Sequence market-data events and provide a snapshot/recovery route for consumers
  that miss updates.
- Measure end-to-end latency at specified offered loads, including rejections and
  backlog. Keep this separate from the core microbenchmark and disk-sync latency.

Completion: a live demo with two independent clients, replayable traffic and a
documented overload policy. No connection to real-money venues is required.

## 3. Add account risk and demonstrate failure recovery

- Introduce account/session identity, maximum order size and integer notional
  checks with explicit overflow handling, plus a kill switch.
- Define reservations and released exposure for partial fills and cancellations
  before implementing them; compare against a simple accounting model.
- Automate killing the process at controlled points around append, sync, apply
  and response. Compare restarted state with the acknowledged command history.
- Add checkpoints and rotation only after replay time justifies them. Test the
  boundary between the checkpoint and subsequent journal records.

Completion: a short demo, a failure matrix and a report showing exactly which
acknowledgements and account invariants survive each tested fault.

The strongest final presentation is a five-minute explanation with reproducible
evidence and a code change you can make live. Keep the README short; put long
experiments in separate reports. Further concurrency or replication should follow
a measured need.
