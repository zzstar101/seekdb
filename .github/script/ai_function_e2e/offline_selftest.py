"""Offline test of run_ai_e2e.py step machinery using a fake cursor (local only)."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import run_ai_e2e as runner  # noqa: E402


class FakeCursor:
    def __init__(self, rows):
        self.rows = rows

    def execute(self, sql):
        if sql.startswith("boom"):
            raise RuntimeError(1064, "syntax error")

    def fetchall(self):
        return self.rows


def check(name, step, rows, expect_ok, expect_problem_contains=None):
    ok, problem = runner.run_step(FakeCursor(rows), step)
    status = "ok" if (ok == expect_ok) else "MISMATCH"
    detail = "" if expect_problem_contains is None else (
        " ok" if expect_problem_contains in (problem or "") else " MISSING-DETAIL")
    print("%-28s -> ok=%s problem=%r %s%s" % (name, ok, problem, status, detail))
    return status == "ok" and detail.strip() in ("ok", "")


def main():
    results = []

    results.append(check("json match", ("check", {
        "name": "j", "sql": "select 1",
        "expect": {"json": {"args": [[1, True], {"k": 2}], "template": "{0}/{0}/{1}"}}}),
        [[json.dumps({"args": [[1, True], {"k": 2}], "template": "{0}/{0}/{1}"})]], True))

    results.append(check("json mismatch", ("check", {
        "name": "j", "sql": "select 1",
        "expect": {"json": {"args": ["lob"], "template": "{0}-ok"}}}),
        [[json.dumps({"args": ["bob"], "template": "{0}-ok"})]], False, "!="))

    results.append(check("rows with NULL", ("check", {
        "name": "r", "sql": "select 1", "expect": {"rows": [[1, 768], [2, None], [3, 768]]}}),
        [(1, 768), (2, None), (3, 768)], True))

    results.append(check("rows NULL mismatch", ("check", {
        "name": "r", "sql": "select 1", "expect": {"rows": [[1, 768], [2, None], [3, 768]]}}),
        [(1, 768), (2, 768), (3, 768)], False, "expected NULL"))

    results.append(check("rows as strings", ("check", {
        "name": "r", "sql": "select 1", "expect": {"rows": [[1]]}}),
        [("1",)], True))

    results.append(check("rows wrong count", ("check", {
        "name": "r", "sql": "select 1", "expect": {"rows": [[1]]}}),
        [(1,), (2,)], False, "row count"))

    results.append(check("tolerated drop", ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('x')",
                                            runner.TOLERATED_DROP_ERRORS), [], True))

    class BoomCursor(FakeCursor):
        def execute(self, sql):
            raise RuntimeError(11118, "AI model does not exist")

    ok, problem = runner.run_step(BoomCursor([]), ("sql", "call x", runner.TOLERATED_DROP_ERRORS))
    print("%-28s -> ok=%s problem=%r" % ("tolerated errno", ok, problem))
    results.append(ok)

    ok, problem = runner.run_step(BoomCursor([]), ("sql", "call y", (0,)))
    print("%-28s -> ok=%s problem=%r" % ("untolerated errno", ok, problem))
    results.append(not ok)

    results.append(check("normalise decimals", ("check", {
        "name": "r", "sql": "select 1", "expect": {"rows": [["a", 2]]}}),
        [("a", "2")], True))

    print("OFFLINE", "OK" if all(results) else "FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
