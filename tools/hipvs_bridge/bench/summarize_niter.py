#!/usr/bin/env python3
"""Summarise a directory of bench_official.py runs (verify_niter_ab.sh output).

The niter=100 run is the frozen baseline's graph reproduced by the same binary,
so every other row is compared against it: the recall deltas are exactly what
the contest's 0.005 limit is about, and the speedup is what the build score is
computed from.
"""
import glob
import json
import os
import re
import sys

LIMIT = 0.005


def load(root):
    rows = []
    for path in glob.glob(os.path.join(root, "niter*.json")):
        m = re.search(r"niter(\d+)\.json$", path)
        if not m:
            continue
        with open(path) as f:
            r = json.load(f)
        r["niter"] = int(m.group(1))
        rows.append(r)
    rows.sort(key=lambda r: -r["niter"])
    return rows


def main(root):
    rows = load(root)
    if not rows:
        print("no niter*.json under %s" % root)
        return 1
    base = next((r for r in rows if r["niter"] == 100), None)
    if base is None:
        print("warning: no niter=100 run, deltas will be against the slowest run")
        base = max(rows, key=lambda r: r["e2e_s"])

    hdr = ("niter", "e2e_s", "speedup", "r@10", "d_r10", "r@100", "d_r100",
           "p50_k10", "qps_batch_k10", "mismatch", "verdict")
    print("%-6s %9s %8s %7s %7s %7s %7s %8s %14s %9s %s" % hdr)
    ok = []
    for r in rows:
        d10 = r["recall@10"] - base["recall@10"]
        d100 = r["recall@100"] - base["recall@100"]
        sp = base["e2e_s"] / r["e2e_s"] if r["e2e_s"] else 0.0
        bad = []
        if d10 < -LIMIT:
            bad.append("r10")
        if d100 < -LIMIT:
            bad.append("r100")
        if r.get("concurrent_mismatch"):
            bad.append("concurrency")
        if r.get("invalid_rows_k10") or r.get("invalid_rows_k100"):
            bad.append("ids")
        verdict = "PASS" if not bad else "FAIL(" + ",".join(bad) + ")"
        if not bad:
            ok.append(r)
        print("%-6d %9.2f %7.2fx %7.4f %+7.4f %7.4f %+7.4f %8.2f %14.1f %9d %s" % (
            r["niter"], r["e2e_s"], sp, r["recall@10"], d10, r["recall@100"], d100,
            r.get("p50_ms_k10", 0.0), r.get("qps_batch_k10", r.get("qps_single_k10", 0.0)),
            r.get("concurrent_mismatch", 0), verdict))

    print()
    if ok:
        best = min(ok, key=lambda r: r["e2e_s"])
        print("fastest budget that holds recall: niter=%d -> %.2fx (e2e %.2f s)" % (
            best["niter"], base["e2e_s"] / best["e2e_s"], best["e2e_s"]))
        if base["e2e_s"] / best["e2e_s"] >= 2.0:
            print("=> 2.0x build point reached with recall inside the 0.005 limit")
        else:
            print("=> below the 2.0x point; the next step is a build-side change, "
                  "not a cheaper budget")
    else:
        print("no budget holds recall: keep niter=100 (or 50)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
