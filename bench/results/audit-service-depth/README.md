# Full-service depth check (2026-09-22)

Windows Release build, three repetitions at 1k/10k/100k resting orders on one price level.
Run: exchange_service_profile 3. Build and host details are in metadata.json.
The recorded base commit is dirty because this benchmark and the risk oracle are new;
source SHA-256 hashes identify the measured files independently of the later commit.

This times ExchangeState risk admission, reservations, matching, cached quotes and public events.
It excludes journal I/O, TCP, TLS and queueing; it is not an end-to-end latency claim.

Median steady cancel/new cost is 324 ns, 521 ns and 636 ns per request respectively.
The 100x depth increase does not produce the former linear per-quote scan. Remaining
map/set/cache costs still vary with depth; this does not claim constant-time requests.
All snapshot pages together copy exactly N orders at each depth, instead of copying
N orders per page. Repeated final hashes agree, with invariant verification outside timing.

These short runs demonstrate the targeted scaling repair only. They do not replace
long, repeated, realistic service benchmarks under trading, durability and subscriber load.
