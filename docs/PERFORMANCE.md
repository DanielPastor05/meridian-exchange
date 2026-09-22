# Performance report — 22 September 2026

Measurements below are observations on one unpinned Windows desktop, not guaranteed exchange latency. Raw data and environment metadata are in [windows-2026-09-22](../bench/results/windows-2026-09-22). The benchmark tools emit their own inputs, timings and checksums; replay verifies the number of completed submissions.

## Core, separate from transport/storage

Command: exchange_profile 100000 3, Release/MSVC. Input generation and prefill are excluded. One warm-up precedes three raw throughput repetitions. A separate pass timestamps individual operations and checks the same final state. Mixture: 20% cancellation, 40% insertion, 20% partial fill, 20% full fill; at most one transient inside-spread order beyond the prefilled 1k/10k/100k book, up to 2,001 occupied price levels. Cancellations rotate through known live orders, so this is synthetic rather than historical market replay.

| Resting depth | Median ns/command | Instrumented p99 cancel / insert / partial / full | Allocations/command | Successful ID lookup mean |
|---:|---:|---|---:|---:|
| 1,000 | 79.05 | 300 / 300 / 100 / 200 ns | 0.8 | 11.53 ns |
| 10,000 | 74.69 | 400 / 300 / 200 / 200 ns | 0.6 | 20.71 ns |
| 100,000 | 176.12 | 600 / 500 / 500 / 500 ns | 0.6 | 113.60 ns |

Global ordinary/array/aligned new calls are counted only inside the throughput apply loop; the known trade buffer is reserved. Trees/hash nodes still allocate. Measurement is not a production zero-allocation claim. The ID lookup pass uses a deterministic permutation and aggregates the result to prevent elimination. No CPU cache/branch hardware-counter profile was collected.

The clock produced 100 ns steps and some zero samples. Clock overhead is included in per-operation distributions, which must not be interpreted as exact sub-100 ns timings. Throughput has no per-operation clock calls. Small differences between depths/repetitions include desktop noise; construction, risk checks, quote publication, locks, journal and network are excluded from this core measurement.

## Scheduled TCP load and durable acknowledgements

Command: node tools/load.mjs SERVER OUTPUT. Client and server are independent processes on the same host. Three accounts; 64 in-flight slots each; two seconds of offered demand per scenario; one sample per scenario. Commands alternate buy insertion/cancellation with integer prices and quantities. This full-stack load does not replay a deep market or trade-heavy distribution; the separate core experiment covers depth and partial fills.

Demand uses a fixed monotonic schedule rather than waiting for each response. Full admission slots shed demand explicitly. Shed requests are never sent and do not consume account sequences. The burst scenario schedules 10,000 arrivals every 100 ms. This establishes overload behavior of the bounded client/server setup, not the server's maximum saturation rate in isolation. All eight runs had zero transport failures for submitted requests.

| Durability | Offered requests/s | Completed | Shed before send | Scheduled-to-response p99 | Socket-submit-to-response p99 |
|---|---:|---:|---:|---:|---:|
| buffered | 100 | 200 | 0 | 2.59 ms | 0.65 ms |
| buffered | 1000 | 2000 | 0 | 3.15 ms | 0.47 ms |
| buffered | 10000 | 20000 | 0 | 5.22 ms | 1.53 ms |
| buffered | 100000 burst | 19065 | 180935 | 46.46 ms | 8.77 ms |
| sync | 100 | 200 | 0 | 34.96 ms | 34.50 ms |
| sync | 1000 | 694 | 1306 | 956.08 ms | 955.91 ms |
| sync | 10000 | 584 | 19416 | 1080.67 ms | 1079.30 ms |
| sync | 100000 burst | 673 | 199327 | 915.05 ms | 913.53 ms |

Scheduled latency includes generator lateness and service/queueing; socket latency starts at the actual send. The CSV records both timestamps and every shed arrival, avoiding a completed-requests-only impression of overload. Distributions above apply to completions; shed demand has no measured response latency. The summary includes p50/p99/p99.9, maximum, elapsed completion time, replay sequence and restart time. A percentile over 200 completions is weak tail evidence; repeat longer runs before making an engineering SLO.

CSV data is gzip-compressed without dropping rows. JSON metadata records CPU, OS, Node version and machine memory. Power policy, frequency scaling, storage hardware and scheduler isolation were not controlled. No CPU pinning, network NIC, TLS or competing production traffic was involved. The first baseline measurements from before account/network support remain in the parent results directory and do not represent this release's full stack.

Sync performs a storage synchronization for every fresh request; buffered flushes to the OS without that guarantee. Under overload the observed sync throughput was around 200–247 completions/s, and queueing reached roughly one second. The core's millions of operations/s therefore cannot be advertised as durable exchange throughput. Group commit is a future measured design change, not an undocumented weakening of sync.
