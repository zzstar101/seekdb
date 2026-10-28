#!/usr/bin/env python3
"""Decompose one contest query into its cost bands.

The build score is settled by the NN-descent budget; the query score is capped
by everything that happens around the GPU call.  This script measures the bands
separately on a warm server so the next optimisation targets a measured number
instead of a guess:

  band 0  SELECT 1                     client + parse + plan + network floor
  band 1  point lookup by primary key  storage round trip for one row
  band 2  kNN top-10 through the index index scan + GPU search + row fetches
  band 3  the same query at top-100    how much of band 2 is per-result-row work

Run it against an already built base directory (see prof_query.sh).
"""
import argparse
import json
import statistics
import time

import pymysql

TABLE = "t"
VEC = "v"


def timed(fn, n):
    lat = []
    out = None
    for _ in range(n):
        t = time.perf_counter()
        out = fn()
        lat.append((time.perf_counter() - t) * 1000.0)
    lat.sort()
    return {
        "n": n,
        "p50_ms": round(statistics.median(lat), 3),
        "p95_ms": round(lat[int(0.95 * (len(lat) - 1))], 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=2881)
    ap.add_argument("--n", type=int, default=1000)
    ap.add_argument("--queries", type=int, default=500)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--lib", default="cuvs")
    ap.add_argument("--ef-search", default="64")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    cur = pymysql.connect(host="127.0.0.1", port=a.port, user="root", password="",
                          autocommit=True).cursor()
    cur.execute("set session ob_hnsw_ef_search=%s" % a.ef_search)

    bands = {}
    bands["band0_select1"] = timed(lambda: cur.execute("SELECT 1"), a.queries)
    bands["band1_point_lookup"] = timed(
        lambda: cur.execute("SELECT id FROM %s WHERE id = 42" % TABLE), a.queries)

    vec10 = "[" + ",".join(["0.01"] * a.dim) + "]"
    knn10 = ("select id from %s order by l2_distance(%s,'%s') approximate limit 10"
             % (TABLE, VEC, vec10))
    knn100 = ("select id from %s order by l2_distance(%s,'%s') approximate limit 100"
              % (TABLE, VEC, vec10))
    bands["band2_knn10"] = timed(lambda: cur.execute(knn10), a.queries)
    bands["band3_knn100"] = timed(lambda: cur.execute(knn100), a.queries)

    b0 = bands["band0_select1"]["p50_ms"]
    b2 = bands["band2_knn10"]["p50_ms"]
    b3 = bands["band3_knn100"]["p50_ms"]
    bands["share_of_knn10"] = {
        "client_and_parse": round(100.0 * b0 / b2, 1) if b2 else 0.0,
        "index_and_fetch": round(100.0 * (b2 - b0) / b2, 1) if b2 else 0.0,
    }
    bands["per_result_row_us"] = round(1000.0 * (b3 - b2) / 90.0, 2)

    print(json.dumps(bands, indent=1))
    print()
    print("band 2 is the only band this project can change: %.2f ms of which "
          "%.1f%% is client/parse floor." % (b2, bands["share_of_knn10"]["client_and_parse"]))
    print("each extra result row costs %.1f us (from the top-10 vs top-100 gap)" %
          bands["per_result_row_us"])
    if a.out:
        with open(a.out, "w") as f:
            json.dump(bands, f, indent=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
