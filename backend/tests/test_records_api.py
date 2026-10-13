import pytest


def test_health_ok_on_a_fresh_store(client):
    r = client.get("/api/health")
    assert r.status_code == 200
    body = r.json()
    assert body["status"] == "ok" and body["engine_open"] and body["engine_loaded"]
    assert body["data_file"] == "indexcraft.db"


def test_stats_of_an_empty_store(client):
    s = client.get("/api/stats").json()
    assert s["record_count"] == 0 and s["index_key_count"] == 0 and s["index_height"] == 1
    assert s["file_size_bytes"] == 64  # just the file header


def test_insert_get_list_delete_round_trip(client):
    r = client.post("/api/records", json={"key": 42, "payload": "forty-two"})
    assert r.status_code == 201
    assert r.json()["key"] == 42 and r.json()["rid"] == 1
    assert client.get("/api/records/42").json() == {"key": 42, "rid": 1, "payload": "forty-two"}
    lst = client.get("/api/records").json()
    assert lst["total"] == 1 and lst["records"][0]["payload"] == "forty-two"
    assert client.get("/api/stats").json()["record_count"] == 1
    d = client.delete("/api/records/42")
    assert d.status_code == 200 and d.json() == {"key": 42, "deleted": True}
    assert client.get("/api/records/42").status_code == 404
    assert client.get("/api/stats").json()["record_count"] == 0


def test_duplicate_key_is_409_and_does_not_change_the_store(client):
    client.post("/api/records", json={"key": 1, "payload": "a"})
    size = client.get("/api/stats").json()["file_size_bytes"]
    r = client.post("/api/records", json={"key": 1, "payload": "b"})
    assert r.status_code == 409
    assert r.json()["detail"]["code"] == "DUPLICATE_KEY"
    assert client.get("/api/stats").json()["file_size_bytes"] == size
    assert client.get("/api/records/1").json()["payload"] == "a"


def test_missing_key_is_404(client):
    r = client.get("/api/records/999")
    assert r.status_code == 404 and r.json()["detail"]["code"] == "NOT_FOUND"
    r = client.delete("/api/records/999")
    assert r.status_code == 404 and r.json()["detail"]["code"] == "NOT_FOUND"


@pytest.mark.parametrize("body", [
    {"key": "abc", "payload": "x"},
    {"key": 1.5, "payload": "x"},
    {"key": 1},
    {"payload": "x"},
    {"key": 2**53, "payload": "x"},
    {"key": -(2**53), "payload": "x"},
    {"key": 1, "payload": "x" * 70000},
    {"key": 1, "payload": 123},
    {},
])
def test_invalid_insert_bodies_are_422(client, body):
    assert client.post("/api/records", json=body).status_code == 422
    assert client.get("/api/stats").json()["record_count"] == 0


def test_not_json_body_is_422(client):
    r = client.post("/api/records", content=b"not json", headers={"Content-Type": "application/json"})
    assert r.status_code == 422


def test_key_path_validation(client):
    assert client.get("/api/records/abc").status_code == 422
    assert client.get(f"/api/records/{2**60}").status_code == 422
    assert client.delete("/api/records/1.5").status_code == 422


def test_safe_integer_boundaries_are_accepted(client):
    for k in (2**53 - 1, -(2**53 - 1), 0):
        assert client.post("/api/records", json={"key": k, "payload": "edge"}).status_code == 201
        assert client.get(f"/api/records/{k}").json()["key"] == k


def test_unicode_quotes_and_newlines_round_trip(client):
    payload = "café ✓ \"quoted\" it's\nsecond line\ttab"
    client.post("/api/records", json={"key": 5, "payload": payload})
    assert client.get("/api/records/5").json()["payload"] == payload


def test_pagination_returns_key_order_and_validates_params(loaded):
    page = loaded.get("/api/records", params={"offset": 20, "limit": 10}).json()
    assert page["total"] == 200 and page["offset"] == 20
    assert [r["key"] for r in page["records"]] == list(range(21, 31))
    last = loaded.get("/api/records", params={"offset": 195, "limit": 50}).json()
    assert [r["key"] for r in last["records"]] == list(range(196, 201))
    assert loaded.get("/api/records", params={"offset": 500, "limit": 10}).json()["records"] == []
    assert loaded.get("/api/records", params={"limit": 0}).status_code == 422
    assert loaded.get("/api/records", params={"limit": 201}).status_code == 422
    assert loaded.get("/api/records", params={"offset": -1}).status_code == 422


def test_sample_generation_inserts_real_records(client):
    r = client.post("/api/records/sample", json={"count": 50})
    assert r.status_code == 200
    assert r.json() == {"inserted": 50, "skipped_duplicates": 0, "first_key": 1, "last_key": 50}
    r2 = client.post("/api/records/sample", json={"count": 10})
    assert r2.json()["first_key"] == 51  # continues after the largest key
    assert client.get("/api/stats").json()["record_count"] == 60
    assert client.post("/api/records/sample", json={"count": 0}).status_code == 422
    assert client.post("/api/records/sample", json={"count": 5001}).status_code == 422


def test_data_persists_across_an_api_restart(settings):
    from fastapi.testclient import TestClient
    from app.main import create_app

    with TestClient(create_app(settings)) as c1:
        for k in (3, 1, 2):
            c1.post("/api/records", json={"key": k, "payload": f"v{k}"})
        c1.delete("/api/records/2")
    with TestClient(create_app(settings)) as c2:  # new process-equivalent: index rebuilt from the heap file
        assert c2.get("/api/stats").json()["record_count"] == 2
        assert c2.get("/api/records/3").json()["payload"] == "v3"
        assert c2.get("/api/records/2").status_code == 404
        assert c2.get("/api/index/consistency").json()["consistent"] is True


def test_clients_cannot_choose_filesystem_paths(client, settings):
    # No endpoint accepts a path; extra fields are ignored and the data file location stays fixed.
    r = client.post("/api/records", json={"key": 1, "payload": "x", "path": "/etc/passwd", "data_dir": "/tmp"})
    assert r.status_code == 201
    assert [p.name for p in settings.data_dir.iterdir()] == ["indexcraft.db"]
