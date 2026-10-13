from fastapi import APIRouter, Depends, Path, Query

from ..config import MAX_SAFE_KEY
from ..deps import AppState, get_engine, get_state, raise_http
from ..engine_bridge import Engine, EngineError

router = APIRouter(prefix="/api/index", tags=["index"])


@router.get("/tree")
def tree(max_nodes: int = Query(500, ge=1, le=5000), engine: Engine = Depends(get_engine),
         state: AppState = Depends(get_state)) -> dict:
    """The real B+ Tree structure from the engine (breadth-first, at most `max_nodes` nodes)."""
    try:
        return engine.tree(max_nodes)
    except EngineError as e:
        raise_http(state, "tree", e)


@router.get("/path/{key}")
def search_path(key: int = Path(ge=-MAX_SAFE_KEY, le=MAX_SAFE_KEY), engine: Engine = Depends(get_engine),
                state: AppState = Depends(get_state)) -> dict:
    """Node ids visited, root to leaf, when searching for `key` (found or not)."""
    try:
        return engine.search_path(key)
    except EngineError as e:
        raise_http(state, "search_path", e)


@router.get("/validate")
def validate(engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Checks every B+ Tree structural invariant."""
    try:
        return engine.validate()
    except EngineError as e:
        raise_http(state, "validate", e)


@router.get("/consistency")
def consistency(engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Cross-checks the heap file against the index in both directions (O(n))."""
    try:
        return engine.check_consistency()
    except EngineError as e:
        raise_http(state, "consistency", e)


@router.post("/rebuild")
def rebuild(engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Discards the in-memory index and rebuilds it from the live heap records."""
    try:
        res = engine.rebuild_index()
    except EngineError as e:
        raise_http(state, "rebuild", e)
    state.ops.operation("REBUILD_INDEX", f"{res['index_key_count']} keys", True)
    return res
