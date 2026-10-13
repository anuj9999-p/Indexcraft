"""Runtime configuration, read from environment variables (see .env.example).

The API never accepts a filesystem path from a request: the data file location is fixed
here, so clients cannot make the engine open or create arbitrary files.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_FILE_NAME = "indexcraft.db"  # fixed name inside the data directory
MAX_SAFE_KEY = 2**53 - 1  # JSON/JavaScript numbers are exact only up to this magnitude


@dataclass(frozen=True)
class Settings:
    data_dir: Path
    lib_path: Path
    bench_bin: Path
    results_dir: Path
    test_results_file: Path
    max_keys: int
    cors_origins: tuple[str, ...]
    max_benchmark_size: int
    max_payload_bytes: int
    frontend_dist: Path

    @property
    def data_file(self) -> Path:
        return self.data_dir / DATA_FILE_NAME

    @classmethod
    def from_env(cls) -> "Settings":
        env = os.environ
        build_dir = Path(env.get("INDEXCRAFT_BUILD_DIR", REPO_ROOT / "build"))
        origins = env.get("INDEXCRAFT_CORS_ORIGINS", "http://localhost:5173,http://127.0.0.1:5173")
        return cls(
            data_dir=Path(env.get("INDEXCRAFT_DATA_DIR", REPO_ROOT / "data")).resolve(),
            lib_path=Path(env.get("INDEXCRAFT_LIB", build_dir / "engine" / "libindexcraft.so")),
            bench_bin=Path(env.get("INDEXCRAFT_BENCH_BIN", build_dir / "engine" / "indexcraft_bench")),
            results_dir=Path(env.get("INDEXCRAFT_RESULTS_DIR", REPO_ROOT / "benchmarks" / "results")).resolve(),
            test_results_file=Path(
                env.get("INDEXCRAFT_TEST_RESULTS", REPO_ROOT / "docs" / "test-results" / "latest.json")
            ),
            max_keys=int(env.get("INDEXCRAFT_MAX_KEYS", "64")),
            cors_origins=tuple(o.strip() for o in origins.split(",") if o.strip()),
            max_benchmark_size=int(env.get("INDEXCRAFT_MAX_BENCH_SIZE", "1000000")),
            max_payload_bytes=int(env.get("INDEXCRAFT_MAX_PAYLOAD_BYTES", str(64 * 1024))),
            frontend_dist=Path(env.get("INDEXCRAFT_FRONTEND_DIST", REPO_ROOT / "frontend" / "dist")),
        )
