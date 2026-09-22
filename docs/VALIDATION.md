# Local verification — 22 September 2026

## Environment

- Windows 11 Pro, x64, OS build 10.0.26200.
- AMD Ryzen 5 5500, 12 logical processors.
- MSVC 19.44.35228, Visual Studio 2022 Build Tools.
- CMake 3.31.6-msvc6, Visual Studio 17 2022 x64 generator.
- Release (`/O2`, `NDEBUG`) and Debug configurations.

## Results

| Check | Result |
|---|---|
| Release compilation, `/W4 /WX` | Passed |
| Debug compilation, `/W4 /WX` | Passed |
| Engine/reference comparison | Passed: 80,000 commands, 20 seeds |
| FIFO, cancellations, slot reuse, partial fills, boundaries | Passed |
| Journal replay and writer exclusion | Passed |
| Every 1..43-byte incomplete final record | Passed |
| Every corrupted byte in a 112-byte journal fixture | Rejected as expected |
| CLI run, replay, restart and malformed input | Passed |
| Benchmark smoke | Passed |

CTest reports 3/3 tests passing in Release and Debug. The correctness executable
contains seven scenario groups; generated cases are compared after every command,
not counted as 80,000 separately authored tests.

Linux builds and sanitizer jobs are configured in GitHub Actions but have **not**
been executed in this local Windows session. The POSIX filesystem-sync path is
therefore not locally verified. There has been no physical power-loss test and no
live market or network benchmark.

## Measured baseline

Command: `exchange_bench 2000000 5`, Release. One warm-up, five raw throughput
repetitions, and a separate per-command latency pass for each workload.

| Workload | Median ns/command | Median commands/s | Instrumented p99 | Instrumented p99.9 |
|---|---:|---:|---:|---:|
| rest_cancel | 69.19 | 14,452,548 | 200 ns | 300 ns |
| cross | 59.21 | 16,889,025 | 200 ns | 200 ns |
| sweep | 83.75 | 11,940,078 | 1,000 ns | 1,600 ns |

Raw data: [windows-msvc-release.csv](../bench/results/windows-msvc-release.csv).
Machine/run metadata: [environment.json](../bench/results/environment.json).

These are observations from one desktop, not latency guarantees. The workloads
have at most 32 resting orders and use synthetic, pre-generated commands. The
latency clock on this run produced many samples in 100 ns increments; the numbers
include instrumentation and scheduling noise. The sweep distribution mixes adds
and sweeps. Throughput is measured separately without per-command timestamps.

No core pinning or controlled power policy was used. No agent build or test was
running concurrently with the recorded benchmark, but normal desktop background
activity was not isolated. An earlier measurement taken during compilation was
discarded. Parsing, network latency, queueing and journal sync are excluded.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Then run `build/exchange_bench 2000000 5`, or
`build/Release/exchange_bench.exe 2000000 5` with the Visual Studio generator.
Expect performance variation; correctness checks should remain deterministic.
