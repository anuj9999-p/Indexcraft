from fastapi import APIRouter, Depends

from ..deps import AppState, get_engine, get_state, raise_http
from ..engine_bridge import Engine, EngineError
from ..schemas import HealthResponse, StatsResponse
import time

router = APIRouter(prefix="/api", tags=["status"])
API_VERSION = "1.0.0"


@router.get("/health", response_model=HealthResponse)
def health(state: AppState = Depends(get_state)) -> HealthResponse:
    """Always answers 200; `status` is "degraded" when the storage engine is not running."""
    ok = state.engine is not None
    return HealthResponse(
        status="ok" if ok else "degraded",
        engine_loaded=state.settings.lib_path.exists(),
        engine_open=ok,
        data_file=state.settings.data_file.name,
        error=None if ok else state.engine_error,
        api_version=API_VERSION,
        uptime_seconds=round(time.monotonic() - state.started, 1),
    )


@router.get("/stats", response_model=StatsResponse)
def stats(engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    try:
        return engine.stats()
    except EngineError as e:
        raise_http(state, "stats", e)
