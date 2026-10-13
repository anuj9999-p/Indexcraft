import random

from fastapi import APIRouter, Depends, Path, Query
from fastapi.responses import JSONResponse

from ..config import MAX_SAFE_KEY
from ..deps import AppState, get_engine, get_state, raise_http
from ..engine_bridge import Engine, EngineError
from ..schemas import DeleteResult, InsertResult, RecordIn, RecordList, RecordOut, SampleRequest

router = APIRouter(prefix="/api/records", tags=["records"])

_WORDS = ("alpha beta gamma delta index btree leaf node page heap scan range lookup split merge "
          "storage engine record key query plan cache latency throughput").split()


@router.get("", response_model=RecordList)
def list_records(
    offset: int = Query(0, ge=0, le=10**9),
    limit: int = Query(25, ge=1, le=200),
    engine: Engine = Depends(get_engine),
    state: AppState = Depends(get_state),
) -> dict:
    """Records in ascending key order, read through the B+ Tree and the heap file."""
    try:
        data = engine.list(offset, limit)
    except EngineError as e:
        raise_http(state, "list", e)
    return {"total": data["total"], "offset": offset, "limit": limit, "records": data["records"]}


@router.post("", response_model=InsertResult, status_code=201)
def insert_record(body: RecordIn, engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    if len(body.payload.encode("utf-8")) > state.settings.max_payload_bytes:
        raise_http(state, "insert", EngineError("INVALID_ARGUMENT", f"payload exceeds {state.settings.max_payload_bytes} bytes"))
    try:
        res = engine.insert(body.key, body.payload)
    except EngineError as e:
        state.ops.operation("INSERT", f"key={body.key}", False)
        raise_http(state, "insert", e)
    state.ops.operation("INSERT", f"key={body.key}", True)
    return res


@router.post("/sample")
def generate_samples(body: SampleRequest, engine: Engine = Depends(get_engine), state: AppState = Depends(get_state)) -> dict:
    """Insert generated sample records (real inserts through the engine)."""
    try:
        start = body.start_key
        if start is None:
            total = engine.stats()["record_count"]
            start = (engine.list(total - 1, 1)["records"][0]["key"] + 1) if total else 1
        if start + body.count - 1 > MAX_SAFE_KEY:
            raise EngineError("INVALID_ARGUMENT", "sample keys would exceed the supported key range")
        rng = random.Random(start)
        inserted = skipped = 0
        for key in range(start, start + body.count):
            payload = f"sample-{key}: " + " ".join(rng.choice(_WORDS) for _ in range(rng.randint(3, 10)))
            try:
                engine.insert(key, payload)
                inserted += 1
            except EngineError as e:
                if e.code != "DUPLICATE_KEY":
                    raise
                skipped += 1
    except EngineError as e:
        raise_http(state, "sample", e)
    state.ops.operation("SAMPLE", f"{inserted} records from key {start}", True)
    return {"inserted": inserted, "skipped_duplicates": skipped, "first_key": start, "last_key": start + body.count - 1}


@router.get("/{key}", response_model=RecordOut)
def get_record(
    key: int = Path(ge=-MAX_SAFE_KEY, le=MAX_SAFE_KEY),
    engine: Engine = Depends(get_engine),
    state: AppState = Depends(get_state),
) -> dict:
    try:
        return engine.get(key)
    except EngineError as e:
        raise_http(state, "get", e)


@router.delete("/{key}", response_model=DeleteResult)
def delete_record(
    key: int = Path(ge=-MAX_SAFE_KEY, le=MAX_SAFE_KEY),
    engine: Engine = Depends(get_engine),
    state: AppState = Depends(get_state),
) -> dict:
    try:
        engine.delete(key)
    except EngineError as e:
        state.ops.operation("DELETE", f"key={key}", False)
        raise_http(state, "delete", e)
    state.ops.operation("DELETE", f"key={key}", True)
    return {"key": key, "deleted": True}
