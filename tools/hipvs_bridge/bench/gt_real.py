#!/usr/bin/env python3
"""Exact L2 ground truth for a raw <base.f32> / <query.f32> pair, with explicit paths.

Same math as make_gt_fast.py (|b|^2 - 2 q.b, argpartition top-K) but the paths are
arguments instead of being hard-coded, and it is chunked so 250K x 4096 does not
need a 4 GB temporary.

usage: gt_real.py base.f32 n dim query.f32 nq K out.npy
"""
import sys

import numpy as np

base_f, n, dim, q_f, nq, K, out_f = sys.argv[1:8]
n, dim, nq, K = int(n), int(dim), int(nq), int(K)

base = np.fromfile(base_f, dtype=np.float32).reshape(n, dim)
q = np.fromfile(q_f, dtype=np.float32).reshape(nq, dim)

bn = np.einsum("ij,ij->i", base, base, dtype=np.float64).astype(np.float32)
out = np.zeros((nq, K), dtype=np.int32)
step = 128
for s in range(0, nq, step):
    e = min(s + step, nq)
    d = bn[None, :] - 2.0 * (q[s:e] @ base.T)
    idx = np.argpartition(d, K, axis=1)[:, :K]
    dv = np.take_along_axis(d, idx, axis=1)
    order = np.argsort(dv, axis=1)
    out[s:e] = np.take_along_axis(idx, order, axis=1)
np.save(out_f, out.astype(np.int64))
print("gt done n=%d dim=%d nq=%d K=%d -> %s" % (n, dim, nq, K, out_f))
