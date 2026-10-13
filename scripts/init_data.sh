#!/usr/bin/env bash
# Create an empty data file in ./data (no-op if it already exists).
source "$(dirname "$0")/_common.sh"
CLI="$(find "$BUILD_DIR" -name indexcraft_cli -type f | head -1)"
mkdir -p "$ROOT/data"
DB="${INDEXCRAFT_DATA_DIR:-$ROOT/data}/indexcraft.db"
if [ -e "$DB" ]; then echo "exists: $DB"; else "$CLI" init --db "$DB"; fi
