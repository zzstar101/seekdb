#!/usr/bin/env python3
"""Run the official AI-function evaluation suite against a locally built seekdb.

The evaluation queue executes
``tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test``
against a mock AI provider.  Waiting for that queue is slow and opaque, so this
script reproduces the same sequence on a machine we control:

  1. start the endpoint mock (mock_ai_endpoints.py) on port 8000,
  2. start ``seekdb`` with ``--base-dir/--port``,
  3. replay every statement of the official case, comparing only the rows the
     official ``.result`` file asserts,
  4. tear everything down and report one PASS/FAIL line per assertion.

Only the assertions are compared; DDL and cleanup statements are executed for
their side effects and their documented error codes (11118/11112 on DROP) are
tolerated so the replay keeps the official ordering.
"""

import argparse
import base64
import datetime
import decimal
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import traceback
from pathlib import Path

READY_TIMEOUT = 900
TOLERATED_DROP_ERRORS = (0, 11112, 11118)

STEPS = [
    # (kind, payload)
    # ---- AI_PROMPT: JSON arguments, repeated placeholders, LOB input ----
    ("check", {
        "name": "ai_prompt json arguments",
        "sql": "select ai_prompt('{0}/{0}/{1}', json_array(1,true), json_object('k',2))",
        "expect": {"json": {"args": [[1, True], {"k": 2}], "template": "{0}/{0}/{1}"}},
    }),
    ("sql", "drop table if exists ai_enhance_prompt"),
    ("sql", "create table ai_enhance_prompt(t longtext, a text)"),
    ("sql", "insert into ai_enhance_prompt values('{0}-ok', 'lob')"),
    ("check", {
        "name": "ai_prompt longtext + text arguments",
        "sql": "select ai_prompt(t, a) from ai_enhance_prompt",
        "expect": {"json": {"args": ["lob"], "template": "{0}-ok"}},
    }),
    ("sql", "drop table ai_enhance_prompt"),

    # ---- OpenAI-compatible completion / embedding endpoints ----
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_complete')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_complete_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('enhance_complete', "
            "'{\"type\":\"completion\",\"model_name\":\"qwen3-32b\"}')"),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enhance_complete_ep', "
            "'{\"ai_model_name\":\"enhance_complete\",\"scope\":\"all\","
            "\"url\":\"http://0.0.0.0:8000/compatible-mode/v1/chat/completions\","
            "\"access_key\":\"mock-key\",\"provider\":\"openai\"}')"),
    ("check", {
        "name": "ai_complete with NULL config",
        "sql": "select length(ai_complete('enhance_complete','hello',null)) > 0",
        "expect": {"rows": [[1]]},
    }),
    ("check", {
        "name": "ai_complete with json config",
        "sql": "select length(ai_complete('enhance_complete', "
               "ai_prompt('describe {0}', json_object('text','hello')), "
               "'{\"temperature\":0.2,\"max_tokens\":16}')) > 0",
        "expect": {"rows": [[1]]},
    }),

    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_embed')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_embed_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('enhance_embed', "
            "'{\"type\":\"dense_embedding\",\"model_name\":\"text-embedding-v1\"}')"),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enhance_embed_ep', "
            "'{\"ai_model_name\":\"enhance_embed\",\"scope\":\"all\","
            "\"url\":\"http://0.0.0.0:8000/compatible-mode/v1/embeddings\","
            "\"access_key\":\"mock-key\",\"provider\":\"openai\"}')"),
    ("check", {
        "name": "ai_embed default dimension",
        "sql": "select json_length(ai_embed('enhance_embed','single-row'))",
        "expect": {"rows": [[768]]},
    }),
    ("check", {
        "name": "ai_embed requested dimension",
        "sql": "select json_length(ai_embed('enhance_embed','dimension-row',768))",
        "expect": {"rows": [[768]]},
    }),

    # ---- batch rows: one NULL row stays NULL, no result skew ----
    ("sql", "drop table if exists ai_enhance_batch"),
    ("sql", "create table ai_enhance_batch(id int primary key, content text)"),
    ("sql", "insert into ai_enhance_batch values(1,'first'),(2,null),(3,'third')"),
    ("check", {
        "name": "ai_embed batch rows keep row mapping",
        "sql": "select id, json_length(ai_embed('enhance_embed', content)) "
               "from ai_enhance_batch order by id",
        "expect": {"rows": [[1, 768], [2, None], [3, 768]]},
    }),
    ("sql", "drop table ai_enhance_batch"),

    # ---- rerank: NULL doc key, object array with a text field ----
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_rerank')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_rerank_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('enhance_rerank', "
            "'{\"type\":\"rerank\",\"model_name\":\"BAAI/bge-reranker-v2-m3\"}')"),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enhance_rerank_ep', "
            "'{\"ai_model_name\":\"enhance_rerank\",\"scope\":\"all\","
            "\"url\":\"http://0.0.0.0:8000/v1/rerank\","
            "\"access_key\":\"mock-key\",\"provider\":\"siliconflow\"}')"),
    ("check", {
        "name": "ai_rerank string array with NULL doc key",
        "sql": "select json_length(ai_rerank('enhance_rerank','apple',"
               "'[\"apple\",\"banana\",\"fruit\",\"vegetable\"]', null))",
        "expect": {"rows": [[4]]},
    }),
    ("check", {
        "name": "ai_rerank object array with text doc key",
        "sql": "select json_unquote(json_extract(r,'$[0].id')), json_length(r) from "
               "(select ai_rerank('enhance_rerank','apple',"
               "'[{\"id\":\"a\",\"text\":\"apple\"},{\"id\":\"b\",\"text\":\"banana\"}]',"
               "'text') as r) x",
        "expect": {"rows": [["a", 2]]},
    }),

    # ---- Ollama provider: empty access_key, native protocol ----
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_ollama_complete')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_ollama_complete_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('enhance_ollama_complete', "
            "'{\"type\":\"completion\",\"model_name\":\"qwen2.5\"}')"),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enhance_ollama_complete_ep', "
            "'{\"ai_model_name\":\"enhance_ollama_complete\",\"scope\":\"all\","
            "\"url\":\"http://0.0.0.0:8000/api/generate\","
            "\"access_key\":\"\",\"provider\":\"ollama\"}')"),
    ("check", {
        "name": "ollama endpoint accepted without access key",
        "sql": "select provider from oceanbase.DBA_OB_AI_MODEL_ENDPOINTS "
               "where endpoint_name = 'enhance_ollama_complete_ep'",
        "expect": {"rows": [["ollama"]]},
    }),
    ("sql", "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('enhance_ollama_complete_ep', "
            "'{\"provider\":\"Ollama\"}')"),
    ("check", {
        "name": "ollama provider name keeps case",
        "sql": "select provider from oceanbase.DBA_OB_AI_MODEL_ENDPOINTS "
               "where endpoint_name = 'enhance_ollama_complete_ep'",
        "expect": {"rows": [["Ollama"]]},
    }),
    ("check", {
        "name": "ai_complete through ollama",
        "sql": "select length(ai_complete('enhance_ollama_complete','hello')) > 0",
        "expect": {"rows": [[1]]},
    }),

    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_ollama_embed')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_ollama_embed_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('enhance_ollama_embed', "
            "'{\"type\":\"dense_embedding\",\"model_name\":\"nomic-embed-text\"}')"),
    ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enhance_ollama_embed_ep', "
            "'{\"ai_model_name\":\"enhance_ollama_embed\",\"scope\":\"all\","
            "\"url\":\"http://0.0.0.0:8000/api/embed\","
            "\"access_key\":\"\",\"provider\":\"OLLAMA\"}')"),
    ("check", {
        "name": "ai_embed through ollama",
        "sql": "select json_valid(ai_embed('enhance_ollama_embed','hello'))",
        "expect": {"rows": [[1]]},
    }),

    # ---- cleanup, same order as the official case ----
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_complete')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_complete_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_embed')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_embed_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_rerank')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_rerank_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_ollama_complete')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_ollama_complete_ep')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_ollama_embed')", TOLERATED_DROP_ERRORS),
    ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('enhance_ollama_embed_ep')", TOLERATED_DROP_ERRORS),
]


FILE_SQL_DATA_SOURCE = Path("tools/deploy/mysql_test/test_suite/file_sql/data")


def prepare_file_sql_fixtures(repo_root, target):
    """Reproduce include/copy_file_sql_data.inc without ssh/scp.

    The official case copies the fixtures into the observer working directory and
    decodes simple.parquet.b64 there; locally we only need a plain directory the
    server can read.
    """
    source = Path(repo_root) / FILE_SQL_DATA_SOURCE
    if not source.is_dir():
        return None
    target = Path(target)
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for item in source.iterdir():
        if item.suffix == ".b64":
            (target / item.stem).write_bytes(base64.b64decode(item.read_text(encoding="utf-8")))
        else:
            shutil.copy2(item, target / item.name)
    return target


def file_sql_steps(data_dir):
    """Assertions of tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test."""
    root = str(Path(data_dir)).replace("\\", "/")
    csv_path = "{}/basic.csv".format(root)
    jsonl_path = "{}/basic.jsonl".format(root)
    parquet_path = "{}/simple.parquet".format(root)
    export_path = "{}/export.csv".format(root)
    return [
        ("check", {
            "name": "enable_file_sql defaults to OFF",
            "sql": "select @@session.enable_file_sql",
            "expect": {"rows": [[0]]},
        }),
        ("error", "select count(*) from read_csv('{}')".format(csv_path), (1235,),
         "read_csv rejected while the gate is off"),
        ("sql", "set session enable_file_sql = ON"),
        ("check", {
            "name": "read_csv scans once the gate is on",
            "sql": "select count(*) from read_csv('{}')".format(csv_path),
            "expect": {"rows": [[3]]},
        }),
        ("error", "select * from read_csv('{}', 'xml')".format(csv_path), (1064,),
         "unknown reader format is a syntax error"),
        ("error", "select * from read_csv('{}', sample_size = 10)".format(csv_path), (1235,),
         "unsupported reader option is rejected"),
        ("check", {
            "name": "csv scan: type conversion, duplicate header, multiline quote",
            "sql": "select id, name, score, active, day, created, "
                   "replace(note, char(10), '<NL>') as note, id__2 "
                   "from read_csv('{}') f order by id".format(csv_path),
            "expect": {"rows": [
                [1, "Alice", 98.5, 1, "2026-08-01", "2026-08-01 10:11:12", "hello,<NL>world", 101],
                [2, "Bob", 87, 0, None, None, "plain", 102],
                [3, "Chen", 90.25, 1, "2026-08-03", "2026-08-03 12:13:14", 'say "hi"', 103],
            ]},
        }),
        ("check", {
            "name": "csv aggregate over inferred schema",
            "sql": "select count(*) as row_count, sum(id) as id_sum, "
                   "round(avg(score), 2) as score_avg, sum(active) as active_count "
                   "from read_csv('{}', header = true, sample_size = -1) f".format(csv_path),
            "expect": {"rows": [[3, 6, 91.92, 2]]},
        }),
        ("check", {
            "name": "jsonl union schema with missing fields",
            "sql": "select id, name, score, active, day, note "
                   "from read_json('{}', format = 'newline_delimited') f order by id".format(jsonl_path),
            "expect": {"rows": [
                [1, "Alice", 98.5, 1, None, None],
                [2, "Bob", None, 0, "2026-08-02", None],
                [3, "Chen", 90, 1, None, "ok"],
            ]},
        }),
        ("check", {
            "name": "describe renders the inferred csv schema",
            "sql": "describe select * from read_csv('{}')".format(csv_path),
            "expect": {"lenient_null": True, "rows": [
                ["id", "BIGINT", "NO", None, None, None],
                ["name", "VARCHAR", "NO", None, None, None],
                ["score", "DOUBLE", "NO", None, None, None],
                ["active", "BOOLEAN", "NO", None, None, None],
                ["day", "DATE", "YES", None, None, None],
                ["created", "DATETIME", "YES", None, None, None],
                ["note", "VARCHAR", "NO", None, None, None],
                ["id__2", "BIGINT", "NO", None, None, None],
            ]},
        }),
        ("check", {
            "name": "parquet scan aggregates projected columns",
            "sql": "select count(*) as row_count, min(c1) as min_c1, "
                   "max(c1) as max_c1, sum(c1) as sum_c1 "
                   "from read_parquet('{}') p".format(parquet_path),
            "expect": {"rows": [[7, 0, 6, 21]]},
        }),
        ("check", {
            "name": "glob lists every fixture as one file column",
            "sql": "select substring_index(file, '/', -1) as file from glob('{}/*') "
                   "order by file".format(root),
            "expect": {"rows": [["basic.csv"], ["basic.jsonl"], ["invalid.txt"], ["simple.parquet"]]},
        }),
        ("sql", "copy (select id, name, score from read_csv('{}') order by id) "
                "to '{}' (format csv, header, nullstr 'NULL')".format(csv_path, export_path)),
        ("check", {
            "name": "csv written by copy reads back",
            "sql": "select id, name, score from read_csv('{}') order by id".format(export_path),
            "expect": {"rows": [[1, "Alice", 98.5], [2, "Bob", 87], [3, "Chen", 90.25]]},
        }),
        ("sql", "set session enable_file_sql = OFF"),
    ]


def wait_port(port, timeout, host="127.0.0.1"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.5)
    return False


def tail(path, lines=80):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return "".join(handle.readlines()[-lines:])
    except OSError as error:
        return "<unreadable {}: {}>".format(path, error)


def normalise(value):
    if value is None:
        return None
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, datetime.datetime):
        return value.strftime("%Y-%m-%d %H:%M:%S")
    if isinstance(value, datetime.date):
        return value.strftime("%Y-%m-%d")
    if isinstance(value, decimal.Decimal):
        integral = value.to_integral_value()
        return int(value) if value == integral else float(value)
    if isinstance(value, (int, float)):
        return value
    text = str(value)
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def compare_rows(actual, expected, lenient_null=False):
    if len(actual) != len(expected):
        return "row count {} != expected {}".format(len(actual), expected)
    for index, (got, want) in enumerate(zip(actual, expected)):
        if len(got) != len(want):
            return "row {} has {} columns, expected {}".format(index, len(got), len(want))
        for column, (got_value, want_value) in enumerate(zip(got, want)):
            got_normalised = normalise(got_value)
            if want_value is None:
                if got_normalised is not None and not (lenient_null and got_normalised == "NULL"):
                    return "row {} column {} = {!r}, expected NULL".format(index, column, got_value)
            elif got_normalised != want_value:
                return "row {} column {} = {!r}, expected {!r}".format(index, column, got_value, want_value)
    return None


def run_step(cursor, step):
    kind = step[0]
    if kind == "error":
        # ``--error N`` in the official case means: the statement must fail with N.
        sql, allowed = step[1], step[2]
        try:
            cursor.execute(sql)
            cursor.fetchall()
            return False, "expected error {}, got success".format(list(allowed))
        except Exception as error:  # noqa: BLE001 - errno matters, not the class
            errno = error.args[0] if error.args else None
            if errno in allowed:
                return True, None
            return False, "{}: {}".format(errno, error)

    if kind == "sql":
        sql = step[1]
        tolerated = step[2] if len(step) > 2 else (0,)
        try:
            cursor.execute(sql)
            cursor.fetchall()
            return True, None
        except Exception as error:  # noqa: BLE001 - errno matters, not the class
            errno = error.args[0] if error.args else None
            if errno in tolerated:
                return True, None
            return False, "{}: {}".format(errno, error)

    payload = step[1]
    try:
        cursor.execute(payload["sql"])
        rows = [list(row) for row in cursor.fetchall()]
    except Exception as error:  # noqa: BLE001
        errno = error.args[0] if error.args else None
        return False, "{}: {}".format(errno, error)

    expect = payload["expect"]
    if "json" in expect:
        if len(rows) != 1 or len(rows[0]) != 1:
            return False, "expected exactly one json value, got {!r}".format(rows)
        try:
            actual = json.loads(rows[0][0])
        except (TypeError, ValueError) as error:
            return False, "result is not json: {!r} ({})".format(rows[0][0], error)
        if actual != expect["json"]:
            return False, "json {} != expected {}".format(
                json.dumps(actual, ensure_ascii=False, sort_keys=True),
                json.dumps(expect["json"], ensure_ascii=False, sort_keys=True))
        return True, None

    problem = compare_rows(rows, expect["rows"], expect.get("lenient_null", False))
    return (problem is None), problem


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seekdb", required=True)
    parser.add_argument("--base-dir", required=True)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock-port", type=int, default=8000)
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--keep-running", action="store_true")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[3]),
                        help="checkout root, used to locate the official file_sql fixtures")
    parser.add_argument("--skip-file-sql", action="store_true",
                        help="only replay the AI-function case")
    args = parser.parse_args()

    work_dir = Path(args.work_dir or os.environ.get("RUNNER_TEMP") or "/tmp") / "ai-e2e"
    work_dir.mkdir(parents=True, exist_ok=True)
    base_dir = Path(args.base_dir).resolve()
    if base_dir.exists():
        shutil.rmtree(base_dir)
    base_dir.mkdir(parents=True)

    seekdb_log = work_dir / "seekdb.log"
    mock_log = work_dir / "mock.log"
    for path in (seekdb_log, mock_log):
        path.write_text("", encoding="utf-8")

    steps = list(STEPS)
    if not args.skip_file_sql:
        fixtures = prepare_file_sql_fixtures(args.repo_root, work_dir / "file_sql_data")
        if fixtures is None:
            print("SKIP  file_sql fixtures not found under {}".format(args.repo_root), flush=True)
        else:
            steps.extend(file_sql_steps(fixtures))
            print("file_sql fixtures at {}".format(fixtures), flush=True)

    mock_script = Path(__file__).with_name("mock_ai_endpoints.py")
    mock = subprocess.Popen(
        [sys.executable, str(mock_script), "--port", str(args.mock_port), "--log", str(mock_log)],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True)
    seekdb = None
    failures = []
    try:
        if not wait_port(args.mock_port, 30):
            print("FAIL  mock endpoints did not open port {}".format(args.mock_port))
            print(tail(mock_log))
            return 1

        seekdb = subprocess.Popen(
            [str(Path(args.seekdb).resolve()), "--base-dir={}".format(base_dir),
             "--port={}".format(args.port), "--nodaemon"],
            cwd=str(base_dir),
            stdout=open(seekdb_log, "ab"), stderr=subprocess.STDOUT, start_new_session=True)
        print("seekdb pid {} base-dir {}".format(seekdb.pid, base_dir), flush=True)

        if not wait_port(args.port, READY_TIMEOUT):
            print("FAIL  seekdb did not open port {} within {}s".format(args.port, READY_TIMEOUT))
            print(tail(seekdb_log, 120))
            return 1

        import pymysql  # imported late so the mock can start even without it

        deadline = time.time() + READY_TIMEOUT
        connection = None
        last_error = None
        while time.time() < deadline:
            try:
                connection = pymysql.connect(
                    host="127.0.0.1", port=args.port, user="root", password="",
                    charset="utf8mb4", autocommit=True, connect_timeout=10,
                    read_timeout=180, write_timeout=180)
                with connection.cursor() as cursor:
                    cursor.execute("select 1")
                    cursor.fetchall()
                break
            except Exception as error:  # noqa: BLE001 - server may still be booting
                last_error = error
                connection = None
                time.sleep(1)
        if connection is None:
            print("FAIL  could not connect to seekdb: {}".format(last_error))
            print(tail(seekdb_log, 120))
            return 1

        with connection.cursor() as cursor:
            cursor.execute("create database if not exists ai_e2e")
            cursor.execute("use ai_e2e")
            for step in steps:
                ok, problem = run_step(cursor, step)
                if step[0] in ("check", "error"):
                    name = step[1]["name"] if step[0] == "check" else step[3]
                    if ok:
                        print("PASS  {}".format(name), flush=True)
                    else:
                        print("FAIL  {} :: {}".format(name, problem), flush=True)
                        failures.append((name, step[1]["sql"] if step[0] == "check" else step[1],
                                         problem))
                elif not ok:
                    print("FAIL  setup statement :: {}".format(problem), flush=True)
                    failures.append(("setup", step[1], problem))
                    break

        connection.close()
    finally:
        if seekdb is not None and not args.keep_running:
            seekdb.terminate()
            try:
                seekdb.wait(timeout=60)
            except subprocess.TimeoutExpired:
                seekdb.kill()
        mock.terminate()
        try:
            mock.wait(timeout=30)
        except subprocess.TimeoutExpired:
            mock.kill()
        print("\n--- mock.log tail ---\n" + tail(mock_log, 60))
        print("--- seekdb.log tail ---\n" + tail(seekdb_log, 60))

    if failures:
        print("\n{} assertion(s) failed".format(len(failures)))
        for name, sql, problem in failures:
            print("- {} :: {} :: {}".format(name, " ".join(sql.split())[:160], problem))
        return 1
    print("\nall assertions passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        sys.exit(2)
