# Architecture

## Layers
1. **HeapFile** (`engine/src/heap_file.cpp`) – binary slot file, see STORAGE_FORMAT.md.
2. **BPlusTree** (`bplus_tree.cpp`) – in-memory, unique `int64` keys → `RecordLocator{rid, offset}`; configurable `max_keys` M ≥ 3 (64 in the API/benchmarks). Leaf split copies the separator up, internal split moves it up. Minimum occupancy: leaf ⌈M/2⌉, internal ⌊M/2⌋. Underflow: borrow from a sibling, else merge; the root shrinks when it has one child. Leaves are chained via `next` for range scans. `validate()` checks ordering, separator bounds, equal leaf depth, leaf-chain order and counts; event counters record splits/merges/redistributions; `search_path()` and `to_json()` feed the UI.
3. **Database** – keeps heap and index consistent: insert (heap then index; rollback of the heap slot if the index insert fails), erase, list in key order, `rebuild_index`, `check_consistency`.
4. **QueryEngine** (`query.cpp`) – tokenizer/parser → `Plan` → executor. Planner: equality → `INDEX_POINT`; range with estimated selectivity ≤ 25 % (uniform assumption, using min/max key) → `INDEX_RANGE`; otherwise `FULL_SCAN`. `compare()` runs both paths and verifies identical rows. Metrics are counted by the executors, not estimated.
5. **C API** (`c_api.cpp`) – `ic_*` functions returning malloc'd JSON, freed with `ic_free`. Not thread-safe.
6. **FastAPI** (`backend/app`) – `engine_bridge.py` (ctypes, an RLock serialises calls), routers for health/stats, records, query, index, benchmark, diagnostics. Engine status codes map to HTTP 400/404/409/500/503. A corrupt/locked file degrades the API (503 + health report) instead of crashing.
7. **Benchmark runner** (`bench_main.cpp`) – standalone executable; the API starts it as a subprocess job (one at a time, cancellable, temp data files) and parses JSON progress lines.
8. **React dashboard** (`frontend/src`) – Overview, Query Workbench, B+ Tree Explorer, Storage Explorer, Performance Lab, Diagnostics. All data comes from the API.

## Decisions and trade-offs
- *Index not persisted*: removes a whole class of index/heap divergence bugs and keeps the format simple, at the cost of O(N) open time (measured: 1.12 s for 1 M records).
- *Results always key-ordered*: makes scan and index outputs comparable and the correctness check trivial.
- *JSON over the C boundary*: slower than a binary ABI but simple and safe; timings in benchmarks are taken inside C++ so the bridge is not measured.
