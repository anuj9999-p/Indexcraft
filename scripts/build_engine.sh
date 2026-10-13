#!/usr/bin/env bash
# Configure and build the C++ engine (library, CLI, benchmark runner, tests).
source "$(dirname "$0")/_common.sh"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" ${CMAKE_EXTRA_ARGS:-}
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo "Built: $BUILD_DIR"
