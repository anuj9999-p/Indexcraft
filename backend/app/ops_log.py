"""In-memory ring buffers for recent operations and recent engine errors (shown in the UI).

These are diagnostics only; they are not persisted and are lost when the API restarts.
"""
from __future__ import annotations

import threading
from collections import deque
from datetime import datetime, timezone
from typing import Any


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class OpsLog:
    def __init__(self, size: int = 200):
        self._lock = threading.Lock()
        self._ops: deque[dict[str, Any]] = deque(maxlen=size)
        self._errors: deque[dict[str, Any]] = deque(maxlen=size)

    def operation(self, op: str, detail: str, ok: bool, duration_ms: float | None = None) -> None:
        with self._lock:
            self._ops.appendleft(
                {"time": _now(), "operation": op, "detail": detail[:200], "ok": ok, "duration_ms": duration_ms}
            )

    def error(self, op: str, code: str, message: str) -> None:
        with self._lock:
            self._errors.appendleft({"time": _now(), "operation": op, "code": code, "message": message[:500]})

    def operations(self, limit: int = 50) -> list[dict[str, Any]]:
        with self._lock:
            return list(self._ops)[:limit]

    def errors(self, limit: int = 50) -> list[dict[str, Any]]:
        with self._lock:
            return list(self._errors)[:limit]
