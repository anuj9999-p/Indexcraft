"""Typed request and response models for the REST API."""
from __future__ import annotations

import re
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

from .config import MAX_SAFE_KEY

Key = int


def _check_key(v: int) -> int:
    if abs(v) > MAX_SAFE_KEY:
        raise ValueError(
            f"key must be within ±{MAX_SAFE_KEY} (the engine stores full int64, but the dashboard API "
            "limits keys to the range JavaScript can represent exactly)"
        )
    return v


# ------------------------------------------------------------------ requests
class RecordIn(BaseModel):
    key: int
    payload: str = Field(max_length=65536)

    _key_ok = field_validator("key")(_check_key)


class SampleRequest(BaseModel):
    count: int = Field(ge=1, le=5000)
    start_key: int | None = None  # default: after the current largest key

    @field_validator("start_key")
    @classmethod
    def _start(cls, v: int | None) -> int | None:
        return None if v is None else _check_key(v)


_QUOTED = re.compile(r"'(?:[^']|'')*'|\"(?:[^\"]|\"\")*\"")
_BIG_INT = re.compile(r"-?\d{16,}")


def _reject_unsafe_integers(sql: str) -> str:
    """Integer literals outside ±(2^53-1) would lose precision in the browser."""
    for m in _BIG_INT.finditer(_QUOTED.sub("''", sql)):
        if abs(int(m.group())) > MAX_SAFE_KEY:
            raise ValueError(
                f"integer literal {m.group()[:24]} is outside ±{MAX_SAFE_KEY}; the dashboard API only "
                "accepts integers JavaScript can represent exactly"
            )
    return sql


class QueryRequest(BaseModel):
    sql: str = Field(min_length=1, max_length=10000)
    force_path: Literal["auto", "scan", "index"] = "auto"
    max_rows: int = Field(default=1000, ge=1, le=5000)

    _sql_ok = field_validator("sql")(_reject_unsafe_integers)


class CompareRequest(BaseModel):
    sql: str = Field(min_length=1, max_length=10000)
    max_rows: int = Field(default=1000, ge=1, le=5000)

    _sql_ok = field_validator("sql")(_reject_unsafe_integers)


class BenchmarkRequest(BaseModel):
    sizes: list[int] = Field(default_factory=lambda: [10000], min_length=1, max_length=3)
    reps: int = Field(default=1000, ge=1, le=100000)
    scan_reps: int = Field(default=20, ge=1, le=200)
    range_reps: int = Field(default=20, ge=1, le=1000)
    heavy_reps: int = Field(default=5, ge=1, le=100)
    warmup: int = Field(default=3, ge=0, le=100)
    max_keys: int = Field(default=64, ge=3, le=512)
    seed: int = Field(default=42, ge=0, le=2**32)

    @field_validator("sizes")
    @classmethod
    def _sizes(cls, v: list[int]) -> list[int]:
        for s in v:
            if s < 1000:
                raise ValueError("each dataset size must be at least 1000 records")
        if len(set(v)) != len(v):
            raise ValueError("dataset sizes must be distinct")
        return v


# ------------------------------------------------------------------ responses
class RecordOut(BaseModel):
    key: int
    rid: int
    payload: str


class RecordList(BaseModel):
    total: int
    offset: int
    limit: int
    records: list[RecordOut]


class InsertResult(BaseModel):
    key: int
    rid: int
    offset: int


class DeleteResult(BaseModel):
    key: int
    deleted: bool


class ErrorBody(BaseModel):
    code: str
    message: str


class QueryRow(BaseModel):
    key: int
    rid: int
    payload: str


class QueryResponse(BaseModel):
    model_config = ConfigDict(extra="allow")
    ok: bool
    statement: str | None
    access_path: str
    path_forced: bool
    executed: bool
    rows: list[QueryRow]
    row_count: int
    rows_matched: int
    has_more: bool
    rows_affected: int
    records_examined: int
    index_lookups: int
    index_nodes_visited: int
    heap_reads: int
    parse_ns: int
    exec_ns: int
    total_ns: int
    duration_ms: float
    plan: list[str]
    message: str


class CompareResponse(BaseModel):
    ok: bool
    results_match: bool
    mismatch: str
    speedup: float
    scan: QueryResponse
    index: QueryResponse


class StatsResponse(BaseModel):
    model_config = ConfigDict(extra="allow")
    data_file: str
    record_count: int
    file_size_bytes: int
    deleted_slots: int
    next_record_id: int
    index_height: int
    index_key_count: int
    index_node_count: int
    index_leaf_count: int
    index_internal_count: int
    index_max_keys: int
    open_rebuild_ms: float
    index_events: dict[str, int]


class HealthResponse(BaseModel):
    status: Literal["ok", "degraded"]
    engine_loaded: bool
    engine_open: bool
    data_file: str
    error: str | None = None
    api_version: str
    uptime_seconds: float


class JobStatus(BaseModel):
    job_id: str
    status: Literal["queued", "running", "completed", "failed", "cancelled"]
    created_at: str
    started_at: str | None = None
    finished_at: str | None = None
    progress_percent: float = 0.0
    stage: str = ""
    current_size: int | None = None
    params: dict[str, Any]
    error: str | None = None
    results_available: bool = False
