"""ctypes bridge to the C++ engine (libindexcraft.so).

Every call goes through one lock because the engine handle is not thread-safe. The engine
returns JSON text; this module parses it and turns failures into EngineError. There is no
storage or indexing logic in Python: all of it runs inside the C++ engine.
"""
from __future__ import annotations

import ctypes
import json
import threading
from pathlib import Path
from typing import Any


class EngineUnavailable(RuntimeError):
    """The shared library is missing or the database could not be opened."""


class EngineError(Exception):
    """An error reported by the engine (Status code + message)."""

    def __init__(self, code: str, message: str, result: dict[str, Any] | None = None):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.result = result


_FORCE = {"auto": 0, "scan": 1, "index": 2}


class Engine:
    def __init__(self, lib_path: Path, data_file: Path, max_keys: int):
        if not lib_path.exists():
            raise EngineUnavailable(
                f"engine library not found at {lib_path}. Build it first: scripts/build_engine.sh"
            )
        self._lock = threading.RLock()
        self._lib = ctypes.CDLL(str(lib_path))
        L = self._lib
        c_json = ctypes.POINTER(ctypes.c_void_p)
        L.ic_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p), c_json]
        L.ic_open.restype = ctypes.c_int
        L.ic_close.argtypes = [ctypes.c_void_p]
        L.ic_close.restype = None
        L.ic_free.argtypes = [ctypes.c_void_p]
        L.ic_free.restype = None
        L.ic_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_size_t, c_json]
        L.ic_compare.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, c_json]
        L.ic_insert.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_size_t, c_json]
        L.ic_delete.argtypes = [ctypes.c_void_p, ctypes.c_int64, c_json]
        L.ic_get.argtypes = [ctypes.c_void_p, ctypes.c_int64, c_json]
        L.ic_list.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, c_json]
        L.ic_stats.argtypes = [ctypes.c_void_p, c_json]
        L.ic_tree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, c_json]
        L.ic_search_path.argtypes = [ctypes.c_void_p, ctypes.c_int64, c_json]
        L.ic_validate.argtypes = [ctypes.c_void_p, c_json]
        L.ic_check_consistency.argtypes = [ctypes.c_void_p, c_json]
        L.ic_rebuild_index.argtypes = [ctypes.c_void_p, c_json]
        L.ic_build_info.argtypes = [c_json]
        for name in ("ic_execute", "ic_compare", "ic_insert", "ic_delete", "ic_get", "ic_list", "ic_stats",
                     "ic_tree", "ic_search_path", "ic_validate", "ic_check_consistency", "ic_rebuild_index",
                     "ic_build_info"):
            getattr(L, name).restype = ctypes.c_int

        self.data_file = data_file
        self._handle = ctypes.c_void_p()
        out = ctypes.c_void_p()
        data_file.parent.mkdir(parents=True, exist_ok=True)
        rc = L.ic_open(str(data_file).encode(), max_keys, ctypes.byref(self._handle), ctypes.byref(out))
        text = self._take(out)
        if rc != 0:
            err = json.loads(text).get("error", {}) if text else {}
            raise EngineUnavailable(f"{err.get('code', rc)}: {err.get('message', 'could not open the data file')}")

    # ------------------------------------------------------------------ internals
    def _take(self, out: ctypes.c_void_p) -> str:
        if not out.value:
            return ""
        try:
            return ctypes.string_at(out.value).decode("utf-8", errors="replace")
        finally:
            self._lib.ic_free(out)

    def _call(self, fn_name: str, *args: Any) -> dict[str, Any]:
        with self._lock:
            if not self._handle.value:
                raise EngineUnavailable("engine is closed")
            out = ctypes.c_void_p()
            rc = getattr(self._lib, fn_name)(self._handle, *args, ctypes.byref(out))
            text = self._take(out)
        data = json.loads(text) if text else {}
        if rc != 0 or data.get("ok") is False:
            err = data.get("error") or {}
            raise EngineError(err.get("code", "INTERNAL"), err.get("message", f"engine returned code {rc}"), data)
        return data

    # ------------------------------------------------------------------ operations
    def close(self) -> None:
        with self._lock:
            if self._handle.value:
                self._lib.ic_close(self._handle)
                self._handle = ctypes.c_void_p()

    def execute(self, sql: str, force: str = "auto", max_rows: int = 1000) -> dict[str, Any]:
        return self._call("ic_execute", sql.encode("utf-8"), _FORCE[force], max_rows)

    def compare(self, sql: str, max_rows: int = 1000) -> dict[str, Any]:
        return self._call("ic_compare", sql.encode("utf-8"), max_rows)

    def insert(self, key: int, payload: str) -> dict[str, Any]:
        raw = payload.encode("utf-8")
        return self._call("ic_insert", key, raw, len(raw))

    def delete(self, key: int) -> dict[str, Any]:
        return self._call("ic_delete", key)

    def get(self, key: int) -> dict[str, Any]:
        return self._call("ic_get", key)["record"]

    def list(self, offset: int, limit: int) -> dict[str, Any]:
        return self._call("ic_list", offset, limit)

    def stats(self) -> dict[str, Any]:
        return self._call("ic_stats")

    def tree(self, max_nodes: int) -> dict[str, Any]:
        return self._call("ic_tree", max_nodes)

    def search_path(self, key: int) -> dict[str, Any]:
        return self._call("ic_search_path", key)

    def validate(self) -> dict[str, Any]:
        return self._call("ic_validate")

    def check_consistency(self) -> dict[str, Any]:
        return self._call("ic_check_consistency")

    def rebuild_index(self) -> dict[str, Any]:
        return self._call("ic_rebuild_index")

    def build_info(self) -> dict[str, Any]:
        with self._lock:
            out = ctypes.c_void_p()
            self._lib.ic_build_info(ctypes.byref(out))
            return json.loads(self._take(out))
