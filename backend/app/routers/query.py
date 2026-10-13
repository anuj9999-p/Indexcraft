from fastapi import APIRouter, Depends

from ..deps import AppState, get_engine, get_state, raise_http
from ..engine_bridge import Engine, EngineError
from ..schemas import CompareRequest, CompareResponse, QueryRequest, QueryResponse

router = APIRouter(prefix="/api/query", tags=["query"])


@router.post("", response_model=QueryResponse)
def run_query(body: QueryRequest, engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Run one statement of the SQL-like subset. `force_path` applies to SELECT only."""
    try:
        result = engine.execute(body.sql, body.force_path, body.max_rows)
    except EngineError as e:
        state.ops.operation("QUERY", body.sql, False)
        raise_http(state, "query", e)
    state.ops.operation("QUERY", body.sql, True, result["exec_ns"] / 1e6)
    return result


@router.post("/compare", response_model=CompareResponse)
def compare_query(body: CompareRequest, engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Run the same SELECT through a full scan and through the B+ Tree, and verify the rows match."""
    try:
        result = engine.compare(body.sql, body.max_rows)
    except EngineError as e:
        state.ops.operation("COMPARE", body.sql, False)
        raise_http(state, "compare", e)
    state.ops.operation("COMPARE", body.sql, True, result["index"]["exec_ns"] / 1e6)
    return result
