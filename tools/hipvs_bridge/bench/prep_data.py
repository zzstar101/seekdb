#!/usr/bin/env python3
"""Turn an ann-benchmarks HDF5 file into the raw f32 pair + exact GT the probe needs.

  prep_data.py <in.hdf5> <outdir> <n> <nq> <K> [--norm]

Writes <outdir>/base.f32 (n x dim, row-major little-endian float32),
<outdir>/query.f32, and <outdir>/gt.npy (nq x K exact L2 top-K row indices).

--norm unit-normalises both sides. For unit vectors |a-b|^2 = 2 - 2 a.b, so the L2
ranking is exactly the angular/cosine ranking; that is how an angular dataset
(nytimes-256-angular) becomes a well-posed L2 problem instead of a silently
different metric. It is always recorded in <outdir>/meta.json so the provenance of a
number can never be lost.

The GT is recomputed against the n-row subset, NOT taken from the file: the file's
neighbours are top-K over the full original train split, which is a different
candidate set once a subset is used.
"""
import json
import sys

import h5py
import numpy as np

src, outdir, n, nq, K = sys.argv[1:6]
n, nq, K = int(n), int(nq), int(K)
norm = "--norm" in sys.argv

with h5py.File(src, "r") as f:
    keys = list(f.keys())
    base = np.asarray(f["train"][:n], dtype=np.float32)
    query = np.asarray(f["test"][:nq], dtype=np.float32)
    dist_attr = f.attrs.get("distance", "?")

if base.shape[0] < n or query.shape[0] < nq:
    raise SystemExit("file too small: %s %s" % (base.shape, query.shape))
dim = base.shape[1]

zero_rows = {"base": 0, "query": 0}
if norm:
    # Safe normalisation: nytimes-256-angular contains all-zero rows (200/250000 base,
    # 4/4096 query). Dividing those by 0 would fill the file with NaN. A zero row is
    # left as a zero row: it is a legal float32 vector, and its exact L2 distance to any
    # unit vector is exactly 1, so it simply ranks last (and the zero queries, if any,
    # are nearest to the zero base rows at distance 0). Nothing is special-cased after
    # this point, and the counts are recorded in meta.json.
    for name, a in (("base", base), ("query", query)):
        nrm = np.linalg.norm(a, axis=1, keepdims=True)
        zero_rows[name] = int((nrm == 0).sum())
        a /= np.where(nrm == 0.0, 1.0, nrm)

base.tofile(outdir + "/base.f32")
query.tofile(outdir + "/query.f32")

gt = np.zeros((nq, K), dtype=np.int32)
bn = np.einsum("ij,ij->i", base, base, dtype=np.float64).astype(np.float32)
step = 128
for s in range(0, nq, step):
    e = min(s + step, nq)
    d = bn[None, :] - 2.0 * (query[s:e] @ base.T)
    idx = np.argpartition(d, K, axis=1)[:, :K]
    dv = np.take_along_axis(d, idx, axis=1)
    order = np.argsort(dv, axis=1)
    gt[s:e] = np.take_along_axis(idx, order, axis=1)
np.save(outdir + "/gt.npy", gt.astype(np.int64))

meta = {"source_file": src.split("/")[-1], "hdf5_keys": keys,
        "hdf5_distance_attr": str(dist_attr), "n": n, "dim": dim, "nq": nq, "K": K,
        "normalised": bool(norm),
        "zero_rows_left_as_zero": zero_rows,
        "subset": "first %d rows of the train split, first %d rows of the test split" % (n, nq),
        "gt": "exact L2 top-K recomputed against the %d-row subset (float32, "
              "argpartition + stable argsort)" % n,
        "metric_note": ("unit-normalised, so L2 ranking == angular ranking"
                        if norm else "raw L2")}
with open(outdir + "/meta.json", "w") as f:
    json.dump(meta, f, indent=2)
print(json.dumps(meta))
