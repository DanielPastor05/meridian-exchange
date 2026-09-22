# Verification record

## Local Windows execution

2026-09-22: Windows 11 build 10.0.26200, AMD Ryzen 5 5500, 12 logical processors, MSVC 19.44 / Visual Studio 2022 Build Tools, CMake 3.31.6, Node 24.13.0. Release compilation uses /W4 /WX and /O2. The seven CTest entries pass locally.

| Test | Evidence |
|---|---|
| correctness | 80,000 operations against independent reference; FIFO/capacity/boundaries; all 59 torn-tail lengths and single-byte corruptions in a 152-byte fixture |
| accounts_and_sessions | 10,000 generated accounting operations; independent cash/inventory ledger; risk, ownership, reservations, self-trade, kill, retries and configuration mismatch |
| tcp_integration | real separate-process TCP clients, fragmentation, authentication, malformed frames, simultaneous clients, reconnect, snapshot gaps and slow reader |
| crash_recovery | externally kill seven exact boundaries; earlier acknowledged orders, both trade counterparties and exact cached outcome checked |
| protocol_boundaries | exact lengths, truncations, framing and 100,000 structured mutations, including accepted inputs |
| cli_roundtrip | legacy text gateway run/replay/restart/numeric parsing |
| benchmark_smoke | deterministic core benchmark outcomes |

The demo is executable assertions, not a pre-recorded screenshot. No performance threshold is tested in shared-runner CI. Parser mutation counts and generated commands are not separately authored test cases.

## Hosted verification

The workflow runs the same suite on Linux/GCC and Windows/MSVC, the complete suite on Linux/Clang ASan/UBSan, then a seeded 30-second libFuzzer run. The first completely successful hosted run is [35776351168](https://github.com/DanielPastor05/meridian-exchange/actions/runs/35776351168), revision 0e906ef. All three jobs passed, including the seven-test suites and the 30-second fuzz run. Initial failures found GCC file-deleter attribute handling, CMake regex portability and a keepalive gap in a slow-consumer test; fixes were verified by that fresh run. Later release packaging revisions are checked by the same workflow. The final release page identifies its source commit and verification run.

## Limits

External process kills are not disk power-loss tests. CRC detects the exercised corruptions, not malicious changes or an entire lost suffix. Network tests use loopback, not a physical NIC. Authentication/TLS hardening, production traffic realism and availability across machine loss are outside the verified scope. The single desktop benchmark does not establish latency guarantees; timing resolution produces zero-nanosecond samples in the instrumented core pass.
