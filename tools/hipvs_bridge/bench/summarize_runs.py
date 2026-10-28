#!/usr/bin/env python3
"""Compare bench_official.py runs that differ by bridge knobs.

Each run is one JSON file named after its label.  The reference run (default:
the first CONFIGS entry, normally the frozen-baseline reproduction with
SEEKDB_CUVS_NITER=100) is what the contest compares against, so the table shows
build speedup and the recall delta that the 0.005 gate is about.

  python3 summarize_runs.py /root/work/ab --ref base
"""
import argparse
import glob
import json
import os

LIMIT = 0.005


def load(root):
    rows = []
    for path in sorted(glob.glob(os.path.join(root, "*.json"))):
        if path.endswith(".prof.json"):
            continue
        try:
            with open(path) as f:
                r = json.load(f)
        except (OSError, ValueError):
            continue
        if "e2e_s" not in r:
            continue
        r["label"] = os.path.splitext(os.path.basename(path))[0]
        rows.append(r)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--ref", default="base")
    a = ap.parse_args()

    rows = load(a.root)
    if not rows:
        print("no run json under %s" % a.root)
        return 1
    ref = next((r for r in rows if r["label"] == a.ref), None)
    if ref is None:
        ref = rows[0]
        print("note: no run labelled %r, using %r as the reference\n" % (a.ref, ref["label"]))

    print("%-12s %5s %9s %8s %7s %7s %7s %7s %8s %12s %s" % (
        "label", "niter", "e2e_s", "speedup", "r@10", "d_r10", "r@100", "d_r100",
        "p50_k10", "qps_batch_10", "verdict"))
    ok = []
    for r in rows:
        d10 = r["recall@10"] - ref["recall@10"]
        d100 = r["recall@100"] - ref["recall@100"]
        sp = ref["e2e_s"] / r["e2e_s"] if r["e2e_s"] else 0.0
        bad = []
        if r is not ref:
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
        print("%-12s %5s %9.2f %7.2fx %7.4f %+7.4f %7.4f %+7.4f %8.2f %12.1f %s" % (
            r["label"], r.get("niter", "-"), r["e2e_s"], sp, r["recall@10"], d10,
            r["recall@100"], d100, r.get("p50_ms_k10", 0.0),
            r.get("qps_batch_k10", r.get("qps_single_k10", 0.0)), verdict))

    print()
    if ok:
        best = min(ok, key=lambda r: r["e2e_s"])
        sp = ref["e2e_s"] / best["e2e_s"] if best["e2e_s"] else 0.0
        print("fastest configuration that holds recall: %s -> %.2fx (e2e %.2f s)" % (
            best["label"], sp, best["e2e_s"]))
        print("=> %s the 2.0x build point" % ("MEETS" if sp >= 2.0 else "does NOT meet"))
    else:
        print("no configuration holds recall")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
