#!/usr/bin/env bash
# Vite dev server on :5173 (proxies /api to :8000). Use `npm run build` for a production bundle.
source "$(dirname "$0")/_common.sh"
cd "$ROOT/frontend" && exec npm run dev -- --host 127.0.0.1
