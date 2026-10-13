import json
import platform
import sys

from fastapi import APIRouter, Depends

from ..deps import AppState, get_state
from ..engine_bridge import EngineError

router = APIRouter(prefix="/api/diagnostics", tags=["diagnostics"])

SYNTAX = {
    "statements": [
        {"name": "INSERT", "syntax": "INSERT INTO records (key, payload) VALUES (<integer>, '<text>')",
         "notes": "Columns may be given in either order. Duplicate keys are rejected."},
        {"name": "SELECT (point)", "syntax": "SELECT * FROM records WHERE key = <integer>",
         "notes": "Uses the B+ Tree (INDEX_POINT)."},
        {"name": "SELECT (range)", "syntax": "SELECT * FROM records WHERE key >= <a> AND key <= <b> [LIMIT <n>]",
         "notes": "Also supports <, >, and key BETWEEN a AND b. INDEX_RANGE when the estimated selectivity is at "
                  "most 25%, otherwise FULL_SCAN."},
        {"name": "SELECT (all)", "syntax": "SELECT * FROM records [LIMIT <n>]", "notes": "FULL_SCAN."},
        {"name": "DELETE", "syntax": "DELETE FROM records WHERE key = <integer>",
         "notes": "Equality only. Deleting a missing key affects 0 rows."},
        {"name": "EXPLAIN", "syntax": "EXPLAIN <INSERT | SELECT | DELETE ...>", "notes": "Shows the plan without executing."},
    ],
    "rules": [
        "Keywords are case-insensitive; one statement per request; a trailing ';' is optional.",
        "Strings use '...' or \"...\"; escape a quote by doubling it ('it''s').",
        "Keys are signed integers. The dashboard API accepts keys within ±9007199254740991.",
        "SELECT results are always ordered by key ascending; LIMIT keeps the first n rows in that order.",
        "Not supported: OR, joins, projections, UPDATE, ORDER BY, GROUP BY, range DELETE, multiple statements.",
    ],
}


@router.get("/build")
def build(state: AppState = Depends(get_state)) -> dict:
    info = {"backend": {"python": sys.version.split()[0], "platform": platform.platform()}, "engine": None,
            "engine_error": state.engine_error}
    if state.engine is not None:
        try:
            info["engine"] = state.engine.build_info()
        except EngineError as e:
            info["engine_error"] = e.message
    return info


@router.get("/syntax")
def syntax() -> dict:
    return SYNTAX


@router.get("/errors")
def errors(state: AppState = Depends(get_state)) -> dict:
    """Recent errors reported by the storage engine since the API started."""
    return {"errors": state.ops.errors(100)}


@router.get("/operations")
def operations(state: AppState = Depends(get_state)) -> dict:
    """Recent operations handled by the API since it started."""
    return {"operations": state.ops.operations(50)}


@router.get("/test-results")
def test_results(state: AppState = Depends(get_state)) -> dict:
    """Results written by scripts/run_all_tests.sh. Nothing is shown unless the tests were actually run."""
    path = state.settings.test_results_file
    if not path.exists():
        return {"available": False,
                "message": "No test results recorded yet. Run scripts/run_all_tests.sh to execute the tests."}
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        return {"available": False, "message": f"Could not read {path.name}: {exc}"}
    return {"available": True, **data}
