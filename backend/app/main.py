"""IndexCraft REST API: a thin FastAPI layer over the C++ storage engine."""
from __future__ import annotations

from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from starlette.exceptions import HTTPException as StarletteHTTPException

from .config import Settings
from .deps import AppState
from .engine_bridge import Engine, EngineUnavailable
from .jobs import JobManager
from .ops_log import OpsLog
from .routers import benchmark, diagnostics, health, index, query, records


class _SPAStaticFiles(StaticFiles):
    """Static files with client-side-routing fallback: unknown non-API paths serve index.html."""

    async def get_response(self, path, scope):
        if path == "api" or path.startswith("api/"):
            raise StarletteHTTPException(status_code=404, detail="Not found")
        try:
            resp = await super().get_response(path, scope)
        except StarletteHTTPException as exc:
            if exc.status_code != 404:
                raise
            return await super().get_response("index.html", scope)
        if resp.status_code == 404:
            return await super().get_response("index.html", scope)
        return resp


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()

    state = AppState(
        settings=settings,
        ops=OpsLog(),
        jobs=JobManager(settings.bench_bin, settings.results_dir, settings.max_benchmark_size),
    )
    try:
        state.engine = Engine(settings.lib_path, settings.data_file, settings.max_keys)
    except EngineUnavailable as exc:
        state.engine_error = str(exc)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        yield
        if state.engine is not None:
            state.engine.close()

    app = FastAPI(
        title="IndexCraft API",
        version=health.API_VERSION,
        description="REST API for the IndexCraft storage engine (C++ heap file + B+ Tree index).",
        lifespan=lifespan,
    )
    app.state.ic = state
    app.add_middleware(
        CORSMiddleware,
        allow_origins=list(settings.cors_origins),
        allow_methods=["GET", "POST", "DELETE"],
        allow_headers=["Content-Type"],
    )
    for module in (health, records, query, index, benchmark, diagnostics):
        app.include_router(module.router)

    # Serve the production frontend build, if present, so one process serves everything.
    if (settings.frontend_dist / "index.html").exists():
        app.mount("/", _SPAStaticFiles(directory=settings.frontend_dist, html=True), name="frontend")
    return app


# Run with:  uvicorn app.main:create_app --factory
# (a factory keeps importing this module free of side effects: nothing is opened until the app is created)
