#!/usr/bin/env python3
"""Benchmark the seekdb hipVS/cuVS CAGRA path with the contest protocol.

Differences to bench_eval.py (which is the quick development loop):
  * index DDL uses the contest parameters (m=16, ef_construction=200) and the
    query session uses ob_hnsw_ef_search=64, exactly like the evaluator;
  * top-10 and top-100 are measured separately for latency and for throughput,
    because the query score is computed from both;
  * every result row is validated (exactly k ids, unique, inside [0, n));
  * recall is measured against an exact ground truth on the same vectors;
  * --dim 256 is supported so the second official dataset shape can be run.

Example (250k x 128, 4096 queries, 8 client threads):
  python3 bench_official.py --n 250000 --dim 128 --queries 4096 --threads 8
"""
import argparse
import json
import os
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pymysql

from bench_eval import lit, start_observer


def exact_topk(base_vecs, queries, k, chunk=256):
    """Exact L2 top-k (ids only).  Chunked so 250k x 4096 never materialises."""
    n = base_vecs.shape[0]
    sq = (base_vecs * base_vecs).sum(axis=1)
    out = np.empty((queries.shape[0], k), dtype=np.int64)
    for s in range(0, queries.shape[0], chunk):
        e = min(s + chunk, queries.shape[0])
        q = queries[s:e]
        d = -2.0 * (q @ base_vecs.T)
        d += sq
        if k < n:
            part = np.argpartition(d, k - 1, axis=1)[:, :k]
        else:
            part = np.tile(np.arange(n), (e - s, 1))
        rows = np.arange(e - s)[:, None]
        vals = d[rows, part]
        order = np.argsort(vals, axis=1)
        out[s:e] = part[rows, order]
    return out


def recall_at(gt, got, k):
    hit = 0
    for i in range(len(got)):
        ref = set(gt[i, :k].tolist())
        hit += len(ref.intersection(got[i][:k]))
    return hit / float(len(got) * k)


def check_rows(rows, k, n):
    bad = 0
    for r in rows:
        if len(r) != k or len(set(r)) != k or min(r) < 0 or max(r) >= n:
            bad += 1
    return bad


def knn_ids(cur, q, k):
    cur.execute(
        "select id from t order by l2_distance(v,'%s') approximate limit %d" % (lit(q), k))
    return [int(r[0]) for r in cur.fetchall()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=4096)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--lib", default="cuvs")
    ap.add_argument("--m", type=int, default=16)
    ap.add_argument("--ef-construction", type=int, default=200)
    ap.add_argument("--ef-search", type=int, default=64)
    ap.add_argument("--port", type=int, default=2881)
    ap.add_argument("--base", default="/root/work/run/eval")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--norm", action="store_true",
                    help="unit-normalise rows (the official demo data is normalised)")
    ap.add_argument("--gt-cache", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    base_vecs = rng.standard_normal((args.n, args.dim)).astype(np.float32)
    queries = rng.standard_normal((args.queries, args.dim)).astype(np.float32)
    if args.norm:
        base_vecs /= np.linalg.norm(base_vecs, axis=1, keepdims=True)
        queries /= np.linalg.norm(queries, axis=1, keepdims=True)

    gt_path = args.gt_cache or "gt_%d_%d_%d_%d%s.npz" % (
        args.n, args.dim, args.queries, args.seed, "_norm" if args.norm else "")
    if os.path.exists(gt_path):
        gt = np.load(gt_path)["gt"]
    else:
        gt = exact_topk(base_vecs, queries, 100)
        np.savez(gt_path, gt=gt)

    conn = start_observer(args.base, args.port)
    cur = conn.cursor()
    cur.execute("select version()")
    version = cur.fetchone()[0]
    cur.execute("drop database if exists bench")
    cur.execute("create database bench")
    cur.execute("use bench")
    cur.execute("alter system set ob_vector_memory_limit_percentage=30")
    cur.execute("create table t(id int primary key, v vector(%d))" % args.dim)

    t0 = time.time()
    for s in range(0, args.n, 2000):
        e = min(s + 2000, args.n)
        cur.execute("insert into t values " + ",".join(
            "(%d,'%s')" % (i, lit(base_vecs[i])) for i in range(s, e)))
    load_s = time.time() - t0

    ddl = ("create vector index idx on t(v) with (distance=l2, type=hnsw, lib=%s, "
           "m=%d, ef_construction=%d)" % (args.lib, args.m, args.ef_construction))
    t_create0 = time.time()
    cur.execute(ddl)
    create_s = time.time() - t_create0

    try:
        cur.execute("set session ob_hnsw_ef_search=%d" % args.ef_search)
    except Exception:
        cur.execute("alter system set ob_hnsw_ef_search=%d" % args.ef_search)

    t_first0 = time.time()
    knn_ids(cur, queries[0], 10)
    first_query_s = time.time() - t_first0

    result = {
        "version": version,
        "n": args.n, "dim": args.dim, "lib": args.lib,
        "m": args.m, "ef_construction": args.ef_construction, "ef_search": args.ef_search,
        "load_s": round(load_s, 3), "create_s": round(create_s, 3),
        "first_query_s": round(first_query_s, 3),
        "e2e_s": round(create_s + first_query_s, 3),
    }

    # --- single-stream latency + recall (top-10 and top-100 separately) ---
    for k in (10, 100):
        lat, rows = [], []
        for i in range(args.queries):
            t = time.time()
            ids = knn_ids(cur, queries[i], k)
            lat.append(time.time() - t)
            rows.append(ids)
        lat = np.array(lat) * 1000.0
        result["p50_ms_k%d" % k] = round(float(np.percentile(lat, 50)), 3)
        result["p95_ms_k%d" % k] = round(float(np.percentile(lat, 95)), 3)
        result["qps_single_k%d" % k] = round(args.queries / (lat.sum() / 1000.0), 1)
        result["recall@%d" % k] = round(recall_at(gt, rows, k), 4)
        result["invalid_rows_k%d" % k] = check_rows(rows, k, args.n)

    # --- concurrent batch throughput (client-side batch, like the evaluator) ---
    def worker(k, shard):
        c = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                            password="", autocommit=True)
        cu = c.cursor()
        cu.execute("use bench")
        for i in shard:
            knn_ids(cu, queries[i], k)
        c.close()

    for k in (10, 100):
        shards = [list(range(i, args.queries, args.threads)) for i in range(args.threads)]
        t = time.time()
        with ThreadPoolExecutor(max_workers=args.threads) as ex:
            list(ex.map(lambda s: worker(k, s), shards))
        dur = time.time() - t
        result["qps_batch_k%d" % k] = round(args.queries / dur, 1)

    out = args.out or "official_%d_%d.json" % (args.n, args.dim)
    with open(out, "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
