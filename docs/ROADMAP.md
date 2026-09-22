# Scope decisions

The requested release covers six stages: network sessions; persistent retry safety; representative measurements; funded account risk; fault/corruption verification; and public reproducibility with hosted CI and runnable artifacts. The executable evidence is tracked in COMPLETION.md.

## Decisions from the measurements

- Keep ordered maps and the hash ID index for this release. The 100k-order core run costs roughly 176 ns per operation on this desktop, with 0.6 allocations per command and approximately 114 ns per successful randomized ID lookup. Those are workload-specific observations, not proof that allocation or cache misses dominate all traffic. No hardware cache/branch counter claim is made.
- Keep one serialized writer. TCP loopback with buffered journaling sustained the offered 10k requests/s in the recorded two-second sample. Lock-free queues, CPU pinning and a poll-mode network stack have not been justified by an isolated transport profile. The Node generator and desktop scheduler are material parts of end-to-end latency.
- Per-request storage sync dominates durable throughput on the measured machine: about 200–247 completed requests/s under overload. A future group-commit mode should batch log writes, synchronize once, and acknowledge only the synchronized prefix. It must repeat the fault matrix and publish batch-size versus tail-latency results. Buffered mode is not a substitute for this guarantee.
- Restart through the measured 20k-record, 1.2 MB journal took about 76 ms including process startup. Checkpoint/rotation is deferred until a measured recovery-time/disk budget justifies it. This observation does not predict million-record recovery. Long-running deployments would require that budget and backup policy.
- Replication and consensus are outside the single-node simulator's contract. Add them only with an explicit availability objective and failure model; local fsync is not replication.

These are documented extensions, not hidden claims of production readiness. Market orders, amendments, multi-instrument routing, real-money connectivity and a graphical dashboard were not prerequisites for this release.
