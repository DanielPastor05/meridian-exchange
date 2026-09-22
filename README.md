# Meridian Exchange

[![Verify exchange](https://github.com/DanielPastor05/meridian-exchange/actions/workflows/ci.yml/badge.svg)](https://github.com/DanielPastor05/meridian-exchange/actions/workflows/ci.yml)

A C++20 exchange simulator for studying matching, crash recovery and the difference between core latency and durable acknowledgements.

One instrument, price/time FIFO, partial fills and cancellation. A bounded binary TCP gateway adds authenticated accounts, integer risk checks, cash/inventory reservations, persistent request deduplication, a kill switch and a sequenced trade/top-of-book feed.

## Run the demonstration

Requires CMake 3.20+, a C++20 compiler and Node.js 24 for client/testing tools. C++ executables use only the standard library and OS APIs. There are no npm packages.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
# Linux:
node tools/demo.mjs ./build/exchange_server
# Windows / Visual Studio:
node tools/demo.mjs ./build/Release/exchange_server.exe
```

The demo starts its own server, crosses orders from two independent clients, verifies the balances, forcibly restarts the server, retries without executing twice, and activates the account kill switch. Temporary files are removed afterwards.

## Interactive session

```sh
./build/exchange_server --journal demo.journal --accounts examples/accounts.conf --port 9000
# A second terminal: account 1 rests a sell.
node tools/client.mjs --account 1 --token 11111111111111111111111111111111 new 1 10 SELL 100 10
# Another client: account 2 buys four at the maker's price of 100.
node tools/client.mjs --account 2 --token 22222222222222222222222222222222 new 1 20 BUY 105 4
node tools/client.mjs --account 1 --token 11111111111111111111111111111111 account
```

On Windows use `build/Release/exchange_server.exe`. Tokens in examples are public fixtures; the default bind address is loopback. Authentication is a plain binary token protocol without TLS. Use only a local or independently secured, trusted network.

Requests carry a per-account sequence starting at one. The **last** identical request returns its original outcome after reconnect/restart. Conflicting, older and skipped sequences cannot execute. Keep one unacknowledged request per account when recovery of its exact response matters. IDs identify active orders, not retry keys.

## What the tests establish

- 80,000 commands compared after every operation with an independent vector reference book.
- 10,000 account operations checked against an independent cash/inventory ledger, plus explicit risk and reservation boundaries.
- Real TCP clients: fragmented/malformed messages, ownership, concurrency, retries, feed gaps and slow-reader isolation.
- External process termination at seven append/sync/apply/response boundaries; recovery preserves acknowledged history and settles a trade once.
- Every incomplete final record length, byte corruption, writer exclusion, parser boundary tests and 100,000 structured parser mutations.
- Hosted Windows/MSVC, Linux/GCC and Linux/Clang ASan/UBSan jobs; a separate bounded libFuzzer run. See the linked workflow for actual status and artifacts.

## Performance with evidence

The core profiler uses 1k, 10k and 100k resting orders, separate cancellation/insertion/partial/full-fill distributions, allocation counts and ID-lookup timings. The independent Node load generator schedules demand on a fixed timeline and records client admission shedding, scheduler lateness and every completed request. It measures buffered and disk-synchronized acknowledgements separately.

```sh
./build/exchange_profile 100000 3
node tools/load.mjs ./build/exchange_server bench/results/my-run
```

See [performance](docs/PERFORMANCE.md), [raw measurements](bench/results/windows-2026-09-22), [verification](docs/VALIDATION.md), [wire protocol](docs/PROTOCOL.md) and [design/failure contract](docs/DESIGN.md). The tiny-book `exchange_bench` remains a historical microbenchmark, not an end-to-end latency claim.

## Scope

Sync mode appends and calls `fsync` / `_commit` before applying each new request and replying. Buffered mode omits disk synchronization and can lose acknowledged operations after power failure. A complete corrupt record stops recovery; an incomplete final record is truncated. Journal v2 deliberately rejects the old v1 format.

The implementation is an engineering simulator, not a deployed financial venue. It has one serialized book, GTC limit orders, funded long-only accounts and a bounded polling feed. It does not provide multi-symbol routing, market/IOC orders, replacement, replicated consensus, cross-account settlement or exchange connectivity. [Scope decisions](docs/ROADMAP.md) explain when batching, checkpointing and different data structures would be justified.

Generated with Codex assistance. The useful portfolio evidence is reproducible behavior, honest measurements, and the author's ability to explain and change the design. [Spanish starting guide](EMPIEZA-AQUI.md).
