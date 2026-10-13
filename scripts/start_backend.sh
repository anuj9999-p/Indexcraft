#!/usr/bin/env bash
# Start the FastAPI backend on 127.0.0.1:8000 (serves frontend/dist too if built).
source "$(dirname "$0")/_common.sh"
cd "$ROOT/backend"
exec python3 -m uvicorn app.main:create_app --factory --host "${HOST:-127.0.0.1}" --port "${PORT:-8000}"
