"""Builds docs/test-results/latest.json from gtest JSON and pytest JUnit XML output."""
import json, sys, datetime, xml.etree.ElementTree as ET
from pathlib import Path

out = Path(sys.argv[1]); cpp_s = float(sys.argv[2]); api_s = float(sys.argv[3])
g = json.loads((out / "cpp.json").read_text())
cpp_fail = []
for ts in g.get("testsuites", []):
    for t in ts.get("testsuite", []):
        if t.get("failures"):
            cpp_fail.append(f'{ts["name"]}.{t["name"]}')
cpp = dict(name="C++ engine (B+ Tree, heap file, database, query)", framework="GoogleTest",
           total=g["tests"], failed=g["failures"] + g.get("errors", 0), skipped=g.get("disabled", 0),
           duration_seconds=cpp_s, failures=cpp_fail)
cpp["passed"] = cpp["total"] - cpp["failed"] - cpp["skipped"]

root = ET.parse(out / "api.xml").getroot()
suite = root if root.tag == "testsuite" else root.find("testsuite")
total = int(suite.get("tests")); failed = int(suite.get("failures")) + int(suite.get("errors")); skipped = int(suite.get("skipped", 0))
api_fail = [f'{c.get("classname")}.{c.get("name")}' for c in suite.iter("testcase") if c.find("failure") is not None or c.find("error") is not None]
api = dict(name="FastAPI backend (records, query, index, benchmark, diagnostics)", framework="pytest",
           total=total, passed=total - failed - skipped, failed=failed, skipped=skipped, duration_seconds=api_s, failures=api_fail)
(out / "latest.json").write_text(json.dumps(dict(
    generated_at=datetime.datetime.now(datetime.timezone.utc).isoformat(), suites=[cpp, api]), indent=2))
print("wrote", out / "latest.json")
