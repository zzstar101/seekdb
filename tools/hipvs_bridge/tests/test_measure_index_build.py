#!/usr/bin/env python3
"""Host-only timing/validation tests; no DB, numpy, pymysql, or GPU required."""

import contextlib
import importlib.util
import io
from pathlib import Path
import re
import subprocess
import sys
import unittest


SCRIPT = Path(__file__).with_name("measure_index_build.py")
SPEC = importlib.util.spec_from_file_location("measurement", SCRIPT)
measurement = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measurement)


class FakeCursor:
    def __init__(self, connection):
        self.connection = connection
        self.rows = []

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        return False

    def execute(self, sql, values=None):
        db = self.connection
        db.sql.append(sql)
        if sql.startswith("SHOW VARIABLES"):
            self.rows = [("ob_compatibility_mode", db.mode)]
        elif sql.startswith("SELECT VERSION()"):
            self.rows = [("mock_seekdb", "STRICT_ALL_TABLES")]
        elif sql.startswith("SET SESSION"):
            db.events.append("set_timeout")
        elif sql.startswith("CREATE TABLE"):
            db.events.append("create_table")
            if db.existing_table:
                raise RuntimeError("table already exists")
        elif sql.startswith("SELECT COUNT(*)"):
            db.events.append("import_count")
            self.rows = [(len(db.imported), 0, len(db.imported) - 1)]
        elif sql.startswith("CREATE VECTOR INDEX"):
            db.events.append("create_index")
        elif sql.startswith("SELECT id"):
            db.events.append("query")
            db.query_count += 1
            topk = int(re.search(r"LIMIT (\d+)$", sql).group(1))
            self.rows = [(row_id,) for row_id in range(topk)]
            if db.bad_first is not None and db.query_count == 1:
                self.rows = db.bad_first
            if db.bad_last is not None and topk == 100:
                self.rows = db.bad_last
            assert values and isinstance(values[0], str)
        else:
            raise AssertionError("unexpected SQL: " + sql)

    def executemany(self, sql, rows):
        assert sql.startswith("INSERT INTO")
        self.connection.sql.append(sql)
        self.connection.events.append("import")
        self.connection.imported.extend(rows)

    def fetchone(self):
        return self.rows[0]

    def fetchall(self):
        self.connection.events.append("fetch_results")
        return self.rows


class FakeConnection:
    def __init__(self):
        self.events = []
        self.sql = []
        self.imported = []
        self.query_count = 0
        self.bad_first = None
        self.bad_last = None
        self.existing_table = False
        self.mode = "MYSQL"
        self.ticks = 0

    def cursor(self):
        return FakeCursor(self)

    def commit(self):
        self.events.append("commit")

    def clock(self):
        self.events.append("clock")
        self.ticks += 1
        return float(self.ticks)


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.args = measurement.parse_args([
            "--mode", "candidate", "--commit", "1" * 40, "--database", "test",
            "--table", "unique_measurement", "--n", "256", "--dim", "2",
            "--queries", "1", "--import-batch", "100",
        ])
        self.db = FakeConnection()
        self.report = measurement.initial_report(self.args)

    def run_measurement(self):
        measurement.measure(self.db, self.args, [[1.0, 2.0]] * self.args.n,
                            ["[1,2]"], [list(range(100))], self.report,
                            clock=self.db.clock)

    def test_import_precedes_timer_and_first_fetch_is_timed(self):
        self.run_measurement()
        first_tick = self.db.events.index("clock")
        self.assertEqual(self.db.events[:first_tick], [
            "set_timeout", "create_table", "import", "import", "import",
            "commit", "import_count",
        ])
        self.assertEqual(self.db.events[first_tick:first_tick + 6], [
            "clock", "create_index", "clock", "query", "fetch_results", "clock",
        ])
        self.assertEqual(self.report["build"]["create_s"], 1.0)
        self.assertEqual(self.report["build"]["first_query_s"], 1.0)
        self.assertEqual(self.report["build"]["end_to_end_s"], 2.0)
        self.assertEqual(self.db.query_count, 3)
        for topk in ("10", "100"):
            self.assertEqual(self.report["single_query"][topk]["recall"], 1.0)
            self.assertEqual(self.report["single_query"][topk]["latencies_s"], [1.0])
        self.assertTrue(any("lib=cuvs" in sql for sql in self.db.sql))
        self.assertFalse(any("DROP" in sql for sql in self.db.sql))

    def test_invalid_first_results_stop_before_more_queries(self):
        invalid = [
            [(row_id,) for row_id in range(9)],
            [(-1,)] + [(row_id,) for row_id in range(1, 10)],
            [(self.args.n,)] + [(row_id,) for row_id in range(1, 10)],
            [(0,), (0,)] + [(row_id,) for row_id in range(2, 10)],
            [(0.5,)] + [(row_id,) for row_id in range(1, 10)],
        ]
        for rows in invalid:
            with self.subTest(rows=rows):
                self.db = FakeConnection()
                self.db.bad_first = rows
                with self.assertRaises(measurement.MeasurementError):
                    self.run_measurement()
                self.assertEqual(self.db.query_count, 1)
                self.assertNotIn("build", self.report)

    def test_invalid_later_results_also_fail(self):
        self.db.bad_last = [(row_id,) for row_id in range(99)]
        with self.assertRaises(measurement.MeasurementError):
            self.run_measurement()
        self.assertEqual(self.db.query_count, 3)
        self.assertNotIn("id_validation", self.report)

    def test_existing_table_is_never_changed_or_dropped(self):
        self.db.existing_table = True
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            self.run_measurement()
        self.assertEqual(self.db.imported, [])
        self.assertEqual(self.db.ticks, 0)
        self.assertFalse(self.report["retained_table"]["created_by_this_run"])
        self.assertFalse(any("DROP" in sql for sql in self.db.sql))

    def test_cpu_uses_vsag_and_rejects_non_mysql_mode(self):
        self.args.mode = "cpu"
        self.run_measurement()
        self.assertTrue(any("lib=vsag" in sql for sql in self.db.sql))
        self.db = FakeConnection()
        self.db.mode = "ORACLE"
        with self.assertRaises(measurement.MeasurementError):
            self.run_measurement()
        self.assertNotIn("create_table", self.db.events)

    def test_cli_validation_and_dependency_free_help(self):
        required = ["--mode", "candidate", "--commit", "1" * 40, "--database", "test"]
        for extra in (["--table", "x;drop table y"], ["--n", "99"],
                      ["--commit", "1234567"], ["--mode", "invalid"],
                      ["--mode", "baseline"], ["--seed", "-1"]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    measurement.parse_args(required + extra)
        result = subprocess.run([sys.executable, "-S", str(SCRIPT), "--help"],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--commit", result.stdout)

    def test_gpu_minimum_and_cpu_minimum(self):
        required = ["--commit", "1" * 40, "--database", "test"]
        self.assertEqual(measurement.parse_args(required + ["--mode", "cpu", "--n", "100"]).n, 100)
        self.assertEqual(measurement.parse_args(required + ["--mode", "candidate", "--n", "256"]).n, 256)
        for mode, sha in (("candidate", "1" * 40), ("baseline", measurement.BASELINE_SHA)):
            with self.subTest(mode=mode), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    measurement.parse_args(required + ["--mode", mode, "--commit", sha, "--n", "255"])


if __name__ == "__main__":
    unittest.main()
