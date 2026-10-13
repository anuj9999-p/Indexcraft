# IndexCraft

**A Lightweight Relational Storage Engine with Adaptive B+ Tree Indexing for High-Throughput Query Execution** (BCSE302P Database Systems Lab)

IndexCraft is a small storage engine written from scratch in C++17: a custom binary heap file, a self-implemented B+ Tree, and a SQL-like query layer with two real execution paths (full scan and B+ Tree index) and an access-path planner. A FastAPI service calls the real engine through a C API (`ctypes`); a React dashboard visualises the real tree, runs queries and runs real benchmarks. There is no SQLite/PostgreSQL/MongoDB anywhere, and no second B+ Tree in Python.

> **Honest scope.** This is a teaching/research prototype. It has **no write-ahead log and no crash safety**, the index is **rebuilt from the heap file at every open**, keys are unique 64-bit integers, and the query language is intentionally tiny. See [Limitations](#limitations).

## Contents
- [Prerequisites](#prerequisites) · [Quick start](#quick-start) · [Query language](#query-language)
- [Architecture](#architecture) · [Tests](#tests) · [Benchmarks](#benchmarks) · [Screenshots](#screenshots)
- [Troubleshooting](#troubleshooting) · [Limitations](#limitations) · [Further docs](#further-docs)

## Prerequisites
Linux/macOS with: a C++17 compiler (GCC ≥ 9 / Clang ≥ 10), CMake ≥ 3.16, GoogleTest (`sudo apt-get install libgtest-dev`, or configure with `-DINDEXCRAFT_FETCH_GTEST=ON`), Python ≥ 3.10, Node ≥ 18 + npm. `bc` is used by `run_all_tests.sh`.

## Quick start
```bash
git clone <this repo> IndexCraft && cd IndexCraft
scripts/build_engine.sh                      # CMake Release build -> build/engine/{libindexcraft.so,indexcraft_cli,indexcraft_bench,indexcraft_tests}
pip install -r backend/requirements.txt      # add --break-system-packages on some distros
scripts/install_frontend.sh                  # npm install
(cd frontend && npm run build)               # optional: lets the API serve the UI on :8000
scripts/start_backend.sh                     # API on http://127.0.0.1:8000  (docs at /docs)
scripts/start_frontend.sh                    # dev UI on http://127.0.0.1:5173 (proxies /api -> :8000)
```
If you ran `npm run build`, the backend alone serves the whole app at http://127.0.0.1:8000. Click **Generate 200 sample records** on the Overview page (or `scripts/generate_samples.sh 5000` with the API stopped) to get data.

Command-line engine use (no API):
```bash
build/engine/indexcraft_cli init     --db data/demo.db
build/engine/indexcraft_cli generate --db data/demo.db --count 10000
build/engine/indexcraft_cli query    --db data/demo.db "SELECT * FROM records WHERE key = 4242"
build/engine/indexcraft_cli compare  --db data/demo.db "SELECT * FROM records WHERE key BETWEEN 100 AND 200"
build/engine/indexcraft_cli check    --db data/demo.db
```

## Query language
```sql
INSERT INTO records (key, payload) VALUES (42, 'hello');
SELECT * FROM records WHERE key = 42;
SELECT * FROM records WHERE key > 10 AND key <= 90 LIMIT 20;
SELECT * FROM records WHERE key BETWEEN 100 AND 200;
DELETE FROM records WHERE key = 42;
EXPLAIN SELECT * FROM records WHERE key BETWEEN 1 AND 500;
```
Only the `key` column is filterable. `SELECT` results are **always ordered by key**, so the scan path and index path return identical rows (the Workbench's *Compare* button executes both and verifies this). The planner chooses `INDEX_POINT` for equality, `INDEX_RANGE` when the estimated selectivity is ≤ 25 % (assuming uniformly spread keys), otherwise `FULL_SCAN`. The Workbench can also force a path. Each query reports records examined, index lookups, index nodes visited, heap reads, parse/exec time and the plan text.

## Architecture
```
React/Vite dashboard ──HTTP──▶ FastAPI ──ctypes──▶ libindexcraft.so (C API, JSON)
                                  │                      │
                                  │                      ├─ QueryEngine (parser, planner, scan / index executors)
                                  └─ subprocess ───▶     ├─ Database (heap + index consistency)
                                     indexcraft_bench    ├─ BPlusTree (in memory, rebuilt on open)
                                                         └─ HeapFile (binary file, CRC-checked slots)
```
Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), file format: [docs/STORAGE_FORMAT.md](docs/STORAGE_FORMAT.md).

Security: the API uses one fixed data file name inside a configured directory; clients cannot supply paths. Benchmarks run a fixed executable with validated numeric parameters on temporary files; job ids are regex-validated; there is no command execution endpoint.

## Tests
```bash
scripts/run_all_tests.sh      # C++ (GoogleTest) + API (pytest); writes docs/test-results/latest.json shown on the Diagnostics page
scripts/run_cpp_tests.sh      # C++ only
scripts/run_api_tests.sh      # API only
```
Last recorded run (this repo, `docs/test-results/latest.json`): **C++ 110/110 passed, API 71/71 passed.** The C++ suite includes randomised comparison against a `std::map` reference model, malformed-file fixtures, persistence/reopen, and 1 M-key and 200 k-record tests. Test cases are listed in [docs/TEST_CASES.md](docs/TEST_CASES.md).

## Benchmarks
```bash
scripts/run_benchmarks.sh 10000,100000,1000000     # writes benchmarks/results/<run-id>/{summary.csv,raw.csv,meta.json}
```
or use the **Performance Lab** page. Method, hardware and the full table of one recorded run (10 k / 100 k / 1 M records) are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Headline (median, warm cache, 2-vCPU VM): point lookup at 1 M records ≈ **3.0 µs via the index vs ≈ 284 ms by full scan**; index range scans win up to ≈ 10 % selectivity and **lose at 50 %**, which is why the planner switches to scanning above 25 %.

## Screenshots
Genuine captures of the running application are in [`docs/screenshots/`](docs/screenshots/) (`01-overview` … `07-performance-lab`). The B+ Tree screenshot uses a separate throw-away database with node capacity 4 and 21 keys so the structure is readable; all other pages use the main demo data (2 000 generated sample records).

## Troubleshooting
| Symptom | Cause / fix |
|---|---|
| API health `degraded`, 503 on data endpoints | engine library not built (`scripts/build_engine.sh`), or data file corrupt/locked – the message names the offset or says another process holds the lock |
| `generate_samples.sh` fails with "locked" | the API has the file open; stop it first |
| UI shows "Cannot reach the IndexCraft API" | backend not running on :8000 |
| Benchmark refused | one job at a time; size above `INDEXCRAFT_MAX_BENCH_SIZE`; or not enough free disk (1 M needs several GB of temp space) |
| CMake can't find GTest | `apt-get install libgtest-dev` or `cmake -DINDEXCRAFT_FETCH_GTEST=ON` |

## Limitations
- **No WAL, no transactions, no crash safety.** A crash mid-write can leave a torn slot; open detects it (CRC/length checks) and refuses the file with an offset, but does not repair it.
- B+ Tree is **memory-only**; it is rebuilt by scanning the heap at open (≈ 1.1 s for 1 M records here).
- Heap has **no compaction or coalescing**; deleted slots are reused best-fit only when the new payload fits.
- Single table, one unique `int64` key, text payload ≤ 1 MiB; API keys limited to ±(2^53 − 1).
- Single-threaded engine; the API serialises calls with a lock.
- Planner selectivity assumes uniform keys; there are no statistics. At ~50 % selectivity the index range path is slower than a scan (measured).
- Benchmarks are **warm-cache** on a small 2-vCPU VM; cold-cache and concurrent workloads were not measured. Percentiles with few repetitions (the 5-rep heavy workloads) are coarse.
- The `EXPLAIN` response reports `statement: "SELECT"` with `executed: false`.

## Further docs
[Lab report](docs/LAB_REPORT.md) · [Architecture](docs/ARCHITECTURE.md) · [Storage format](docs/STORAGE_FORMAT.md) · [Benchmarks](docs/BENCHMARKS.md) · [Test cases](docs/TEST_CASES.md) · [Demo recording](docs/DEMO.md)
