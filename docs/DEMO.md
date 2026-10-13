# Demo recording checklist (≈ 5 minutes)

Setup: `scripts/build_engine.sh`, `(cd frontend && npm install && npm run build)`, `rm -f data/indexcraft.db`, `scripts/start_backend.sh`, open http://127.0.0.1:8000 in a 1440×900 window. Record with any screen recorder (OBS, or the OS recorder).

1. **Overview** – show empty engine; click *Generate 200 sample records*; point out record count = indexed keys.
2. **Storage Explorer** – insert key 5000, search it, try inserting it again (duplicate error toast), delete it (confirmation dialog).
3. **Query Workbench** – run `SELECT * FROM records WHERE key = 77;`, then `EXPLAIN SELECT * FROM records WHERE key BETWEEN 10 AND 60;`, then press *Compare scan vs index* and show records examined and identical results. Show a syntax error message.
4. **B+ Tree Explorer** – trace key 77, click a node, show occupancy and validation = Valid. (For a readable picture start a second API with `INDEXCRAFT_MAX_KEYS=4`.)
5. **Performance Lab** – run a 10 000 benchmark; point out the "measured in this run" label, charts, summary table; download summary.csv.
6. **Diagnostics** – re-run checks, show recorded test results (from `scripts/run_all_tests.sh`), supported syntax, build info; mention limitations (no crash safety).
Terminal bonus: `scripts/run_all_tests.sh` output.
