"""Background benchmark jobs.

A job runs the C++ `indexcraft_bench` executable as a subprocess against its own temporary
data files (never the live database), parses its JSON progress lines, and stores the output
CSV files under benchmarks/results/<job_id>/. The API stays responsive while it runs; clients
poll GET /api/benchmark/{job_id}. Only one job runs at a time.
"""
from __future__ import annotations

import csv
import json
import re
import shutil
import subprocess
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .schemas import BenchmarkRequest

JOB_ID_RE = re.compile(r"^[0-9a-f]{12}$")
JOB_TIMEOUT_SECONDS = 4 * 3600


class JobError(Exception):
    def __init__(self, status_code: int, code: str, message: str):
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class JobManager:
    def __init__(self, bench_bin: Path, results_dir: Path, max_size: int):
        self.bench_bin = bench_bin
        self.results_dir = results_dir
        self.max_size = max_size
        self._lock = threading.Lock()
        self._jobs: dict[str, dict[str, Any]] = {}
        self._procs: dict[str, subprocess.Popen] = {}
        self._cancelled: set[str] = set()
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self._load_existing()

    # ------------------------------------------------------------------ persistence
    def _job_dir(self, job_id: str) -> Path:
        if not JOB_ID_RE.match(job_id):
            raise JobError(404, "NOT_FOUND", "no such benchmark job")
        return self.results_dir / job_id

    def _save(self, job: dict[str, Any]) -> None:
        d = self.results_dir / job["job_id"]
        d.mkdir(parents=True, exist_ok=True)
        tmp = d / "job.json.tmp"
        tmp.write_text(json.dumps(job, indent=2))
        tmp.replace(d / "job.json")

    def _load_existing(self) -> None:
        for p in self.results_dir.glob("*/job.json"):
            try:
                job = json.loads(p.read_text())
                if not JOB_ID_RE.match(job.get("job_id", "")):
                    continue
            except (OSError, ValueError):
                continue
            if job["status"] in ("queued", "running"):
                job.update(status="failed", error="interrupted: the API restarted while this job was running",
                           finished_at=_now())
                self._save(job)
            self._jobs[job["job_id"]] = job

    # ------------------------------------------------------------------ public API
    def start(self, req: BenchmarkRequest) -> dict[str, Any]:
        if not self.bench_bin.exists():
            raise JobError(503, "ENGINE_UNAVAILABLE",
                           f"benchmark executable not found at {self.bench_bin}. Build it first: scripts/build_engine.sh")
        for s in req.sizes:
            if s > self.max_size:
                raise JobError(422, "SIZE_NOT_ALLOWED",
                               f"dataset size {s} exceeds the configured maximum of {self.max_size} "
                               "(INDEXCRAFT_MAX_BENCH_SIZE)")
        with self._lock:
            if any(j["status"] in ("queued", "running") for j in self._jobs.values()):
                raise JobError(409, "JOB_RUNNING", "a benchmark is already running; wait for it or cancel it")
            job_id = uuid.uuid4().hex[:12]
            job = {
                "job_id": job_id, "status": "queued", "created_at": _now(), "started_at": None,
                "finished_at": None, "progress_percent": 0.0, "stage": "queued", "current_size": None,
                "params": req.model_dump(), "error": None, "results_available": False,
            }
            self._jobs[job_id] = job
            self._save(job)
        threading.Thread(target=self._run, args=(job_id, req), daemon=True, name=f"bench-{job_id}").start()
        return dict(job)

    def get(self, job_id: str) -> dict[str, Any]:
        self._job_dir(job_id)
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                raise JobError(404, "NOT_FOUND", "no such benchmark job")
            return dict(job)

    def list(self) -> list[dict[str, Any]]:
        with self._lock:
            return sorted((dict(j) for j in self._jobs.values()), key=lambda j: j["created_at"], reverse=True)

    def cancel(self, job_id: str) -> dict[str, Any]:
        job = self.get(job_id)
        if job["status"] not in ("queued", "running"):
            raise JobError(409, "NOT_RUNNING", f"job is already {job['status']}")
        with self._lock:
            self._cancelled.add(job_id)
            proc = self._procs.get(job_id)
        if proc and proc.poll() is None:
            proc.terminate()
        return self.get(job_id)

    def csv_path(self, job_id: str, kind: str) -> Path:
        job = self.get(job_id)
        if kind not in ("summary", "raw"):
            raise JobError(422, "BAD_KIND", "kind must be 'summary' or 'raw'")
        path = self._job_dir(job_id) / f"{kind}.csv"
        if job["status"] != "completed" or not path.exists():
            raise JobError(409, "NOT_READY", "results are only available for completed jobs")
        return path

    def results(self, job_id: str) -> dict[str, Any]:
        job = self.get(job_id)
        if job["status"] != "completed":
            raise JobError(409, "NOT_READY", f"job is {job['status']}; results are only available when completed")
        d = self._job_dir(job_id)
        rows: list[dict[str, Any]] = []
        with open(d / "summary.csv", newline="") as f:
            for r in csv.DictReader(f):
                row: dict[str, Any] = {}
                for k, v in r.items():
                    if k in ("run_id", "workload", "access_path", "notes"):
                        row[k] = v
                    elif v == "":
                        row[k] = None
                    else:
                        row[k] = float(v)
                for k in ("dataset_size", "repetitions"):
                    if row.get(k) is not None:
                        row[k] = int(row[k])
                rows.append(row)
        meta = json.loads((d / "meta.json").read_text()) if (d / "meta.json").exists() else {}
        with open(d / "raw.csv", "rb") as f:
            raw_rows = max(sum(1 for _ in f) - 1, 0)
        return {"job_id": job_id, "measured": True, "meta": meta, "summary": rows, "raw_rows": raw_rows}

    # ------------------------------------------------------------------ worker
    def _update(self, job_id: str, **changes: Any) -> None:
        with self._lock:
            self._jobs[job_id].update(changes)
            self._save(self._jobs[job_id])

    def _run(self, job_id: str, req: BenchmarkRequest) -> None:
        d = self.results_dir / job_id
        cmd = [
            str(self.bench_bin), "--sizes", ",".join(str(s) for s in req.sizes), "--reps", str(req.reps),
            "--scan-reps", str(req.scan_reps), "--range-reps", str(req.range_reps), "--heavy-reps", str(req.heavy_reps),
            "--warmup", str(req.warmup), "--max-keys", str(req.max_keys), "--seed", str(req.seed),
            "--workdir", str(d / "work"), "--out", str(d), "--run-id", job_id,
        ]
        error: str | None = None
        done = False
        timer: threading.Timer | None = None
        try:
            with open(d / "stderr.log", "w") as errlog:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=errlog, text=True, bufsize=1)
                with self._lock:
                    self._procs[job_id] = proc
                self._update(job_id, status="running", started_at=_now(), stage="starting")
                timer = threading.Timer(JOB_TIMEOUT_SECONDS, proc.kill)
                timer.start()
                assert proc.stdout is not None
                for line in proc.stdout:
                    try:
                        ev = json.loads(line)
                    except ValueError:
                        continue
                    kind = ev.get("event")
                    if kind == "progress":
                        size = ev.get("dataset_size")
                        idx = req.sizes.index(size) if size in req.sizes else 0
                        pct = (idx + float(ev.get("percent", 0)) / 100.0) / len(req.sizes) * 100.0
                        self._update(job_id, progress_percent=round(pct, 1), stage=str(ev.get("stage", "")),
                                     current_size=size)
                    elif kind == "done":
                        done = True
                    elif kind == "error":
                        error = str(ev.get("message", "benchmark failed"))
                rc = proc.wait()
            if job_id in self._cancelled:
                self._update(job_id, status="cancelled", finished_at=_now(), stage="cancelled")
            elif rc == 0 and done and (d / "summary.csv").exists():
                self._update(job_id, status="completed", finished_at=_now(), progress_percent=100.0,
                             stage="completed", results_available=True)
            else:
                if error is None:
                    tail = (d / "stderr.log").read_text()[-300:].strip()
                    error = tail or f"benchmark process exited with code {rc}"
                self._update(job_id, status="failed", finished_at=_now(), error=error)
        except Exception as exc:  # noqa: BLE001 - report any unexpected failure on the job
            self._update(job_id, status="failed", finished_at=_now(), error=f"{type(exc).__name__}: {exc}")
        finally:
            if timer:
                timer.cancel()
            with self._lock:
                self._procs.pop(job_id, None)
            shutil.rmtree(d / "work", ignore_errors=True)
