"""Fixtures: every test gets its own temporary data directory and a real engine instance."""
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import REPO_ROOT, Settings
from app.main import create_app

BUILD = REPO_ROOT / "build" / "engine"


def make_settings(tmp_path: Path, **overrides) -> Settings:
    base = dict(
        data_dir=tmp_path / "data",
        lib_path=BUILD / "libindexcraft.so",
        bench_bin=BUILD / "indexcraft_bench",
        results_dir=tmp_path / "results",
        test_results_file=tmp_path / "test-results" / "latest.json",
        max_keys=4,  # small nodes so even small datasets build multi-level trees
        cors_origins=("http://localhost:5173",),
        max_benchmark_size=100000,
        max_payload_bytes=65536,
        frontend_dist=tmp_path / "no-frontend",
    )
    base.update(overrides)
    return Settings(**base)


@pytest.fixture(scope="session", autouse=True)
def engine_is_built():
    if not (BUILD / "libindexcraft.so").exists():
        pytest.fail("C++ engine not built. Run scripts/build_engine.sh first (tests use the real engine).")


@pytest.fixture()
def settings(tmp_path):
    return make_settings(tmp_path)


@pytest.fixture()
def client(settings):
    with TestClient(create_app(settings)) as c:
        yield c


@pytest.fixture()
def loaded(client):
    """A client whose database holds keys 1..200 (multi-level tree with max_keys=4)."""
    for k in range(1, 201):
        r = client.post("/api/records", json={"key": k, "payload": f"payload-{k}"})
        assert r.status_code == 201, r.text
    return client


def wait_for_job(client, job_id: str, timeout: float = 120.0) -> dict:
    deadline = time.time() + timeout
    while time.time() < deadline:
        j = client.get(f"/api/benchmark/{job_id}").json()
        if j["status"] in ("completed", "failed", "cancelled"):
            return j
        time.sleep(0.2)
    raise AssertionError("benchmark job did not finish in time")
