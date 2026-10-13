#!/usr/bin/env bash
# Shared helpers: resolves the repo root and the build directory.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${INDEXCRAFT_BUILD_DIR:-$ROOT/build}"
export INDEXCRAFT_BUILD_DIR="$BUILD_DIR"
