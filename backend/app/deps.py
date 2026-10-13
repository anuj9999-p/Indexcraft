"""Shared application state and FastAPI dependencies."""
from __future__ import annotations

import time
from dataclasses import dataclass, field

from fastapi import HTTPException, Request

from .config import Settings
from .engine_bridge import Engine, EngineError, EngineUnavailable
from .jobs import JobManager
from .ops_log import OpsLog

# Engine status code -> HTTP status
HTTP_FOR_CODE = {
    "INVALID_ARGUMENT": 400,
    "SYNTAX_ERROR": 400,
    "UNSUPPORTED": 400,
    "DUPLICATE_KEY": 409,
    "NOT_FOUND": 404,
    "CORRUPT": 500,
    "IO_ERROR": 500,
    "INTERNAL": 500,
}


@dataclass
class AppState:
    settings: Settings
    ops: OpsLog
    jobs: JobManager
    engine: Engine | None = None
    engine_error: str | None = None
    started: float = field(default_factory=time.monotonic)


def get_state(request: Request) -> AppState:
    return request.app.state.ic


def get_engine(request: Request) -> Engine:
    state: AppState = request.app.state.ic
    if state.engine is None:
        raise HTTPException(
            status_code=503,
            detail={"code": "ENGINE_UNAVAILABLE", "message": state.engine_error or "storage engine is not running"},
        )
    return state.engine


def raise_http(state: AppState, op: str, exc: EngineError) -> None:
    """Record the engine error and convert it to an HTTPException."""
    state.ops.error(op, exc.code, exc.message)
    raise HTTPException(
        status_code=HTTP_FOR_CODE.get(exc.code, 500), detail={"code": exc.code, "message": exc.message}
    ) from exc


__all__ = ["AppState", "get_state", "get_engine", "raise_http", "EngineUnavailable", "EngineError"]
