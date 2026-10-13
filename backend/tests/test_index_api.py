def node_map(tree):
    return {n["id"]: n for n in tree["nodes"]}


def test_tree_is_the_real_structure_of_the_engine(loaded):
    t = loaded.get("/api/index/tree", params={"max_nodes": 5000}).json()
    s = loaded.get("/api/stats").json()
    assert t["key_count"] == 200 == s["index_key_count"]
    assert t["height"] == s["index_height"] >= 4  # max_keys=4 and 200 ascending keys
    assert t["node_count"] == s["index_node_count"] == len(t["nodes"])
    assert t["truncated"] is False and t["max_keys"] == 4
    nodes = node_map(t)
    root = nodes[t["root_id"]]
    assert root["level"] == 0 and not root["leaf"]
    leaves = [n for n in t["nodes"] if n["leaf"]]
    assert len(leaves) == s["index_leaf_count"]
    # parent/child relations: every child id exists, sits one level deeper, and keys stay sorted
    for n in t["nodes"]:
        assert n["keys"] == sorted(n["keys"])
        assert len(n["keys"]) <= t["max_keys"]
        if n["leaf"]:
            assert n["children"] == []
        else:
            assert len(n["children"]) == len(n["keys"]) + 1
            for c in n["children"]:
                assert nodes[c]["level"] == n["level"] + 1
    # following leaf links from the leftmost leaf yields every key in order
    cur = next(n for n in leaves if n["keys"][0] == 1)
    seen = []
    while cur:
        seen += cur["keys"]
        cur = nodes[cur["next"]] if cur["next"] is not None else None
    assert seen == list(range(1, 201))


def test_tree_node_cap_marks_truncation(loaded):
    t = loaded.get("/api/index/tree", params={"max_nodes": 5}).json()
    assert t["truncated"] is True and len(t["nodes"]) == 5 and t["nodes_returned"] == 5
    assert loaded.get("/api/index/tree", params={"max_nodes": 0}).status_code == 422
    assert loaded.get("/api/index/tree", params={"max_nodes": 5001}).status_code == 422


def test_search_path_walks_root_to_the_leaf_holding_the_key(loaded):
    t = loaded.get("/api/index/tree", params={"max_nodes": 5000}).json()
    nodes = node_map(t)
    p = loaded.get("/api/index/path/137").json()
    assert p["found"] is True and p["node_ids"][0] == t["root_id"]
    assert len(p["node_ids"]) == t["height"]
    assert 137 in nodes[p["node_ids"][-1]]["keys"] and nodes[p["node_ids"][-1]]["leaf"]
    for parent, child in zip(p["node_ids"], p["node_ids"][1:]):
        assert child in nodes[parent]["children"]
    miss = loaded.get("/api/index/path/5000").json()
    assert miss["found"] is False and len(miss["node_ids"]) == t["height"]


def test_validation_reports_a_valid_tree_after_inserts_and_deletes(loaded):
    for k in range(1, 201, 3):
        assert loaded.delete(f"/api/records/{k}").status_code == 200
    v = loaded.get("/api/index/validate").json()
    assert v["valid"] is True and v["violations"] == []
    assert v["key_count"] == 200 - len(range(1, 201, 3))
    c = loaded.get("/api/index/consistency").json()
    assert c["consistent"] is True and c["heap_live_records"] == c["index_entries"] == v["key_count"]


def test_deletes_trigger_real_structural_events(loaded):
    for k in range(1, 201):
        loaded.delete(f"/api/records/{k}")
    ev = loaded.get("/api/stats").json()["index_events"]
    assert ev["leaf_merges"] > 0 and ev["root_shrinks"] > 0 and ev["leaf_redistributions"] > 0
    assert loaded.get("/api/stats").json()["index_height"] == 1


def test_rebuild_reproduces_the_same_logical_index(loaded):
    before = loaded.get("/api/records", params={"limit": 200}).json()
    r = loaded.post("/api/index/rebuild")
    assert r.status_code == 200 and r.json()["index_key_count"] == 200
    assert loaded.get("/api/records", params={"limit": 200}).json() == before
    assert loaded.get("/api/index/validate").json()["valid"] is True


def test_empty_tree(client):
    t = client.get("/api/index/tree").json()
    assert t["key_count"] == 0 and t["height"] == 1 and len(t["nodes"]) == 1
    assert client.get("/api/index/path/5").json()["found"] is False
