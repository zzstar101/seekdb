#!/usr/bin/env python3
"""Functional smoke: lib=vsag still works; lib=cuvs builds via the GPU graph path,
returns k distinct legal ids (small n -> cuvsCagraBuild path, large n -> GPU
graph path), and the persisted snapshot reloads after an observer restart.

usage: SEEKDB_HOME=... func_smoke.py --base DIR --port P --lib-path LIBDIR
"""
import argparse
import sys
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sift_eval import start_observer, lit  # noqa: E402


def stop(conn, proc):
    try:
        conn.close()
    except Exception:
        pass
    proc.terminate()
    try:
        proc.wait(60)
    except Exception:
        proc.kill()
        proc.wait()


def knn(cur, table, q, k):
    cur.execute("SELECT id FROM %s ORDER BY l2_distance(v, '%s') APPROXIMATE LIMIT %d" % (table, lit(q), k))
    return [r[0] for r in cur.fetchall()]


def load(cur, table, x, idx_with):
    cur.execute("DROP TABLE IF EXISTS %s" % table)
    cur.execute("CREATE TABLE %s (id INT PRIMARY KEY, v VECTOR(%d))" % (table, x.shape[1]))
    for s in range(0, len(x), 1000):
        rows = ",".join("(%d,'%s')" % (i, lit(x[i])) for i in range(s, min(s + 1000, len(x))))
        cur.execute("INSERT INTO %s VALUES %s" % (table, rows))
    t = time.time()
    cur.execute("CREATE VECTOR INDEX idx_%s ON %s(v) WITH (%s)" % (table, table, idx_with))
    return time.time() - t


def check(cur, table, x, qs, k, tag):
    bad = 0
    rec = 0.0
    for q in qs:
        ids = knn(cur, table, q, k)
        if len(ids) != k or len(set(ids)) != k or min(ids) < 0 or max(ids) >= len(x):
            bad += 1
        d = ((x - q) ** 2).sum(1)
        gt = set(np.argsort(d, kind="stable")[:k].tolist())
        rec += len(gt & set(ids)) / k
    print("%s: queries=%d bad=%d recall@%d=%.4f" % (tag, len(qs), bad, k, rec / len(qs)), flush=True)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--port", type=int, default=3401)
    ap.add_argument("--lib-path", default="")
    a = ap.parse_args()
    rng = np.random.default_rng(1)
    small = rng.standard_normal((800, 32)).astype(np.float32)
    big = rng.standard_normal((20000, 64)).astype(np.float32)
    qs_s = small[:20] + 0.01
    qs_b = big[:30] + 0.01
    p = "distance=l2, type=hnsw, lib=%s, m=16, ef_construction=200, ef_search=64"
    conn, proc = start_observer(a.base, a.port, a.lib_path)
    cur = conn.cursor()
    cur.execute("CREATE DATABASE IF NOT EXISTS smoke")
    cur.execute("USE smoke")
    bad = 0
    print("vsag create %.2fs" % load(cur, "tv", small, p % "vsag"), flush=True)
    bad += check(cur, "tv", small, qs_s, 10, "vsag")
    print("cuvs small create %.2fs" % load(cur, "tcs", small, p % "cuvs"), flush=True)
    bad += check(cur, "tcs", small, qs_s, 10, "cuvs-small")
    print("cuvs big create %.2fs" % load(cur, "tcb", big, p % "cuvs"), flush=True)
    bad += check(cur, "tcb", big, qs_b, 10, "cuvs-big")
    bad += check(cur, "tcb", big, qs_b, 100, "cuvs-big")
    stop(conn, proc)
    conn, proc = start_observer(a.base, a.port, a.lib_path)
    cur = conn.cursor()
    cur.execute("USE smoke")
    bad += check(cur, "tv", small, qs_s, 10, "vsag-after-restart")
    bad += check(cur, "tcs", small, qs_s, 10, "cuvs-small-after-restart")
    bad += check(cur, "tcb", big, qs_b, 10, "cuvs-big-after-restart")
    stop(conn, proc)
    print("FUNC_SMOKE %s" % ("PASS" if bad == 0 else "FAIL"))


if __name__ == "__main__":
    main()
