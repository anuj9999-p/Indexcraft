#!/usr/bin/env bash
# Usage: run_benchmarks.sh [sizes] [extra indexcraft_bench args]   e.g. run_benchmarks.sh 10000,100000
source "$(dirname "$0")/_common.sh"
BIN="$(find "$BUILD_DIR" -name indexcraft_bench -type f | head -1)"
SIZES="${1:-10000,100000}"; shift || true
RUN="run-$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$ROOT/benchmarks/results/$RUN"
mkdir -p "$OUT"
"$BIN" --sizes "$SIZES" --out "$OUT" --run-id "$RUN" --workdir "${INDEXCRAFT_BENCH_WORKDIR:-$(mktemp -d)}" "$@"
echo "Results: $OUT"
