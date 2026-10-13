# Test cases and outcomes

Outcomes below are from the run recorded in `docs/test-results/latest.json` (produced by `scripts/run_all_tests.sh`): **C++ 110 / 110 passed, API 71 / 71 passed, 0 failed, 0 skipped.** Re-run the script to refresh; the Diagnostics page shows the same file.

## C++ (GoogleTest, `engine/tests`)
| Suite (count) | What it checks |
|---|---|
| BPlusTreeBasics (6), Range (3), Structure (5), Introspection (3) | insert/find/erase/update semantics, duplicate rejection, range iteration and bounds, leaf split / borrow / merge / root growth and shrink, `validate()` invariants, `search_path`, JSON export, event counters |
| Capacities/InsertionOrder (18) | ascending, descending and random insertion across node capacities (M = 3…), each followed by full invariant validation |
| Capacities/TreeReferenceModel (4), BPlusTreeLarge (1) | randomised insert/erase/lookup/range operations compared with a `std::map` reference after every batch; 1 M-key stress |
| HeapFile (11) | create/open, header CRC, insert/read/delete, tombstones and slot reuse (new rid, never reused), payload limits, stale locators, file lock |
| MalformedFile (17) | truncation, bad magic/version, bit flips in header/slot/payload (CRC), length > capacity, duplicate rid, zero-length and short files: each must fail with `Corrupt` and an offset, never crash |
| Database (15), DatabaseReferenceModel (1), DatabaseLarge (1) | heap/index consistency, persistence across reopen, delete + reopen, duplicate keys, index rebuild, random operations vs reference model, 200 k records |
| Tokenizer (3), Parser (5) | literals, keywords, syntax errors with position, unsupported constructs, int64 range |
| Executor (11), Compare (4) | every statement form, each access path, planner choice and thresholds, LIMIT, metrics, scan vs index row equality |
| JsonOutput (2) | escaping and valid JSON for hostile payloads |

*Mutation check (during development):* deliberately injected bugs in the tree (wrong split separator, skipped borrow, wrong chain link) were each caught by at least one test.

## API (pytest, `backend/tests`, real engine, temp data dirs)
records (22): CRUD, pagination, validation, duplicate → 409, missing → 404, key/payload limits, sample generation · query (26): all statements, forced paths, compare, syntax errors → 400, row limits · index (7): tree JSON, search path, validate, consistency, rebuild · benchmark (8): job lifecycle, cancellation, parameter validation, one-at-a-time, id validation, CSV download · diagnostics (8): build info, syntax doc, error log, operations log, test-result file handling, corrupt-file degradation, SPA fallback with JSON 404 for unknown `/api` paths.

## Not covered (known gaps)
No frontend unit tests (the UI was exercised manually and via Playwright screenshots, which is not an automated regression suite); no crash-injection tests (crash safety is not claimed); no concurrency tests beyond the API lock; no cold-cache benchmarks.
