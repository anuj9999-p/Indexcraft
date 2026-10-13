#!/usr/bin/env bash
# Run the GoogleTest suite. Writes JSON to $1 if given (used by run_all_tests.sh).
source "$(dirname "$0")/_common.sh"
BIN="$(find "$BUILD_DIR" -name indexcraft_tests -type f | head -1)"
[ -n "$BIN" ] || { echo "indexcraft_tests not built; run scripts/build_engine.sh"; exit 1; }
if [ -n "${1:-}" ]; then "$BIN" --gtest_output=json:"$1"; else "$BIN"; fi
