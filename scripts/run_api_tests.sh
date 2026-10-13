#!/usr/bin/env bash
# Run the pytest suite. Optional $1: junit xml output path.
source "$(dirname "$0")/_common.sh"
cd "$ROOT/backend"
if [ -n "${1:-}" ]; then python3 -m pytest -q --junitxml="$1"; else python3 -m pytest -q; fi
