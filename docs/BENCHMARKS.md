# Benchmarks

Run `run-20261012T201801Z` (files in `benchmarks/results/run-20261012T201801Z/`: `summary.csv`, `raw.csv`, `meta.json`). Produced by `scripts/run_benchmarks.sh 10000,100000,1000000`; nothing here is sample data.

**System:** Intel Xeon @ 2.80 GHz, 2 logical CPUs, 8 029 MiB RAM, Linux 6.18 x86_64, GCC 13.3.0, Release `-O3 -DNDEBUG`, B+ Tree node capacity 64, seed 42.

**Method.** Keys are a seeded random permutation of 1..N; payload 16–111 bytes. Timing is `steady_clock` around `execute_plan` with a prepared plan (parse/plan excluded), rows materialised; inserts timed in batches of 1000. Warm OS page cache (not dropped). 3 untimed warm-ups per measurement. Repetitions: 1000 point lookups (index), 20 (point scan, full scan, 0.1 %/1 % ranges), 5 for 10 %/50 % ranges and rebuild measured 3×. Every result is checked against the expected row count and heap/index consistency is checked after load. These are single-machine, warm-cache, small-repetition measurements: treat ratios as indicative, not as general performance claims.

## Median latency (ns converted)
| Workload | 10 k | 100 k | 1 M |
|---|---|---|---|
| point lookup – index | 2.24 µs | 2.10 µs | 2.96 µs |
| point lookup – full scan | 2.88 ms | 30.2 ms | 283.7 ms |
| range 0.1 % – index / scan | 10.9 µs / 2.68 ms | 112 µs / 30.1 ms | 1.71 ms / 288 ms |
| range 1 % – index / scan | 89 µs / 2.69 ms | 1.07 ms / 27.5 ms | 15.0 ms / 287 ms |
| range 10 % – index / scan | 0.89 ms / 2.82 ms | 12.5 ms / 31.3 ms | 143 ms / 289 ms |
| range 50 % – index / scan | 4.50 ms / 3.33 ms | 57.7 ms / 35.7 ms | 733 ms / 379 ms |
| insert heap only (per record) | 1.12 µs | 0.98 µs | 1.07 µs |
| insert index only | 0.14 µs | 0.25 µs | 0.62 µs |
| insert heap + index (random order) | 1.31 µs | 1.67 µs | 2.30 µs |
| insert heap + index (ascending) | 1.32 µs | 1.19 µs | 1.29 µs |
| index rebuild at open | 6.4 ms | 83 ms | 1.12 s |

## Observations
- Point lookups: index cost grows slowly (depth 2→3) while scan cost grows linearly: ratios ≈ 1 300× (10 k), 14 000× (100 k), 96 000× (1 M).
- Range queries: the index wins at 0.1–10 % selectivity (≈ 2× at 10 % for 1 M) but is **slower than a scan at 50 %** (733 ms vs 379 ms at 1 M) because each matched record costs a heap read through the index. This supports the planner's 25 % threshold; the threshold itself was chosen a priori and was not tuned or evaluated for optimality.
- Insert maintenance cost grows with dataset size for random-order keys (cache misses in the tree), while ascending inserts stay flat.
- The UI Performance Lab runs the same executable; its numbers vary run to run (e.g. a 10 k UI run measured ≈ 2.7 µs point lookup).
- 5-repetition workloads have coarse p95/p99 (equal to max).
