#!/usr/bin/env bash
# Print the paths of all recorded benchmark CSV files.
source "$(dirname "$0")/_common.sh"
find "$ROOT/benchmarks/results" -name '*.csv' | sort
