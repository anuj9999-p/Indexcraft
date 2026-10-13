import csv
import io
import time

from .conftest import wait_for_job

SMALL = {"sizes": [1000], "reps": 50, "scan_reps": 3, "range_reps": 3, "heavy_reps": 2, "warmup": 1, "max_keys": 32}


def test_benchmark_job_lifecycle_produces_measured_results_and_csv(client):
    r = client.post("/api/benchmark", json=SMALL)
    assert r.status_code == 202
    job = r.json()
    assert job["status"] in ("queued", "running") and len(job["job_id"]) == 12
    final = wait_for_job(client, job["job_id"])
    assert final["status"] == "completed", final
    assert final["progress_percent"] == 100.0 and final["results_available"] is True

    res = client.get(f"/api/benchmark/{job['job_id']}/results").json()
    assert res["measured"] is True and res["meta"]["dataset_sizes"] == [1000]
    assert res["meta"]["system"]["logical_cpus"] >= 1 and "cache_conditions" in res["meta"]["methodology"]
    rows = {(r["workload"], r["access_path"]): r for r in res["summary"]}
    pt_idx = rows[("point_lookup", "INDEX_POINT")]
    pt_scan = rows[("point_lookup", "FULL_SCAN")]
    assert pt_idx["repetitions"] == 50 and pt_scan["repetitions"] == 3
    assert pt_scan["mean_records_examined"] == 1000 and pt_idx["mean_records_examined"] == 1
    for r in res["summary"]:
        if r["workload"] != "index_maintenance_overhead":
            assert r["mean_ns"] > 0 and r["median_ns"] is not None and r["p99_ns"] >= r["median_ns"]
    assert pt_idx["mean_ns"] < pt_scan["mean_ns"]  # an index lookup beats scanning 1000 records
    derived = rows[("index_maintenance_overhead", "DERIVED")]
    assert derived["median_ns"] is None and "derived" in derived["notes"]
    for wl in ("insert_heap_only", "insert_index_only", "insert_full", "insert_full_ascending",
               "index_rebuild_on_open", "full_scan", "range_1pct", "range_50pct"):
        assert any(k[0] == wl for k in rows), wl

    raw = client.get(f"/api/benchmark/{job['job_id']}/csv/raw")
    assert raw.status_code == 200 and raw.headers["content-type"].startswith("text/csv")
    assert "attachment" in raw.headers["content-disposition"]
    parsed = list(csv.DictReader(io.StringIO(raw.text)))
    assert len(parsed) == res["raw_rows"] and int(parsed[0]["latency_ns"]) > 0
    summ = client.get(f"/api/benchmark/{job['job_id']}/csv/summary")
    assert summ.status_code == 200 and summ.text.splitlines()[0].startswith("run_id,dataset_size")

    assert any(j["job_id"] == job["job_id"] for j in client.get("/api/benchmark").json())


def test_benchmark_does_not_touch_the_live_database(loaded):
    before = loaded.get("/api/records", params={"limit": 200}).json()
    job = loaded.post("/api/benchmark", json=SMALL).json()
    assert wait_for_job(loaded, job["job_id"])["status"] == "completed"
    assert loaded.get("/api/records", params={"limit": 200}).json() == before


def test_only_one_job_runs_at_a_time_and_jobs_can_be_cancelled(client):
    big = {"sizes": [100000], "reps": 1000, "scan_reps": 50, "range_reps": 50, "heavy_reps": 20, "warmup": 3}
    first = client.post("/api/benchmark", json=big)
    assert first.status_code == 202
    second = client.post("/api/benchmark", json=SMALL)
    assert second.status_code == 409 and second.json()["detail"]["code"] == "JOB_RUNNING"
    jid = first.json()["job_id"]
    time.sleep(0.5)
    cancelled = client.delete(f"/api/benchmark/{jid}")
    assert cancelled.status_code == 200
    final = wait_for_job(client, jid)
    assert final["status"] == "cancelled"
    assert client.get(f"/api/benchmark/{jid}/results").status_code == 409
    assert client.delete(f"/api/benchmark/{jid}").status_code == 409  # already finished


def test_progress_is_reported_while_running(client):
    job = client.post("/api/benchmark", json={**SMALL, "sizes": [20000], "scan_reps": 10}).json()
    seen_running = False
    for _ in range(300):
        j = client.get(f"/api/benchmark/{job['job_id']}").json()
        if j["status"] == "running" and j["stage"]:
            seen_running = True
        if j["status"] in ("completed", "failed"):
            break
        time.sleep(0.05)
    assert seen_running
    assert wait_for_job(client, job["job_id"])["status"] == "completed"


def test_invalid_benchmark_requests(client):
    for body in [
        {"sizes": []},
        {"sizes": [10]},
        {"sizes": [1000, 1000]},
        {"sizes": [1000, 2000, 3000, 4000]},
        {"sizes": [1000], "reps": 0},
        {"sizes": [1000], "scan_reps": 1000},
        {"sizes": [1000], "max_keys": 2},
        {"sizes": ["many"]},
    ]:
        assert client.post("/api/benchmark", json=body).status_code == 422, body
    too_big = client.post("/api/benchmark", json={"sizes": [10**7]})
    assert too_big.status_code == 422 and too_big.json()["detail"]["code"] == "SIZE_NOT_ALLOWED"


def test_unknown_and_malicious_job_ids_are_404(client):
    for jid in ("deadbeef0000", "..", "not-a-job", "%2e%2e", "../../etc"):
        assert client.get(f"/api/benchmark/{jid}").status_code == 404, jid
        assert client.get(f"/api/benchmark/{jid}/results").status_code == 404, jid
    assert client.get("/api/benchmark/deadbeef0000/csv/raw").status_code == 404


def test_results_of_an_unfinished_job_are_not_available(client):
    big = {"sizes": [100000], "scan_reps": 50, "range_reps": 50, "heavy_reps": 20}
    jid = client.post("/api/benchmark", json=big).json()["job_id"]
    r = client.get(f"/api/benchmark/{jid}/results")
    assert r.status_code == 409 and r.json()["detail"]["code"] == "NOT_READY"
    assert client.get(f"/api/benchmark/{jid}/csv/raw").status_code == 409
    client.delete(f"/api/benchmark/{jid}")
    wait_for_job(client, jid)


def test_missing_benchmark_binary_is_a_clean_503(tmp_path):
    from fastapi.testclient import TestClient
    from app.main import create_app
    from .conftest import make_settings

    s = make_settings(tmp_path, bench_bin=tmp_path / "nope")
    with TestClient(create_app(s)) as c:
        r = c.post("/api/benchmark", json=SMALL)
        assert r.status_code == 503 and r.json()["detail"]["code"] == "ENGINE_UNAVAILABLE"
