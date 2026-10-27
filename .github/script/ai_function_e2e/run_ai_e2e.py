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


COLUMN_DROP_ENDPOINT = (0, 11112)
COLUMN_DROP_MODEL = (0, 11118)
COLUMN_ANY = (0, 1049, 1008, 11112, 11118)

AI_COLUMN_DDL_ASSERTIONS = [
    ("check", {
        "name": "ai column metadata registers two rows",
        "sql": "select count(*) = 2 from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs'",
        "expect": {"rows": [[1]]},
    }),
    ("check", {
        "name": "ai column keeps per-column retry budget",
        "sql": "select a.column_name, a.max_retries from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
               "order by a.column_name",
        "expect": {"rows": [["emb", 3], ["tags", 2]]},
    }),
    ("check", {
        "name": "three hidden state columns per ai column",
        "sql": "select count(*) = 6 from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "join oceanbase.__all_column c on c.table_id = a.table_id "
               "and c.column_id in (a.state_column_id, a.request_column_id, "
               "a.result_spec_column_id) "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
               "and c.is_hidden = 1",
        "expect": {"rows": [[1]]},
    }),
    ("check", {
        "name": "result columns stay user visible",
        "sql": "select count(*) = 2 from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "join oceanbase.__all_column c on c.table_id = a.table_id "
               "and c.column_id = a.column_id "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
               "and c.is_hidden = 0",
        "expect": {"rows": [[1]]},
    }),
    ("check", {
        "name": "json result spec prompt is persisted",
        "sql": "select count(*) = 1 from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
               "and a.column_name = 'tags' "
               "and a.normalized_expr like '%Return exactly one valid JSON value%'",
        "expect": {"rows": [[1]]},
    }),
    ("check", {
        "name": "alter table add ai column registers metadata",
        "sql": "select count(*) = 1 from oceanbase.__all_ai_column a "
               "join oceanbase.__all_table t on t.table_id = a.table_id "
               "join oceanbase.__all_database d on d.database_id = t.database_id "
               "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'alter_docs'",
        "expect": {"rows": [[1]]},
    }),
    ("error",
     "create table ai_column_p0_ddl.bad_dimension(content varchar(20), "
     "emb vector(4) AI COLUMN (AI_EMBED('p0_ddl_embed', content, 3)))",
     (7600,), "dimension mismatch is rejected"),
    ("error",
     "create table ai_column_p0_ddl.bad_dependency(content varchar(20), "
     "first_ai varchar(200) AI COLUMN (AI_COMPLETE('p0_ddl_complete', content)), "
     "second_ai varchar(200) AI COLUMN (AI_COMPLETE('p0_ddl_complete', first_ai)))",
     (1235,), "ai column cannot depend on another ai column"),
]


def ai_column_steps():
    """Assertions of tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test.

    That case makes no model request: it only exercises the physical DDL,
    the metadata catalog and the validation errors.
    """
    return [
        ("sql", "drop database if exists ai_column_p0_ddl", COLUMN_ANY),
        ("sql", "create database ai_column_p0_ddl"),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_embed_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_complete')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_embed')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_ddl_complete', "
                "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_ddl_embed', "
                "'{\"type\":\"dense_embedding\",\"model_name\":\"mock-embed\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_ddl_complete_ep', "
                "'{\"ai_model_name\":\"p0_ddl_complete\","
                "\"url\":\"http://127.0.0.1:19120/v1/chat/completions\","
                "\"access_key\":\"p0-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-complete\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_ddl_embed_ep', "
                "'{\"ai_model_name\":\"p0_ddl_embed\","
                "\"url\":\"http://127.0.0.1:19120/v1/embeddings\","
                "\"access_key\":\"p0-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-embed\"}')"),
        ("sql", "drop table if exists ai_column_p0_ddl.docs", COLUMN_ANY),
        ("sql", "create table ai_column_p0_ddl.docs ("
                "id bigint primary key, content varchar(200), "
                "tags json AI COLUMN (AI_COMPLETE('p0_ddl_complete', "
                "concat('Classify: ', content))) WITH (sync_mode='async', max_retries=2), "
                "emb vector(4) AI COLUMN (AI_EMBED('p0_ddl_embed', content)) "
                "WITH (sync_mode='async', max_retries=3))"),
    ] + AI_COLUMN_DDL_ASSERTIONS[:5] + [
        ("sql", "drop table if exists ai_column_p0_ddl.alter_docs", COLUMN_ANY),
        ("sql", "create table ai_column_p0_ddl.alter_docs(id bigint primary key, "
                "content varchar(200))"),
        ("sql", "alter table ai_column_p0_ddl.alter_docs add column tags json AI COLUMN ("
                "AI_COMPLETE('p0_ddl_complete', content)) WITH (sync_mode='async', "
                "max_retries=1)"),
    ] + AI_COLUMN_DDL_ASSERTIONS[5:]


def ai_column_cleanup_steps():
    return [
        ("sql", "drop table ai_column_p0_ddl.alter_docs", COLUMN_ANY),
        ("sql", "drop table ai_column_p0_ddl.docs", COLUMN_ANY),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_embed_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_complete')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_embed')", COLUMN_DROP_MODEL),
        ("sql", "drop database if exists ai_column_p0_ddl", COLUMN_ANY),
    ]


AI_COLUMN_DML_PORT = 19121


def ai_column_dml_steps():
    """Assertions of ai_column/t/ai_column_t1_dml.test (async T1 lifecycle).

    The official case drives the async worker through transaction invalidation,
    primary-key rewrites, rollback, batches and deletes.  Its mysqltest ``while``
    polling loops become ``wait`` steps here.
    """
    task_count = ("select count(*) = {} from oceanbase.__all_ai_column_task task "
                  "join oceanbase.__all_table t on t.table_id = task.table_id "
                  "join oceanbase.__all_database d on d.database_id = t.database_id "
                  "where d.database_name = 'ai_column_p0_t1' and t.table_name = 'docs'")
    doc = "ai_column_p0_t1.docs"
    hidden = [("sql", "drop table if exists {}.docs".format("ai_column_p0_t1"), COLUMN_ANY)]
    steps = [
        ("mockstart", AI_COLUMN_DML_PORT),
        ("sql", "drop database if exists ai_column_p0_t1", COLUMN_ANY),
        ("sql", "create database ai_column_p0_t1"),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_t1_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_t1_complete')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_t1_complete', "
                "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_t1_complete_ep', "
                "'{\"ai_model_name\":\"p0_t1_complete\","
                "\"url\":\"http://127.0.0.1:%d/v1/chat/completions\","
                "\"access_key\":\"p0-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-complete\"}')" % AI_COLUMN_DML_PORT),
        ("sql", "create table ai_column_p0_t1.docs(id bigint primary key, "
                "title varchar(100), content varchar(200), "
                "tags json AI COLUMN (AI_COMPLETE('p0_t1_complete', "
                "concat('Classify: ', content))) WITH (sync_mode='async', max_retries=2))"),

        # ---- transaction 1: the result stays hidden until commit ----
        ("sql", "start transaction"),
        ("sql", "insert into {}(id, title, content) values(1, 'title-1', 'initial')".format(doc)),
        ("check", {
            "name": "ai result hidden until commit",
            "sql": "select id, tags is null from {}".format(doc),
            "expect": {"rows": [[1, 1]]},
        }),
        ("check", {
            "name": "insert creates an immediately eligible task",
            "sql": "select count(*) = 1 and max(task.eligible_ts <= now(6)) = 1 "
                   "from oceanbase.__all_ai_column_task task "
                   "join oceanbase.__all_table t on t.table_id = task.table_id "
                   "join oceanbase.__all_database d on d.database_id = t.database_id "
                   "where d.database_name = 'ai_column_p0_t1' and t.table_name = 'docs'",
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),
        ("wait", {
            "name": "worker completes the first task",
            "sql": "select tags is not null from {} where id = 1".format(doc),
            "expect": {"rows": [[1]]},
            "timeout": 30,
        }),
        ("check", {
            "name": "async completion stores the prompt echo",
            "sql": "select tags is not null, "
                   "json_unquote(json_extract(tags, '$.input')) = 'Classify: initial' "
                   "from {} where id = 1".format(doc),
            "expect": {"rows": [[1, 1]]},
        }),

        # ---- unrelated update: no invalidation, no request ----
        ("mockreset", AI_COLUMN_DML_PORT),
        ("sql", "update {} set title = 'title-2' where id = 1".format(doc)),
        ("sleep", 2),
        ("check", {
            "name": "unrelated update keeps the result",
            "sql": "select tags is not null from {} where id = 1".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "unrelated update creates no task",
            "sql": task_count.format(0),
            "expect": {"rows": [[1]]},
        }),
        ("mockassert", AI_COLUMN_DML_PORT, "no-requests"),

        # ---- dependent column update invalidates ----
        ("sql", "start transaction"),
        ("sql", "update {} set content = 'dependency-update' where id = 1".format(doc)),
        ("check", {
            "name": "dependency update invalidates the result",
            "sql": "select tags is null from {} where id = 1".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "dependency update creates one task",
            "sql": task_count.format(1),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),
        ("wait", {
            "name": "worker completes the dependency task",
            "sql": "select json_unquote(json_extract(tags, '$.input')) "
                   "= 'Classify: dependency-update' from {} where id = 1".format(doc),
            "expect": {"rows": [[1]]},
            "timeout": 30,
        }),

        # ---- primary key rewrite ----
        ("sql", "start transaction"),
        ("sql", "update {} set id = 2 where id = 1".format(doc)),
        ("check", {
            "name": "primary key update removes the old row",
            "sql": "select count(*) = 0 from {} where id = 1".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "primary key update invalidates the result",
            "sql": "select tags is null from {} where id = 2".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "new task keyed by the new primary key",
            "sql": "select count(*) = 1 from oceanbase.__all_ai_column_task task "
                   "join oceanbase.__all_table t on t.table_id = task.table_id "
                   "join oceanbase.__all_database d on d.database_id = t.database_id "
                   "where d.database_name = 'ai_column_p0_t1' and t.table_name = 'docs' "
                   "and task.rowkey_predicate = '`id` = 2'",
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),
        ("wait", {
            "name": "worker completes the rewritten key",
            "sql": "select tags is not null from {} where id = 2".format(doc),
            "expect": {"rows": [[1]]},
            "timeout": 30,
        }),
        ("check", {
            "name": "task queue drains after the rewrite",
            "sql": task_count.format(0),
            "expect": {"rows": [[1]]},
        }),

        # ---- rollback restores the row and drops the task ----
        ("mockreset", AI_COLUMN_DML_PORT),
        ("sql", "start transaction"),
        ("sql", "update {} set content = 'rollback-value' where id = 2".format(doc)),
        ("check", {
            "name": "transaction sees the invalidation",
            "sql": "select content = 'rollback-value' and tags is null "
                   "from {} where id = 2".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "transaction sees one pending task",
            "sql": task_count.format(1),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "rollback"),
        ("check", {
            "name": "rollback restores the row and result",
            "sql": "select content = 'dependency-update' and tags is not null "
                   "from {} where id = 2".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "rollback removes the pending task",
            "sql": task_count.format(0),
            "expect": {"rows": [[1]]},
        }),
        ("sleep", 2),
        ("mockassert", AI_COLUMN_DML_PORT, "no-rollback-value"),

        # ---- batch insert then delete inside one transaction ----
        ("sql", "start transaction"),
        ("sql", "insert into {}(id, title, content) "
                "values (10, 'batch-10', 'batch-10'), (11, 'batch-11', 'batch-11')".format(doc)),
        ("check", {
            "name": "batch insert invalidates two rows",
            "sql": "select count(*) = 2 from {} where id in (10, 11) and tags is null".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "batch insert creates two tasks",
            "sql": "select count(*) = 2 from oceanbase.__all_ai_column_task task "
                   "join oceanbase.__all_table t on t.table_id = task.table_id "
                   "join oceanbase.__all_database d on d.database_id = t.database_id "
                   "where d.database_name = 'ai_column_p0_t1' and t.table_name = 'docs' "
                   "and task.rowkey_predicate in ('`id` = 10', '`id` = 11')",
            "expect": {"rows": [[1]]},
        }),
        ("sql", "delete from {} where id in (10, 11)".format(doc)),
        ("check", {
            "name": "delete cleans the pending tasks",
            "sql": "select count(*) = 0 from oceanbase.__all_ai_column_task task "
                   "join oceanbase.__all_table t on t.table_id = task.table_id "
                   "join oceanbase.__all_database d on d.database_id = t.database_id "
                   "where d.database_name = 'ai_column_p0_t1' and t.table_name = 'docs' "
                   "and task.rowkey_predicate in ('`id` = 10', '`id` = 11')",
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),

        # ---- delete the last row ----
        ("sql", "delete from {} where id = 2".format(doc)),
        ("check", {
            "name": "delete removes the row",
            "sql": "select count(*) = 0 from {}".format(doc),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "delete leaves no task behind",
            "sql": task_count.format(0),
            "expect": {"rows": [[1]]},
        }),
    ]
    return hidden + steps


def ai_column_dml_cleanup_steps():
    return [
        ("sql", "drop table if exists ai_column_p0_t1.docs", COLUMN_ANY),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_t1_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_t1_complete')", COLUMN_DROP_MODEL),
        ("sql", "drop database if exists ai_column_p0_t1", COLUMN_ANY),
        ("mockstop", AI_COLUMN_DML_PORT),
    ]


AI_COLUMN_WORKER_PORT = 19122
AI_COLUMN_VECTOR_PORT = 19123
AI_COLUMN_RETRY_PORT = 19124


def _task_count_sql(db, table):
    return ("select count(*) from oceanbase.__all_ai_column_task task "
            "join oceanbase.__all_table t on t.table_id = task.table_id "
            "join oceanbase.__all_database d on d.database_id = t.database_id "
            "where d.database_name = '{}' and t.table_name = '{}'".format(db, table))


def _tasks_absent_sql(db, table):
    """``count(*) = 0`` over the task rows of one table.

    ``_task_count_sql`` already aggregates, so wrapping it in another
    ``count(*)`` would compare 1 to 0 and always fail; this helper keeps the
    aggregate in the top position instead.
    """
    return ("select count(*) = 0 from oceanbase.__all_ai_column_task task "
            "join oceanbase.__all_table t on t.table_id = task.table_id "
            "join oceanbase.__all_database d on d.database_id = t.database_id "
            "where d.database_name = '{}' and t.table_name = '{}'".format(db, table))


def ai_column_worker_steps():
    """Assertions of ai_column/t/ai_column_worker.test.

    Covers the grouped (one request per row, several AI columns) visibility rule,
    a transient 400 that must be retried and then succeed, a terminal failure that
    must leave NULL behind, and the stale-result guard: a slow request for the old
    value must never overwrite the result of the newer value.
    """
    db = "ai_column_p0_worker"
    docs = db + ".docs"
    port = AI_COLUMN_WORKER_PORT
    steps = [
        ("mockstart", port),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("sql", "create database {}".format(db)),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_worker_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_worker_fail_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_worker_complete')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_worker_fail')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_worker_complete', "
                "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_worker_fail', "
                "'{\"type\":\"completion\",\"model_name\":\"mock-fail\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_worker_complete_ep', "
                "'{\"ai_model_name\":\"p0_worker_complete\","
                "\"url\":\"http://127.0.0.1:%d/v1/chat/completions\","
                "\"access_key\":\"p0-worker-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-complete\"}')" % port),
        # ``always-fail`` is a path the official mock answers with HTTP 400.
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_worker_fail_ep', "
                "'{\"ai_model_name\":\"p0_worker_fail\","
                "\"url\":\"http://127.0.0.1:%d/v1/always-fail/completions\","
                "\"access_key\":\"p0-worker-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-fail\"}')" % port),
        ("sql", "drop table if exists {}".format(docs), COLUMN_ANY),
        ("sql", "create table {}(id bigint primary key, content varchar(200), "
                "tags json AI COLUMN (AI_COMPLETE('p0_worker_complete', "
                "concat('Classify: ', content))) WITH (sync_mode='async', max_retries=2), "
                "score bigint AI COLUMN (AI_COMPLETE('p0_worker_complete', "
                "concat('Score: ', content))) WITH (sync_mode='async', max_retries=2))".format(docs)),

        # ---- one request group feeds two AI columns, and both appear together ----
        ("sql", "start transaction"),
        ("sql", "insert into {}(id, content) values(1, 'typed-prompts')".format(docs)),
        ("check", {
            "name": "group starts hidden",
            "sql": "select tags is null and score is null from {} where id = 1".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "one request group for two columns",
            "sql": ("select count(*) = 2 and count(distinct task.request_id) = 1 "
                    "from oceanbase.__all_ai_column_task task "
                    "join oceanbase.__all_table t on t.table_id = task.table_id "
                    "join oceanbase.__all_database d on d.database_id = t.database_id "
                    "where d.database_name = '{}' and t.table_name = 'docs'").format(db),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),
        ("wait", {
            "name": "no partial visibility",
            "sql": ("select ifnull((tags is not null) + (score is not null), 0) "
                    "from {} where id = 1".format(docs)),
            "expect": {"rows": [[2]]},
            "forbid": {"rows": [[1]]},
            "timeout": 20,
        }),
        ("check", {
            "name": "grouped results visible",
            "sql": "select tags is not null and score = 7 from {} where id = 1".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "grouped tasks removed",
            "sql": _tasks_absent_sql(db, "docs"),
            "expect": {"rows": [[1]]},
        }),
        ("mockassert", port, "typed-prompts"),

        # ---- a transient 400 must be retried and then succeed ----
        ("sql", "drop table if exists {}.retry_docs".format(db), COLUMN_ANY),
        ("sql", "create table {}.retry_docs(id bigint primary key, content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('p0_worker_complete', content)) "
                "WITH (sync_mode='async', max_retries=2))".format(db)),
        ("sql", "insert into {}.retry_docs(id, content) values(2, 'retry-once')".format(db)),
        ("wait", {
            "name": "retry state committed",
            "interval": 0.05,
            "sql": ("select count(*) > 0 from oceanbase.__all_ai_column_task task "
                    "join oceanbase.__all_table t on t.table_id = task.table_id "
                    "join oceanbase.__all_database d on d.database_id = t.database_id "
                    "where d.database_name = '{}' and t.table_name = 'retry_docs' "
                    "and task.state = 3 and task.retry_count = 1").format(db),
            "expect": {"rows": [[1]]},
            "timeout": 15,
        }),
        ("wait", {
            "name": "retry eventually completes",
            "sql": "select ifnull(result = 'mock:retry-once', 0) from {}.retry_docs "
                   "where id = 2".format(db),
            "expect": {"rows": [[1]]},
            "timeout": 20,
        }),
        ("check", {
            "name": "retry task removed",
            "sql": _tasks_absent_sql(db, "retry_docs"),
            "expect": {"rows": [[1]]},
        }),
        ("mockassert", port, "retry-once"),

        # ---- terminal failure keeps NULL and drains the task ----
        ("sql", "drop table if exists {}.fail_docs".format(db), COLUMN_ANY),
        ("sql", "create table {}.fail_docs(id bigint primary key, content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('p0_worker_fail', content)) "
                "WITH (sync_mode='async', max_retries=0))".format(db)),
        ("sql", "insert into {}.fail_docs(id, content) values(3, 'terminal-failure')".format(db)),
        ("wait", {
            "name": "terminal failure removes task",
            "sql": _tasks_absent_sql(db, "fail_docs"),
            "expect": {"rows": [[1]]},
            "timeout": 15,
        }),
        ("check", {
            "name": "terminal failure keeps null",
            "sql": "select result is null from {}.fail_docs where id = 3".format(db),
            "expect": {"rows": [[1]]},
        }),

        # ---- stale guard: slow old request must not overwrite the new value ----
        ("mockreset", port),
        ("sql", "insert into {}(id, content) values(4, 'slow-old')".format(docs)),
        ("sleep", 0.3),
        ("sql", "update {} set content = 'fast-new' where id = 4".format(docs)),
        ("wait", {
            "name": "latest request wins",
            "sql": "select ifnull(tags is not null and score is not null, 0) "
                   "from {} where id = 4".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("sleep", 2),
        ("check", {
            "name": "late old result cannot overwrite",
            "sql": ("select json_unquote(json_extract(tags, '$.input')) = 'Classify: fast-new' "
                    "and score = 7 from {} where id = 4".format(docs)),
            "expect": {"rows": [[1]]},
        }),
        ("mockassert", port, "stale-guard"),
    ]
    return steps


def ai_column_worker_cleanup_steps():
    db = "ai_column_p0_worker"
    return [
        ("sql", "drop table if exists {}.fail_docs".format(db), COLUMN_ANY),
        ("sql", "drop table if exists {}.retry_docs".format(db), COLUMN_ANY),
        ("sql", "drop table if exists {}.docs".format(db), COLUMN_ANY),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_worker_fail_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_worker_complete_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_worker_fail')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_worker_complete')", COLUMN_DROP_MODEL),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("mockstop", AI_COLUMN_WORKER_PORT),
    ]


def ai_column_vector_steps():
    """Assertions of ai_column/t/ai_column_vector_index.test.

    Checks that a regenerated AI vector leaves the ANN index (so a stale vector
    cannot be hit), comes back when the new embedding is ready, and that a rollback
    restores both the row and its index entry.
    """
    db = "ai_column_p0_vector"
    docs = db + ".docs"
    port = AI_COLUMN_VECTOR_PORT
    ann = ("(select id from {} order by l2_distance(emb, [0,0,0,0]) "
           "approximate limit {}) ann")
    steps = [
        ("mockstart", port),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("sql", "create database {}".format(db)),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_vector_embed_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_vector_embed')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_vector_embed', "
                "'{\"type\":\"dense_embedding\",\"model_name\":\"mock-embed\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_vector_embed_ep', "
                "'{\"ai_model_name\":\"p0_vector_embed\","
                "\"url\":\"http://127.0.0.1:%d/v1/embeddings\","
                "\"access_key\":\"p0-vector-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-embed\"}')" % port),
        ("sql", "drop table if exists {}".format(docs), COLUMN_ANY),
        ("sql", "create table {}(id bigint primary key, content varchar(200), "
                "emb vector(4) AI COLUMN (AI_EMBED('p0_vector_embed', content)) "
                "WITH (sync_mode='async', max_retries=2))".format(docs)),
        ("sql", "insert into {}(id, content) values (1, 'vec-zero'), (2, 'vec-ten'), "
                "(3, 'vec-twenty')".format(docs)),
        ("wait", {
            "name": "initial embeddings ready",
            "sql": "select count(*) = 3 from {} where emb is not null".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("check", {
            "name": "mock vectors are stored verbatim",
            "sql": "select id from {} where emb is not null order by id".format(docs),
            "expect": {"rows": [[1], [2], [3]]},
        }),
        ("sql", "create vector index idx_p0_ai_emb on {}(emb) "
                "with(distance=l2, type=hnsw, lib=vsag, m=10, "
                "ef_construction=12, ef_search=40)".format(docs)),
        ("sql", "select id, l2_distance(emb, [0,0,0,0]) as distance from {} "
                "order by distance approximate limit 3".format(docs)),

        # ---- in-transaction regeneration: the stale vector must leave the index ----
        ("sql", "start transaction"),
        ("sql", "update {} set content = 'vec-zero' where id = 2".format(docs)),
        ("check", {
            "name": "vector hidden during regeneration",
            "sql": "select content = 'vec-zero' and emb is null from {} "
                   "where id = 2".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "vector update task created",
            "sql": ("select count(*) = 1 from oceanbase.__all_ai_column_task task "
                    "join oceanbase.__all_table t on t.table_id = task.table_id "
                    "join oceanbase.__all_database d on d.database_id = t.database_id "
                    "where d.database_name = '{}' and t.table_name = 'docs' "
                    "and task.rowkey_predicate = '`id` = 2'").format(db),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "old vector removed from ann",
            "sql": "select count(*) = 2 and sum(id = 2) = 0 from " + ann.format(docs, 3),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "commit"),
        ("wait", {
            "name": "regenerated vector ready",
            "sql": "select emb = [0,0,0,0] from {} where id = 2".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("check", {
            "name": "vector task removed",
            "sql": _tasks_absent_sql(db, "docs"),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "regenerated vector visible to ann",
            "sql": ("select count(*) = 2 and sum(id = 1) = 1 and sum(id = 2) = 1 from "
                    + ann.format(docs, 2)),
            "expect": {"rows": [[1]]},
        }),
        ("mockassert", port, "embedding"),

        # ---- rollback restores the row and its index entry, and sends no request ----
        ("mockreset", port),
        ("sql", "start transaction"),
        ("sql", "update {} set content = 'vec-ten' where id = 3".format(docs)),
        ("check", {
            "name": "vector transaction sees invalidation",
            "sql": "select content = 'vec-ten' and emb is null from {} "
                   "where id = 3".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "vector transaction sees task",
            "sql": ("select count(*) = 1 from oceanbase.__all_ai_column_task task "
                    "join oceanbase.__all_table t on t.table_id = task.table_id "
                    "join oceanbase.__all_database d on d.database_id = t.database_id "
                    "where d.database_name = '{}' and t.table_name = 'docs' "
                    "and task.rowkey_predicate = '`id` = 3'").format(db),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "rollback"),
        ("check", {
            "name": "vector rollback restores row",
            "sql": "select content = 'vec-twenty' and emb = [20,0,0,0] from {} "
                   "where id = 3".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "vector rollback removes task",
            "sql": _tasks_absent_sql(db, "docs"),
            "expect": {"rows": [[1]]},
        }),
        ("sleep", 2),
        ("mockassert", port, "no-requests"),
        ("check", {
            "name": "rollback vector still visible to ann",
            "sql": ("select count(*) = 1 and sum(id = 3) = 1 from "
                    "(select id from {} order by l2_distance(emb, [20,0,0,0]) "
                    "approximate limit 1) ann").format(docs),
            "expect": {"rows": [[1]]},
        }),
    ]
    return steps


def ai_column_vector_cleanup_steps():
    db = "ai_column_p0_vector"
    return [
        ("sql", "drop table if exists {}.docs".format(db), COLUMN_ANY),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_vector_embed_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_vector_embed')", COLUMN_DROP_MODEL),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("mockstop", AI_COLUMN_VECTOR_PORT),
    ]


def ai_column_retry_steps():
    """Assertions of ai_column/t/ai_column_retry_strict.test.

    Covers ``ALTER TABLE ... RETRY AI COLUMN <col> WHERE <predicate>``: retried rows
    are invalidated, the sibling AI column is untouched, strict reads reject exactly
    the referenced-but-incomplete AI column, a zero-match retry is a no-op, and the
    STATUS pseudo column can be combined with business predicates.
    """
    db = "ai_column_retry_strict"
    docs = db + ".docs"
    port = AI_COLUMN_RETRY_PORT
    strict_errors = ("select id from {} where result is null".format(docs), 11120)
    steps = [
        ("mockstart", port),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("sql", "create database {}".format(db)),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('retry_strict_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('retry_strict_model')", COLUMN_DROP_MODEL),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL('retry_strict_model', "
                "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')"),
        ("sql", "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('retry_strict_ep', "
                "'{\"ai_model_name\":\"retry_strict_model\","
                "\"url\":\"http://127.0.0.1:%d/v1/chat/completions\","
                "\"access_key\":\"retry-key\",\"provider\":\"openai\","
                "\"request_model_name\":\"mock-complete\"}')" % port),
        ("sql", "drop table if exists {}".format(docs), COLUMN_ANY),
        ("sql", "create table {}(id bigint primary key, category varchar(20), "
                "status varchar(20), content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('retry_strict_model', content)) "
                "WITH (sync_mode='async', max_retries=0), "
                "label varchar(400) AI COLUMN (AI_COMPLETE('retry_strict_model', "
                "concat('Label: ', content))) WITH (sync_mode='async', max_retries=0))".format(docs)),
        ("sql", "insert into {}(id, category, status, content) values "
                "(1, 'news', 'active', 'row-one'), (2, 'sports', 'active', 'row-two'), "
                "(3, 'news', 'inactive', 'row-three')".format(docs)),
        ("wait", {
            "name": "initial results ready",
            "sql": "select count(*) = 3 from {} where result is not null "
                   "and label is not null".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("check", {
            "name": "strict read defaults off",
            "sql": "select @@ai_column_strict_read = 0",
            "expect": {"rows": [[1]]},
        }),

        # ---- a business predicate retries exactly the selected rows ----
        ("mockstop", port),
        ("sql", "alter table {} retry ai column result where id in (1, 3)".format(docs)),
        ("sql", "set session ai_column_strict_read = off"),
        ("check", {
            "name": "selected result invalidated",
            "sql": "select id, result is null, label is not null from {} "
                   "order by id".format(docs),
            "expect": {"rows": [[1, 1, 1], [2, 0, 1], [3, 1, 1]]},
        }),

        # ---- strict read rejects only the AI column the statement references ----
        ("sql", "set session ai_column_strict_read = on"),
        ("check", {
            "name": "strict read keeps plain columns readable",
            "sql": "select id, category from {} order by id".format(docs),
            "expect": {"rows": [[1, "news"], [2, "sports"], [3, "news"]]},
        }),
        ("check", {
            "name": "ready label is readable",
            "sql": "select count(*) = 3 from {} where label is not null".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("error", "select result from {} where id = 2".format(docs), (11120,),
         "strict read rejects ready row of an incomplete column"),
        ("error", "select id, result from {} order by id".format(docs), (11120,),
         "strict read rejects a full scan of the column"),
        ("error", strict_errors[0], (11120,), "strict read rejects IS NULL filtering"),
        ("error", "select id from {} where id in (select id from {} where result is null)"
                  .format(docs, docs), (11120,), "strict read rejects nested subquery"),
        ("error", "update {} set category = category where result is null".format(docs),
         (11120,), "strict read rejects UPDATE touching the column"),
        ("error", "delete from {} where result is null".format(docs),
         (11120,), "strict read rejects DELETE touching the column"),
        ("sql", "set session ai_column_strict_read = off"),
        ("check", {
            "name": "rejected writes leave rows unchanged",
            "sql": "select count(*) = 3 from {}".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("wait", {
            "name": "failed task rows cleaned",
            "sql": _tasks_absent_sql(db, "docs"),
            "expect": {"rows": [[1]]},
            "timeout": 20,
        }),
        ("sql", "set session ai_column_strict_read = on"),
        ("error", "select result from {} where id = 1".format(docs), (11120,),
         "strict read still rejects the failed row"),
        ("sql", "set session ai_column_strict_read = off"),

        # ---- STATUS pseudo column, and a full retry that restores every row ----
        ("sql", "alter table {} retry ai column result "
                "where status = 'FAILED' and category = 'news'".format(docs)),
        ("mockstart", port),
        ("sql", "alter table {} retry ai column result".format(docs)),
        ("wait", {
            "name": "full retry ready",
            "sql": "select count(*) = 3 from {} where result is not null".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("sql", "set session ai_column_strict_read = on"),
        ("check", {
            "name": "strict read succeeds when all ready",
            "sql": "select count(*) = 3 from {} where result is not null".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "set session ai_column_strict_read = off"),
        ("mockstop", port),

        # ---- zero-match retry is a no-op, READY rows can be retried ----
        ("sql", "alter table {} retry ai column result "
                "where status = 'READY' and id = 999".format(docs)),
        ("check", {
            "name": "zero match keeps ready results",
            "sql": "select count(*) = 3 from {} where result is not null".format(docs),
            "expect": {"rows": [[1]]},
        }),
        ("check", {
            "name": "zero match creates no tasks",
            "sql": _tasks_absent_sql(db, "docs"),
            "expect": {"rows": [[1]]},
        }),
        ("sql", "alter table {} retry ai column result "
                "where status = 'READY' and category = 'sports'".format(docs)),
        ("check", {
            "name": "sports row retried",
            "sql": "select id, result is null from {} order by id".format(docs),
            "expect": {"rows": [[1, 0], [2, 1], [3, 0]]},
        }),
        ("mockstart", port),
        ("sql", "alter table {} retry ai column result".format(docs)),
        ("wait", {
            "name": "retry after ready restores everything",
            "sql": "select count(*) = 3 from {} where result is not null".format(docs),
            "expect": {"rows": [[1]]},
            "timeout": 25,
        }),
        ("mockstop", port),

        # ---- backticks address the physical status column, not the pseudo column ----
        ("sql", "alter table {} retry ai column result "
                "where `status` = 'active' and id = 1".format(docs)),
        ("check", {
            "name": "physical status row retried",
            "sql": "select id, result is null from {} order by id".format(docs),
            "expect": {"rows": [[1, 1], [2, 0], [3, 0]]},
        }),
        ("error", "alter table {} retry ai column content where id = 1".format(docs),
         (1235,), "retry targets an AI column only"),
        ("error", "alter table {} retry ai column result where id = 1, "
                  "add column extra_col int".format(docs),
         (1235,), "retry cannot be mixed with another ALTER action"),
    ]
    return steps


def ai_column_retry_cleanup_steps():
    db = "ai_column_retry_strict"
    return [
        ("sql", "drop table if exists {}.docs".format(db), COLUMN_ANY),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('retry_strict_ep')",
         COLUMN_DROP_ENDPOINT),
        ("sql", "call DBMS_AI_SERVICE.DROP_AI_MODEL('retry_strict_model')", COLUMN_DROP_MODEL),
        ("sql", "drop database if exists {}".format(db), COLUMN_ANY),
        ("mockstop", AI_COLUMN_RETRY_PORT),
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


RUNTIME = {"mock": None, "started": set()}


def mock_action(action, port, assertion=None):
    script = RUNTIME["mock"]
    if script is None:
        return False, "ai_column_mock.py not found in the checkout"
    argv = [sys.executable, str(script), action, str(port)]
    if assertion:
        argv.append(assertion)
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as error:
        return False, "{} {}".format(action, error)
    detail = (proc.stderr or proc.stdout or "").strip()
    return proc.returncode == 0, "exit {} {}".format(proc.returncode, detail[-300:])


def run_step(cursor, step):
    kind = step[0]
    if kind == "sleep":
        time.sleep(float(step[1]))
        return True, None

    if kind == "mockstart":
        ok, problem = mock_action("start", step[1])
        if ok:
            RUNTIME["started"].add(step[1])
        return ok, problem

    if kind == "mockstop":
        ok, problem = mock_action("stop", step[1])
        RUNTIME["started"].discard(step[1])
        return ok, problem

    if kind == "mockreset":
        return mock_action("reset", step[1])

    if kind == "mockassert":
        return mock_action("assert", step[1], step[2])

    if kind == "wait":
        # Poll like the official ``while`` loop.  ``forbid`` expresses the
        # "this must never be visible" assertions: seeing it is a failure.
        payload = step[1]
        timeout = float(payload.get("timeout", 30))
        interval = float(payload.get("interval", 1.0))
        deadline = time.time() + timeout
        rows = []
        problem = "no attempt"
        while True:
            try:
                cursor.execute(payload["sql"])
                rows = [list(row) for row in cursor.fetchall()]
                if "forbid" in payload:
                    if compare_rows(rows, payload["forbid"]["rows"],
                                    payload["forbid"].get("lenient_null", False)) is None:
                        return False, "observed forbidden state {!r}".format(rows)
                    problem = "still {!r}".format(rows)
                    if compare_rows(rows, payload["expect"]["rows"],
                                    payload["expect"].get("lenient_null", False)) is None:
                        return True, None
                else:
                    problem = compare_rows(rows, payload["expect"]["rows"],
                                           payload["expect"].get("lenient_null", False))
                    if problem is None:
                        return True, None
            except Exception as error:  # noqa: BLE001 - keep polling on transient errors
                problem = "{}: {}".format(error.args[0] if error.args else None, error)
            if time.time() > deadline:
                return False, "timeout after {}s: {}".format(int(timeout), problem)
            time.sleep(interval)

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
    parser.add_argument("--skip-ai-column", action="store_true",
                        help="skip the AI-column DDL case")
    parser.add_argument("--skip-ai-column-dml", action="store_true",
                        help="skip the AI-column async DML case")
    parser.add_argument("--skip-ai-column-worker", action="store_true",
                        help="skip the AI-column worker/retry/stale case")
    parser.add_argument("--skip-ai-column-vector", action="store_true",
                        help="skip the AI-column vector index case")
    parser.add_argument("--skip-ai-column-retry", action="store_true",
                        help="skip the AI-column retry/strict-read case")
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
    if not args.skip_ai_column:
        steps.extend(ai_column_steps())
        steps.extend(ai_column_cleanup_steps())
    ai_column_cases = (not args.skip_ai_column_dml or not args.skip_ai_column_worker
                       or not args.skip_ai_column_vector or not args.skip_ai_column_retry)
    if ai_column_cases:
        mock_script = (Path(args.repo_root) / "tools/deploy/mysql_test/test_suite/"
                       "ai_column/include/ai_column_mock.py")
        if mock_script.is_file():
            RUNTIME["mock"] = mock_script.resolve()
        else:
            print("SKIP  ai_column_mock.py not found under {}".format(args.repo_root), flush=True)
    if not args.skip_ai_column_dml and RUNTIME["mock"] is not None:
        steps.extend(ai_column_dml_steps())
        steps.extend(ai_column_dml_cleanup_steps())
    if not args.skip_ai_column_worker and RUNTIME["mock"] is not None:
        steps.extend(ai_column_worker_steps())
        steps.extend(ai_column_worker_cleanup_steps())
    if not args.skip_ai_column_vector and RUNTIME["mock"] is not None:
        steps.extend(ai_column_vector_steps())
        steps.extend(ai_column_vector_cleanup_steps())
    if not args.skip_ai_column_retry and RUNTIME["mock"] is not None:
        steps.extend(ai_column_retry_steps())
        steps.extend(ai_column_retry_cleanup_steps())
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
                if step[0] in ("check", "wait", "mockassert", "error"):
                    if step[0] == "mockassert":
                        name, sql = step[2], "mock assertion {}".format(step[2])
                    elif step[0] == "error":
                        name, sql = step[3], step[1]
                    else:
                        name, sql = step[1]["name"], step[1]["sql"]
                    if ok:
                        print("PASS  {}".format(name), flush=True)
                    else:
                        print("FAIL  {} :: {}".format(name, problem), flush=True)
                        failures.append((name, sql, problem))
                elif not ok:
                    print("FAIL  setup statement :: {}".format(problem), flush=True)
                    failures.append(("setup", step[1], problem))
                    break

        connection.close()
    finally:
        for port in sorted(RUNTIME["started"]):
            mock_action("stop", port)
        RUNTIME["started"].clear()
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
