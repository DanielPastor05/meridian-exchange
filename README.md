# Meridian Exchange

A deterministic, single-instrument limit-order matching engine in C++20.
Price-time priority, partial fills, cancellation, a checksummed command journal,
and a deliberately simple reference engine that checks every execution and the
entire book across 80,000 generated commands.

**Status: working local foundation.** This release has a console gateway and a
core benchmark. Network transport, account risk limits and market-data feeds are
future milestones. Performance numbers measure the in-memory core; they do not
measure a production exchange or a durable acknowledgement.

## Build and run

Requires CMake 3.20+ and a C++20 compiler. No third-party runtime dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Linux, with a fresh journal:

```sh
./build/exchange run --journal demo.journal --input examples/session.txt
./build/exchange replay demo.journal
./build/exchange_bench 200000 5
```

Windows, with the Visual Studio generator:

```powershell
.\build\Release\exchange.exe run --journal demo.journal --input examples/session.txt
.\build\Release\exchange.exe replay demo.journal
.\build\Release\exchange_bench.exe 200000 5
```

Reopening a journal restores its book and continues its sequence. To repeat the
example as a fresh session, use a new journal path. Re-submitting the same input
is a new set of commands, not a replay.

The example's buy order 4 fills seven units from order 1 and three from order 2,
both at 10005 ticks. After the cancellation and final buy, order 5 rests with two
units at 10012. Read-only replay must produce exactly the same book and fingerprint.

## Commands and contract

```text
NEW 1 SELL 10005 7
NEW 2 BUY 10005 3
CANCEL 1
BOOK
```

- One instrument per engine. Prices are positive signed 64-bit integer ticks;
  quantities and IDs are unsigned 64-bit integers. The instrument's tick size is
  external to the matching core. No floating-point money arithmetic.
- Limit orders are good-till-cancelled. Best price wins, then arrival order.
  Executions use the resting maker's price. Unfilled incoming quantity rests.
- Order IDs are unique **among active orders**. Reuse after cancellation or full
  execution is allowed. An order ID is not an idempotency key.
- Cancelling removes the remaining quantity. Unknown IDs are rejected.
- Zero ID, nonpositive price, zero quantity and invalid enum values are rejected.
  Duplicate IDs and full capacity are rejected before any matching occurs.
- Admission reserves an available order slot even for a potentially fully
  executable incoming order. A full book rejects all new orders until a cancel
  frees a slot. This conservative policy is part of the replay contract.
- The CLI journals every syntactically parsed command, including engine
  rejections. Malformed text is reported on stderr, consumes no sequence, and
  causes exit code 2 after the remaining lines have been processed.
- `BOOK` reads state without consuming a sequence. Output is JSON Lines. IDs,
  prices, quantities, sequences and fingerprints are decimal **strings** to
  preserve 64-bit values in JavaScript consumers.

Run without arguments for the CLI options. Capacity defaults to 65,536 orders;
`--capacity N` selects 1..1,000,000 and is recorded in the journal header.

## Architecture

```text
text input -> parse -> append journal -> flush/sync -> apply -> JSON response
                                                   |
                     restart: replay commands -----+

matching core:
  ordered price levels -> FIFO linked slots in a preallocated order pool
  order-ID hash index  -> direct cancellation lookup
```

One thread owns each book. The order pool and free-slot stack are preallocated.
Price levels use `std::map`, and the ID index uses `std::unordered_map`. Those
containers still allocate nodes; this implementation does not claim zero
allocations. Trade output can also grow unless the caller reserves its buffer.

These choices make a small, verifiable baseline. Profile allocation cost and
price-level lookup before replacing either structure. See [DESIGN.md](docs/DESIGN.md)
for complexity, persistence and recovery details.

## What is checked

`exchange_tests` contains named scenarios plus 20 deterministic random seeds,
4,000 commands each, against a vector-based reference implementation. Both engines
must produce the same status, sequence, remaining quantity, ordered trades and
complete resting book after **every command**. Checks remain active in Release.

Additional checks cover cancellation at the head/middle/tail, slot reuse, integer
limits, rejection without partial effects, replay of accepted and rejected
commands, writer exclusion, all 43 incomplete record-tail lengths, every
single-byte corruption in a two-record fixture, and incomplete headers.

CTest also runs a benchmark smoke check and a CLI test spanning initial input,
read-only replay, restart/cancellation, and malformed numeric input.

The GitHub Actions workflow is configured for Windows/MSVC, Linux/GCC and
Linux/Clang with address/undefined-behavior sanitizers. Local verification results
are recorded separately in [VALIDATION.md](docs/VALIDATION.md); a workflow file
alone is not evidence that those remote jobs have run.

## Benchmark method

`exchange_bench [commands=200000] [repetitions=5]` emits CSV with raw repetitions
in comment lines. Inputs are generated before timing. One untabulated warm-up
precedes the repeated throughput runs. A separate instrumented run measures
individual command latency and checks the final fingerprint again.

| Workload | Operations | Peak resting orders |
|---|---|---:|
| `rest_cancel` | Add 32 noncrossing orders, then cancel them | 32 |
| `cross` | Add a sell, fully cross it with a buy | 1 |
| `sweep` | Add 32 sells at eight levels, cross them with one buy | 32 |

This is a **small-book, synthetic, closed-loop baseline**. Per-command latency
includes clock instrumentation. The sweep percentiles mix cheap adds and expensive
sweeps. Report the distribution and workload, not a single universal latency.
It excludes parsing, network queues, storage and durable responses. It does not
measure overload, production market traffic, or many-symbol capacity.

Keep the raw CSV, compiler/build settings, CPU/OS, power policy and workload with
any published result. No performance threshold is asserted in shared-runner CI.

## Durability and limitations

`sync` is the default: each command is written and synchronized using `fsync`
(POSIX) or `_commit` (Windows) **before** applying it and returning a result.
`buffered` flushes the C library stream to the OS but skips the storage sync;
power loss may lose acknowledged commands. Device/filesystem guarantees still
bound what a storage sync means.

Reopening validates the header, checksums and sequence. Only an incomplete final
record is discarded. A complete record with a bad checksum fails closed. Read-only
replay reports incomplete tail bytes and never modifies the file. Run read-only
replay against a stopped writer or a stable copy, not a growing journal.

A crash after journaling but before returning can commit an operation for which
the caller never received a response. There is no exactly-once request protocol.
Allocation or I/O failures are fatal to the session: reopen and replay before
continuing. Disk corruption detection is not replication or automatic repair.

This version has no network gateway, authentication, account ledger, self-trade
prevention, market orders, auctions, amendments, snapshots or journal rotation.
It is an engineering simulator. The [roadmap](docs/ROADMAP.md) defines the next
three measurable stages toward a stronger internship portfolio project.

## Authorship

This initial implementation was generated with Codex assistance. Treat the
reference tests, measurements, subsequent design decisions and your ability to
explain and modify the code as the evidence of engineering work.
