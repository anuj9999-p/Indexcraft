import pytest


def q(client, sql, **kw):
    return client.post("/api/query", json={"sql": sql, **kw})


def test_point_query_uses_the_index(loaded):
    r = q(loaded, "SELECT * FROM records WHERE key = 77")
    assert r.status_code == 200
    b = r.json()
    assert b["access_path"] == "INDEX_POINT" and b["row_count"] == 1
    assert b["rows"][0] == {"key": 77, "rid": 77, "payload": "payload-77"}
    assert b["records_examined"] == 1 and b["index_lookups"] == 1 and b["exec_ns"] > 0
    assert b["index_nodes_visited"] == loaded.get("/api/stats").json()["index_height"]
    assert any("INDEX_POINT" in line for line in b["plan"])


def test_forced_scan_examines_every_record_and_returns_the_same_rows(loaded):
    scan = q(loaded, "SELECT * FROM records WHERE key >= 10 AND key <= 20", force_path="scan").json()
    idx = q(loaded, "SELECT * FROM records WHERE key >= 10 AND key <= 20", force_path="index").json()
    assert scan["access_path"] == "FULL_SCAN" and scan["path_forced"] and scan["records_examined"] == 200
    assert idx["access_path"] == "INDEX_RANGE" and idx["records_examined"] == 11
    assert scan["rows"] == idx["rows"] and len(scan["rows"]) == 11


def test_automatic_path_choice(loaded):
    assert q(loaded, "SELECT * FROM records WHERE key >= 5 AND key <= 9").json()["access_path"] == "INDEX_RANGE"
    assert q(loaded, "SELECT * FROM records WHERE key >= 1 AND key <= 150").json()["access_path"] == "FULL_SCAN"
    assert q(loaded, "SELECT * FROM records").json()["access_path"] == "FULL_SCAN"


def test_limit_and_max_rows(loaded):
    b = q(loaded, "SELECT * FROM records LIMIT 5").json()
    assert [r["key"] for r in b["rows"]] == [1, 2, 3, 4, 5]
    b = q(loaded, "SELECT * FROM records WHERE key >= 1 AND key <= 100", max_rows=10).json()
    assert b["row_count"] == 10 and b["has_more"] is True


def test_insert_and_delete_through_sql(client):
    r = q(client, "INSERT INTO records (key, payload) VALUES (9, 'nine')")
    assert r.status_code == 200 and r.json()["rows_affected"] == 1
    assert q(client, "INSERT INTO records (key, payload) VALUES (9, 'again')").status_code == 409
    assert client.get("/api/records/9").json()["payload"] == "nine"
    assert q(client, "DELETE FROM records WHERE key = 9").json()["rows_affected"] == 1
    assert q(client, "DELETE FROM records WHERE key = 9").json()["rows_affected"] == 0


def test_explain_does_not_execute(loaded):
    b = q(loaded, "EXPLAIN INSERT INTO records (key, payload) VALUES (999, 'x')").json()
    assert b["executed"] is False and b["plan"]
    assert loaded.get("/api/records/999").status_code == 404


@pytest.mark.parametrize("sql,code", [
    ("SELEC * FROM records", "SYNTAX_ERROR"),
    ("SELECT * FROM", "SYNTAX_ERROR"),
    ("INSERT INTO records (key, payload) VALUES ('a', 'b')", "SYNTAX_ERROR"),
    ("SELECT * FROM records WHERE key = 'x'", "SYNTAX_ERROR"),
    ("SELECT * FROM records WHERE key = 1 OR key = 2", "UNSUPPORTED"),
    ("DELETE FROM records WHERE key >= 1", "UNSUPPORTED"),
    ("UPDATE records SET payload = 'x'", "UNSUPPORTED"),
    ("SELECT * FROM records; SELECT * FROM records", "SYNTAX_ERROR"),
])
def test_invalid_queries_are_400_with_a_meaningful_message(client, sql, code):
    r = q(client, sql)
    assert r.status_code == 400
    d = r.json()["detail"]
    assert d["code"] == code and len(d["message"]) > 10


@pytest.mark.parametrize("body", [
    {"sql": ""},
    {"sql": "x" * 10001},
    {"sql": "SELECT * FROM records", "force_path": "magic"},
    {"sql": "SELECT * FROM records", "max_rows": 0},
    {"sql": "SELECT * FROM records", "max_rows": 5001},
    {"sql": 5},
    {},
])
def test_invalid_query_requests_are_422(client, body):
    assert client.post("/api/query", json=body).status_code == 422


def test_unsafe_integer_literals_are_rejected_but_quoted_digits_are_fine(client):
    assert q(client, "SELECT * FROM records WHERE key = 9007199254740993").status_code == 422
    assert q(client, "SELECT * FROM records WHERE key >= -99999999999999999").status_code == 422
    ok = q(client, "INSERT INTO records (key, payload) VALUES (1, 'id 12345678901234567890123')")
    assert ok.status_code == 200


def test_forcing_a_path_on_a_write_is_rejected_and_changes_nothing(client):
    r = q(client, "INSERT INTO records (key, payload) VALUES (1, 'x')", force_path="scan")
    assert r.status_code == 400 and r.json()["detail"]["code"] == "UNSUPPORTED"
    assert client.get("/api/stats").json()["record_count"] == 0


def test_compare_runs_both_paths_and_verifies_equal_results(loaded):
    r = loaded.post("/api/query/compare", json={"sql": "SELECT * FROM records WHERE key >= 50 AND key <= 80"})
    assert r.status_code == 200
    b = r.json()
    assert b["results_match"] is True and b["mismatch"] == ""
    assert b["scan"]["access_path"] == "FULL_SCAN" and b["index"]["access_path"] == "INDEX_RANGE"
    assert b["scan"]["records_examined"] == 200 and b["index"]["records_examined"] == 31
    assert b["scan"]["rows"] == b["index"]["rows"] and len(b["scan"]["rows"]) == 31
    assert b["speedup"] > 0


def test_compare_rejects_non_select_and_bad_sql(client):
    r = client.post("/api/query/compare", json={"sql": "DELETE FROM records WHERE key = 1"})
    assert r.status_code == 400 and r.json()["detail"]["code"] == "UNSUPPORTED"
    assert client.post("/api/query/compare", json={"sql": "nonsense"}).status_code == 400


def test_operations_and_error_logs_record_real_activity(loaded):
    q(loaded, "SELECT * FROM records WHERE key = 5")
    q(loaded, "SELEC nonsense")
    ops = loaded.get("/api/diagnostics/operations").json()["operations"]
    assert any(o["operation"] == "QUERY" and o["ok"] for o in ops)
    assert any(o["operation"] == "QUERY" and not o["ok"] for o in ops)
    errs = loaded.get("/api/diagnostics/errors").json()["errors"]
    assert errs and errs[0]["code"] == "SYNTAX_ERROR"
