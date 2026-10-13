from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse

from ..deps import AppState, get_state
from ..jobs import JobError
from ..schemas import BenchmarkRequest, JobStatus

router = APIRouter(prefix="/api/benchmark", tags=["benchmark"])


def _http(e: JobError) -> HTTPException:
    return HTTPException(status_code=e.status_code, detail={"code": e.code, "message": e.message})


@router.post("", response_model=JobStatus, status_code=202)
def start(body: BenchmarkRequest, state: AppState = Depends(get_state)) -> dict:
    """Start a benchmark in the background. It runs on its own temporary data, not the live database."""
    try:
        job = state.jobs.start(body)
    except JobError as e:
        raise _http(e) from e
    state.ops.operation("BENCHMARK", f"sizes={body.sizes}", True)
    return job


@router.get("", response_model=list[JobStatus])
def list_jobs(state: AppState = Depends(get_state)) -> list[dict]:
    return state.jobs.list()


@router.get("/{job_id}", response_model=JobStatus)
def status(job_id: str, state: AppState = Depends(get_state)) -> dict:
    try:
        return state.jobs.get(job_id)
    except JobError as e:
        raise _http(e) from e


@router.delete("/{job_id}", response_model=JobStatus)
def cancel(job_id: str, state: AppState = Depends(get_state)) -> dict:
    try:
        return state.jobs.cancel(job_id)
    except JobError as e:
        raise _http(e) from e


@router.get("/{job_id}/results")
def results(job_id: str, state: AppState = Depends(get_state)) -> dict:
    """Measured results of a completed job (parsed summary.csv plus run metadata)."""
    try:
        return state.jobs.results(job_id)
    except JobError as e:
        raise _http(e) from e


@router.get("/{job_id}/csv/{kind}")
def download_csv(job_id: str, kind: str, state: AppState = Depends(get_state)) -> FileResponse:
    """Download raw.csv (every measured repetition) or summary.csv."""
    try:
        path = state.jobs.csv_path(job_id, kind)
    except JobError as e:
        raise _http(e) from e
    return FileResponse(path, media_type="text/csv", filename=f"indexcraft-{job_id}-{kind}.csv")
