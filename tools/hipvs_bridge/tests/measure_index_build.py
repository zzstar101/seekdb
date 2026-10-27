#!/usr/bin/env python3
r"""Development-only FP32/L2 build measurement against an already started seekdb.

Install numpy and pymysql, restart the intended server, then run, for example:
  SEEKDB_PASSWORD='' python3 tools/hipvs_bridge/tests/measure_index_build.py \
    --mode baseline --commit 08c76418a8e1fef3c1aae83010658ff56a4faa57 \
    --database test > baseline-01.json

Use --mode candidate with its full commit SHA, or --mode cpu for lib=vsag.
Repeat on independently restarted servers for each cold measurement. This
script does not restart services, clear caches, inspect source/binary identity,
prove GPU execution, enforce contest gates, benchmark batches, or award scores.
It never drops a table. The JSON identifies the new table retained for manual
cleanup. Do not use a shared production database for this development workload.

SQL references in this repository:
  tools/deploy/mysql_test/test_suite/vector_index/t/vector_index_post_create.test
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from numbers import Integral
import os
import platform
import re
import sys
import time
import uuid


BASELINE_SHA = "08c76418a8e1fef3c1aae83010658ff56a4faa57"
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}\Z")


class MeasurementError(RuntimeError):
    pass


def identifier(value):
    if not IDENTIFIER.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "identifiers must be 1-64 ASCII letters/digits/underscores, "
            "starting with a letter or underscore")
    return value


def positive(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def full_sha(value):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise argparse.ArgumentTypeError("a full 40-character commit SHA is required")
    return value.lower()


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", required=True, choices=("baseline", "candidate", "cpu"))
    parser.add_argument("--commit", required=True, type=full_sha,
                        help="declared server source commit; verified externally")
    parser.add_argument("--database", required=True, type=identifier,
                        help="existing development database; never created or dropped")
    parser.add_argument("--table", type=identifier,
                        help="new table name; default is a unique name; existing tables cause failure")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=positive, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--n", type=positive, default=10_000,
                        help="rows: at least 256 for GPU modes, 100 for CPU")
    parser.add_argument("--dim", type=positive, default=128)
    parser.add_argument("--queries", type=positive, default=100)
    parser.add_argument("--seed", type=int, default=1314)
    parser.add_argument("--import-batch", type=positive, default=250)
    parser.add_argument("--timeout", type=positive, default=3600,
                        help="SQL query and socket read/write timeout in seconds")
    return parser


def parse_args(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.n < 100:
        parser.error("--n must be at least 100 for Recall@100")
    if args.mode != "cpu" and args.n < 256:
        parser.error("GPU modes require --n >= 256: the frozen adaptor does not build CAGRA below this threshold")
    if args.n > 0xFFFFFFFF or args.dim > 0xFFFFFFFF:
        parser.error("--n and --dim must fit uint32")
    if args.seed < 0 or args.seed > 0xFFFFFFFFFFFFFFFF:
        parser.error("--seed must be an unsigned 64-bit integer")
    if args.port > 65535:
        parser.error("--port must be in 1..65535")
    if args.mode == "baseline" and args.commit != BASELINE_SHA:
        parser.error("baseline mode requires the frozen contest commit " + BASELINE_SHA)
    if args.table is None:
        args.table = "hipvs_" + args.mode + "_" + uuid.uuid4().hex[:16]
    return args


def vector_text(values):
    # Nine significant decimal digits round-trip every finite binary32 value.
    return "[" + ",".join(format(float(value), ".9g") for value in values) + "]"


def make_workload(args, np):
    rng = np.random.Generator(np.random.PCG64(args.seed))
    base = rng.standard_normal((args.n, args.dim), dtype=np.float32)
    probes = rng.standard_normal((args.queries, args.dim), dtype=np.float32)
    query_texts = [vector_text(query) for query in probes]
    # Accumulate exact L2 on the supplied FP32 values in float64. Compute one
    # query at a time instead of materializing an N x Q x D distance array.
    ground_truth = []
    base64 = base.astype(np.float64)
    ids = np.arange(args.n, dtype=np.int64)
    for query in probes:
        delta = base64 - query.astype(np.float64)
        squared_l2 = np.einsum("ij,ij->i", delta, delta)
        ground_truth.append(np.lexsort((ids, squared_l2))[:100].tolist())
    fingerprint = lambda values: hashlib.sha256(
        np.asarray(values, dtype="<f4").tobytes(order="C")).hexdigest()
    dataset = {
        "kind": "public_synthetic", "generator": "numpy.PCG64.standard_normal.float32",
        "seed": args.seed, "n": args.n, "dim": args.dim, "queries": args.queries,
        "dtype": "float32", "distance": "l2", "id_range": [0, args.n - 1],
        "base_f32le_sha256": fingerprint(base),
        "queries_f32le_sha256": fingerprint(probes),
        "ground_truth": "exact squared L2 of FP32 input, float64 accumulation; ID breaks ties",
    }
    return base, query_texts, ground_truth, dataset


def validate_ids(rows, topk, n):
    if len(rows) != topk:
        raise MeasurementError("expected %d IDs, received %d" % (topk, len(rows)))
    ids = []
    for row in rows:
        if len(row) != 1 or isinstance(row[0], bool) or not isinstance(row[0], Integral):
            raise MeasurementError("query returned a non-integer ID or unexpected row shape")
        row_id = int(row[0])
        if row_id < 0 or row_id >= n:
            raise MeasurementError("query returned an ID outside the inserted dataset: %d" % row_id)
        ids.append(row_id)
    if len(set(ids)) != topk:
        raise MeasurementError("query returned duplicate IDs")
    return ids


def query_sql(table, topk):
    identifier(table)
    if topk not in (10, 100):
        raise MeasurementError("only top-k 10 and 100 are supported")
    return ("SELECT id FROM `%s` ORDER BY l2_distance(v, %%s) "
            "APPROXIMATE LIMIT %d" % (table, topk))


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def measure(connection, args, base, query_texts, ground_truth, report,
            clock=time.perf_counter):
    """All data preparation/import precedes the first call to the build clock."""
    table = identifier(args.table)
    library = "vsag" if args.mode == "cpu" else "cuvs"
    index_sql = ("CREATE VECTOR INDEX `idx_v` ON `%s` (v) "
                 "WITH (distance=l2,type=hnsw,lib=%s,m=16,"
                 "ef_construction=200,ef_search=200)" % (table, library))
    first_sql = query_sql(table, 10)
    with connection.cursor() as cursor:
        cursor.execute("SHOW VARIABLES LIKE 'ob_compatibility_mode'")
        mode = cursor.fetchone()
        if not mode or str(mode[1]).upper() != "MYSQL":
            raise MeasurementError("this script requires seekdb MySQL compatibility mode")
        cursor.execute("SELECT VERSION(), @@sql_mode")
        version, sql_mode = cursor.fetchone()
        report["server_observed"] = {
            "version": str(version), "compatibility_mode": str(mode[1]),
            "sql_mode": str(sql_mode),
        }
        cursor.execute("SET SESSION ob_query_timeout = %s", (args.timeout * 1_000_000,))
        # No IF NOT EXISTS: a name collision must fail before inserting anything.
        cursor.execute("CREATE TABLE `%s` (id BIGINT PRIMARY KEY, v VECTOR(%d))"
                       % (table, args.dim))
        report["retained_table"]["created_by_this_run"] = True
        insert_sql = "INSERT INTO `%s` (id, v) VALUES (%%s, %%s)" % table
        for start in range(0, args.n, args.import_batch):
            rows = [(row_id, vector_text(base[row_id]))
                    for row_id in range(start, min(start + args.import_batch, args.n))]
            cursor.executemany(insert_sql, rows)
        connection.commit()
        cursor.execute("SELECT COUNT(*), MIN(id), MAX(id) FROM `%s`" % table)
        if tuple(cursor.fetchone()) != (args.n, 0, args.n - 1):
            raise MeasurementError("imported row count or ID range is incorrect")

        # Do not add a vector query, EXPLAIN, warmup, or cache reuse here.
        begin = clock()
        cursor.execute(index_sql)
        created = clock()
        cursor.execute(first_sql, (query_texts[0],))
        first_rows = cursor.fetchall()
        finished = clock()
        first_ids = validate_ids(first_rows, 10, args.n)
        report["build"] = {
            "create_s": created - begin,
            "first_query_s": finished - created,
            "end_to_end_s": finished - begin,
            "first_query": {"query_index": 0, "topk": 10, "ids": first_ids},
            "boundary": "CREATE VECTOR INDEX execute start through first fixed query fetchall completion",
        }

        report["single_query"] = {}
        for topk in (10, 100):
            sql = query_sql(table, topk)
            latencies = []
            recalls = []
            for query_index, text in enumerate(query_texts):
                start = clock()
                cursor.execute(sql, (text,))
                rows = cursor.fetchall()
                elapsed = clock() - start
                ids = validate_ids(rows, topk, args.n)
                recalls.append(len(set(ids).intersection(ground_truth[query_index][:topk])) / topk)
                latencies.append(elapsed)
            total = sum(latencies)
            report["single_query"][str(topk)] = {
                "queries": len(latencies), "recall": sum(recalls) / len(recalls),
                "recall_per_query": recalls,
                "qps": len(latencies) / total if total > 0 else None,
                "p50_s": percentile(latencies, 0.50),
                "p95_s": percentile(latencies, 0.95),
                "latencies_s": latencies,
            }
        report["id_validation"] = "all first/measurement queries returned the required number of legal unique IDs"


def initial_report(args):
    external = "not verified by this script; user or external harness must verify"
    return {
        "schema_version": 1, "purpose": "development measurement; not an official scorer",
        "status": "not_completed", "started_utc": datetime.now(timezone.utc).isoformat(),
        "mode": args.mode, "declared_commit": args.commit,
        "frozen_baseline_commit": BASELINE_SHA,
        "endpoint": {"host": args.host, "port": args.port, "database": args.database},
        "index": {"type": "hnsw", "lib": "vsag" if args.mode == "cpu" else "cuvs",
                  "distance": "l2", "m": 16, "ef_construction": 200, "ef_search": 200},
        "retained_table": {"database": args.database, "table": args.table,
                           "created_by_this_run": False, "cleanup": "manual; this script never drops tables"},
        "external_verification": {
            "server_binary_matches_declared_commit_and_clean_source": external,
            "fresh_server_restart_and_no_reusable_graph_or_index_cache": external,
            "same_server_resources_software_and_index_settings_across_runs": external,
            "amd_gpu_rocm_hipvs_environment": external if args.mode != "cpu" else "not applicable to CPU mode",
            "real_hipvs_cagra_build_without_cpu_fallback": external if args.mode != "cpu" else "not applicable to CPU mode",
        },
        "measurement_notes": [
            "Synthetic generation, exact ground truth, import, connection and service startup are outside build timing.",
            "SQL success and lib=cuvs are not evidence of GPU execution; collect separate backend/device evidence.",
            "Single-query latency includes execute and fetchall; QPS is count/sum(latencies), without concurrency or batching.",
            "Post-build query metrics exclude the first fixed query and include no extra warmup.",
            "Recall thresholds, baseline regression gates, batch performance and official scoring are not evaluated.",
            "Use identical data fingerprints and alternate three independently cold baseline/candidate runs externally.",
        ],
    }


def main(argv=None):
    args = parse_args(argv)  # --help works without numpy, pymysql or credentials.
    report = initial_report(args)
    connection = None
    exit_code = 1
    try:
        if "SEEKDB_PASSWORD" not in os.environ:
            raise MeasurementError("set SEEKDB_PASSWORD in the environment (an empty value is allowed)")
        import numpy as np
        import pymysql

        report["client"] = {"python": platform.python_version(), "platform": platform.platform(),
                            "numpy": np.__version__, "pymysql": pymysql.__version__}
        base, probes, ground_truth, dataset = make_workload(args, np)
        report["dataset"] = dataset
        print("Creating new development table %s.%s; it will be retained for manual cleanup."
              % (args.database, args.table), file=sys.stderr)
        connection = pymysql.connect(
            host=args.host, port=args.port, user=args.user,
            password=os.environ["SEEKDB_PASSWORD"], database=args.database,
            charset="utf8mb4", autocommit=True, connect_timeout=10,
            read_timeout=args.timeout, write_timeout=args.timeout,
        )
        measure(connection, args, base, probes, ground_truth, report)
        report["status"] = "completed"
        exit_code = 0
    except Exception as exc:
        report["status"] = "failed"
        report["error"] = "%s: %s" % (type(exc).__name__, exc)
    finally:
        if connection is not None:
            connection.close()
        if report["retained_table"]["created_by_this_run"]:
            report["retained_table"]["manual_cleanup_sql"] = "DROP TABLE `%s`.`%s`;" % (
                args.database, args.table)
        print(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
