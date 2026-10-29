#!/usr/bin/env python3
"""Recall + id-legality check for a dumped uint32 id file.

usage: rec_real.py gt.npy ids.bin nq topk [n_rows]

Prints recall@10/@100 plus, when n_rows is given, how many returned ids fall
outside [0, n_rows) (an illegal id means the position->primary-key mapping or the
result buffer is wrong, and would silently deflate recall).
"""
import sys

import numpy as np

gt = np.load(sys.argv[1])
ids = np.fromfile(sys.argv[2], dtype=np.uint32)
nq, topk = int(sys.argv[3]), int(sys.argv[4])
n_rows = int(sys.argv[5]) if len(sys.argv) > 5 else None

ids = ids.reshape(nq, -1)
if ids.shape[1] < topk:
    print("ERR ids have %d columns, need %d" % (ids.shape[1], topk))
    sys.exit(1)

res = {}
for k in (10, 100):
    if k > topk:
        continue
    h = 0
    for i in range(nq):
        got = set(int(x) for x in ids[i, :k])
        want = set(int(x) for x in gt[i, :k])
        h += len(got & want)
    res["recall@%d" % k] = h / (nq * k)

dup = float(np.mean([1.0 - len(set(ids[i, :topk].tolist())) / topk for i in range(nq)]))
out = dict(res)
out["dup_rate@%d" % topk] = round(dup, 6)
if n_rows is not None:
    bad = int((ids[:, :topk] >= n_rows).sum())
    out["illegal_ids"] = bad
    out["legal"] = bad == 0
print(" ".join("%s=%s" % (k, v if isinstance(v, str) else round(v, 6)) for k, v in out.items()))
