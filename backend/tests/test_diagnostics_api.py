import json

from fastapi.testclient import TestClient

from app.main import create_app
from .conftest import make_settings


def test_build_and_syntax_diagnostics(client):
    b = client.get("/api/diagnostics/build").json()
    assert b["engine"]["storage_format_version"] == 1 and b["engine"]["cxx_standard"] >= 201703
    assert b["engine"]["compiler"] and b["backend"]["python"]
    s = client.get("/api/diagnostics/syntax").json()
    assert any(x["name"] == "INSERT" for x in s["statements"]) and s["rules"]


def test_test_results_are_only_shown_when_a_real_run_recorded_them(client, settings):
    r = client.get("/api/diagnostics/test-results").json()
    assert r["available"] is False and "run_all_tests" in r["message"]
    settings.test_results_file.parent.mkdir(parents=True)
    settings.test_results_file.write_text(json.dumps({"generated_at": "t", "suites": [{"name": "x", "passed": 1}]}))
    r = client.get("/api/diagnostics/test-results").json()
    assert r["available"] is True and r["suites"][0]["name"] == "x"
    settings.test_results_file.write_text("{broken")
    assert client.get("/api/diagnostics/test-results").json()["available"] is False


def test_corrupt_data_file_degrades_the_api_instead_of_crashing(tmp_path):
    s = make_settings(tmp_path)
    s.data_dir.mkdir(parents=True)
    s.data_file.write_bytes(b"this is definitely not an IndexCraft data file" * 3)
    original = s.data_file.read_bytes()
    with TestClient(create_app(s)) as c:
        h = c.get("/api/health")
        assert h.status_code == 200
        body = h.json()
        assert body["status"] == "degraded" and body["engine_open"] is False and "CORRUPT" in body["error"]
        for path in ("/api/stats", "/api/records", "/api/index/tree", "/api/index/validate"):
            r = c.get(path)
            assert r.status_code == 503 and r.json()["detail"]["code"] == "ENGINE_UNAVAILABLE", path
        assert c.post("/api/records", json={"key": 1, "payload": "x"}).status_code == 503
        assert c.post("/api/query", json={"sql": "SELECT * FROM records"}).status_code == 503
    assert s.data_file.read_bytes() == original  # never "repaired" silently


def test_missing_engine_library_degrades_cleanly(tmp_path):
    s = make_settings(tmp_path, lib_path=tmp_path / "missing.so")
    with TestClient(create_app(s)) as c:
        body = c.get("/api/health").json()
        assert body["status"] == "degraded" and body["engine_loaded"] is False and "not found" in body["error"]
        assert c.get("/api/stats").status_code == 503


def test_locked_data_file_is_reported(settings):
    first = TestClient(create_app(settings))
    with first:
        with TestClient(create_app(settings)) as second:  # same file: the engine's file lock refuses it
            body = second.get("/api/health").json()
            assert body["status"] == "degraded" and "locked" in body["error"]
        assert first.get("/api/health").json()["status"] == "ok"


def test_cors_allows_the_dev_frontend_only(client):
    ok = client.get("/api/health", headers={"Origin": "http://localhost:5173"})
    assert ok.headers.get("access-control-allow-origin") == "http://localhost:5173"
    bad = client.get("/api/health", headers={"Origin": "http://evil.example"})
    assert "access-control-allow-origin" not in bad.headers


def test_openapi_documents_every_documented_endpoint(client):
    paths = client.get("/openapi.json").json()["paths"]
    for p in ("/api/health", "/api/stats", "/api/records", "/api/records/{key}", "/api/query", "/api/query/compare",
              "/api/index/tree", "/api/index/validate", "/api/benchmark", "/api/benchmark/{job_id}",
              "/api/benchmark/{job_id}/results"):
        assert p in paths, p


def test_spa_fallback_serves_index_but_api_404_stays_json(tmp_path):
    from fastapi.testclient import TestClient

    from app.main import create_app
    from tests.conftest import make_settings

    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "index.html").write_text("<!doctype html><title>spa</title>")
    with TestClient(create_app(make_settings(tmp_path, frontend_dist=dist))) as c:
        r = c.get("/query")  # client-side route, deep link
        assert r.status_code == 200 and "spa" in r.text
        r = c.get("/api/does-not-exist")
        assert r.status_code == 404 and r.headers["content-type"].startswith("application/json")
        assert c.get("/api/health").status_code == 200
