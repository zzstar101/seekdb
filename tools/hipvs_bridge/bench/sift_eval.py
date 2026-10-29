#!/usr/bin/env python3
"""seekdb + CAGRA end-to-end measurement on a REAL dataset file pair.

Differences from bench_eval.py (which is kept untouched):
  * vectors come from <base.f32>/<query.f32> (raw little-endian float32) instead of
    being generated from a seed, so the numbers are attributable to a public dataset;
  * recall is checked against a precomputed exact ground truth (.npy) instead of
    being recomputed inline (4096 queries x 250K rows would dominate the run);
  * the query phase is a strict single-threaded nq=1 loop (one connection, one
    cursor, one query in flight) and Top10 and Top100 are measured in separate
    phases, so the bridge's per-topk cached search params are not rebuilt inside a
    timing loop;
  * every returned id is checked for legality, and the bridge shared object that the
    observer actually mapped is read back from /proc/<pid>/maps, so a mixed-library
    run cannot be reported as a clean one.

usage:
  sift_eval.py --data-dir D --n 250000 --dim 128 --nq 4096 --base /root/work/run/x \
               --port 3201 --gt D/gt.npy --out out.json [--topk 10,100] [--skip-start]
"""
import argparse
import json
import os
import subprocess
import time

import numpy as np
import pymysql

HOME = os.environ.get("SEEKDB_HOME", "")
SEEKDB = os.path.join(HOME, "bin", "seekdb")


def start_observer(base, port, lib_path):
    os.makedirs(base, exist_ok=True)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = lib_path
    log = open(base + ".out", "w")
    p = subprocess.Popen(
        [SEEKDB, "--base-dir=" + base, "--port=%d" % port, "--nodaemon",
         "--parameter", "_memory_budget=16G", "--parameter", "datafile_size=20G",
         "--parameter", "datafile_maxsize=40G", "--parameter", "log_disk_size=10G",
         "--parameter", "_max_syslog_file_count=10", "--log-level", "INFO"],
        stdout=log, stderr=log, env=env, cwd=base)
    for _ in range(180):
        if p.poll() is not None:
            raise RuntimeError("observer exited early, see %s.out" % base)
        try:
            c = pymysql.connect(host="127.0.0.1", port=port, user="root",
                                password="", autocommit=True)
            c.cursor().execute("select 1")
            return c, p
        except Exception:
            time.sleep(2)
    raise RuntimeError("observer did not start")


def bridge_maps(pid):
    try:
        with open("/proc/%d/maps" % pid) as f:
            return sorted({l.split()[-1] for l in f if "cuvs" in l or "rmm" in l})
    except Exception as e:
        return ["<maps unreadable: %s>" % e]


def lit(row):
    return "[" + ",".join("%.6f" % x for x in row) + "]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--nq", type=int, default=4096)
    ap.add_argument("--topk", default="10,100")
    ap.add_argument("--port", type=int, default=3201)
    ap.add_argument("--base", required=True)
    ap.add_argument("--rows-per-insert", type=int, default=1000)
    ap.add_argument("--gt", default="")
    ap.add_argument("--lib", default="cuvs")
    ap.add_argument("--lib-path", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--tag", default="run")
    a = ap.parse_args()

    topks = [int(x) for x in a.topk.split(",")]
    base_vecs = np.fromfile(os.path.join(a.data_dir, "base.f32"),
                            dtype=np.float32).reshape(a.n, a.dim)
    queries = np.fromfile(os.path.join(a.data_dir, "query.f32"),
                          dtype=np.float32).reshape(a.nq, a.dim)
    gt = (np.fromfile(a.gt, dtype=np.int32).reshape(a.nq, -1) if a.gt.endswith(".i32") else np.load(a.gt)) if a.gt else None

    conn, proc = start_observer(a.base, a.port, a.lib_path)
    cur = conn.cursor()
    cur.execute("select version()")
    version = cur.fetchone()[0]
    cur.execute("drop database if exists bench")
    cur.execute("create database bench")
    cur.execute("use bench")
    cur.execute("alter system set ob_vector_memory_limit_percentage=30")
    cur.execute("create table t(id int primary key, v vector(%d))" % a.dim)

    t_load = time.time()
    for s in range(0, a.n, a.rows_per_insert):
        e = min(s + a.rows_per_insert, a.n)
        sql = "insert into t values " + ",".join(
            "(%d,'%s')" % (i, lit(base_vecs[i])) for i in range(s, e))
        cur.execute(sql)
    load_s = time.time() - t_load

    t0 = time.time()
    cur.execute("create vector index idx on t(v) with (distance=l2, type=hnsw, lib=%s, m=16, ef_construction=200, ef_search=64)"
                % a.lib)
    t1 = time.time()
    create_s = t1 - t0

    q0 = lit(queries[0])
    cur.execute("select id from t order by l2_distance(v,'%s') approximate limit 10" % q0)
    first_ids = [int(r[0]) for r in cur.fetchall()]
    t2 = time.time()
    first_query_s = t2 - t1

    res = {"tag": a.tag, "version": version, "n": a.n, "dim": a.dim, "nq": a.nq,
           "lib": a.lib, "load_s": round(load_s, 3), "create_s": round(create_s, 3),
           "first_query_s": round(first_query_s, 3),
           "e2e_s": round(create_s + first_query_s, 3),
           "first_ids_head": first_ids[:10],
           "bridge_maps": bridge_maps(proc.pid),
           "env": {k: os.environ.get(k) for k in
                   ("SEEKDB_CUVS_NITER", "SEEKDB_CUVS_GRAPH_DEGREE",
                    "SEEKDB_CUVS_INTER_DEGREE", "SEEKDB_CUVS_ITOPK",
                    "SEEKDB_CUVS_SEARCH_WIDTH")}}
    print(json.dumps({k: res[k] for k in ("tag", "version", "load_s", "create_s",
                                          "first_query_s", "e2e_s")}), flush=True)
    print("bridge_maps=%s" % (res["bridge_maps"],), flush=True)

    lat_all = {}
    for k in topks:
        lat, ids, empty = [], np.zeros((a.nq, k), dtype=np.uint32), 0
        # warm the params object for this topk once, outside the timed loop
        cur.execute("select id from t order by l2_distance(v,'%s') approximate limit %d"
                    % (q0, k))
        cur.fetchall()
        for i in range(a.nq):
            qq = lit(queries[i])
            tt = time.perf_counter()
            cur.execute("select id from t order by l2_distance(v,'%s') approximate limit %d"
                        % (qq, k))
            rows = cur.fetchall()
            lat.append((time.perf_counter() - tt) * 1000.0)
            if not rows:
                empty += 1
            else:
                ids[i, :len(rows)] = [int(r[0]) for r in rows[:k]]
        lat = np.asarray(lat)
        lat.tofile(os.path.join(a.data_dir, "%s_top%d_ms.f32" % (a.tag, k)))
        ids.tofile(os.path.join(a.data_dir, "%s_top%d_ids.bin" % (a.tag, k)))
        entry = {"p50_ms": round(float(np.percentile(lat, 50)), 3),
                 "p95_ms": round(float(np.percentile(lat, 95)), 3),
                 "mean_ms": round(float(lat.mean()), 3),
                 "min_ms": round(float(lat.min()), 3),
                 "max_ms": round(float(lat.max()), 3),
                 "qps": round(1000.0 / float(lat.mean()), 1),
                 "first10_ms": [round(float(x), 3) for x in lat[:10]],
                 "empty": empty,
                 "illegal_ids": int((ids >= a.n).sum())}
        if gt is not None:
            h = 0
            for i in range(a.nq):
                h += len(set(int(x) for x in ids[i]) & set(int(x) for x in gt[i, :k]))
            entry["recall@%d" % k] = round(h / (a.nq * k), 4)
            entry["dup_rate"] = round(
                float(np.mean([1.0 - len(set(ids[i].tolist())) / k for i in range(a.nq)])), 6)
        lat_all["top%d" % k] = entry
        print(json.dumps({"topk": k, **entry}), flush=True)
    res["query"] = lat_all

    conn.close()
    proc.terminate()
    try:
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    if a.out:
        with open(a.out, "w") as f:
            json.dump(res, f, indent=2)
    print("WROTE %s" % a.out, flush=True)


if __name__ == "__main__":
    main()
