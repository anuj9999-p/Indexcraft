#!/usr/bin/env bash
# Insert N synthetic sample records (default 1000). Stop the API first: the file is locked while it runs.
source "$(dirname "$0")/_common.sh"
CLI="$(find "$BUILD_DIR" -name indexcraft_cli -type f | head -1)"
DB="${INDEXCRAFT_DATA_DIR:-$ROOT/data}/indexcraft.db"
"$CLI" generate --db "$DB" --count "${1:-1000}"
