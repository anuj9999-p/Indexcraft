#!/usr/bin/env bash
# Runs C++ and API tests and records real results in docs/test-results/latest.json.
source "$(dirname "$0")/_common.sh"
OUT="$ROOT/docs/test-results"; mkdir -p "$OUT"
cpp_rc=0; api_rc=0
s=$(date +%s.%N); "$ROOT/scripts/run_cpp_tests.sh" "$OUT/cpp.json" | tee "$OUT/cpp.log" || cpp_rc=$?; e=$(date +%s.%N)
CPP_SECS=$(echo "$e - $s" | bc)
s=$(date +%s.%N); "$ROOT/scripts/run_api_tests.sh" "$OUT/api.xml" | tee "$OUT/api.log" || api_rc=$?; e=$(date +%s.%N)
API_SECS=$(echo "$e - $s" | bc)
python3 -I "$ROOT/scripts/summarize_tests.py" "$OUT" "$CPP_SECS" "$API_SECS"
echo "C++ rc=$cpp_rc, API rc=$api_rc"
exit $(( cpp_rc || api_rc ))
