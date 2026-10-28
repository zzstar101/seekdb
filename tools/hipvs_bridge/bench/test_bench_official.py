#!/usr/bin/env python3
"""Offline self-test for the contest bench harness.

The benchmark itself needs a running seekdb and a GPU, so it cannot prove here
that the numbers are right; what it can prove is that the *measurement* is not
lying: the ground truth really is the exact L2 top-k, the recall routine scores
a perfect answer as 1.0 and a degraded answer below it, the result checker
rejects duplicate / out-of-range ids, and the fvecs/bvecs readers agree with
what was written.

Run:  python3 test_bench_official.py
"""
import os
import struct
import sys
import tempfile
import types

import numpy as np

# bench_official imports the runtime helpers (pymysql, bench_eval); stub them so
# the pure functions can be exercised without a database.
sys.modules.setdefault("pymysql", types.SimpleNamespace())
if "bench_eval" not in sys.modules:
    sys.modules["bench_eval"] = types.SimpleNamespace(
        lit=lambda s: "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'",
        start_observer=lambda *a, **k: None,
    )

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_official as B  # noqa: E402


def naive_topk(base, queries, k):
    """Reference implementation: full sort, no blocking, no argpartition."""
    out = np.empty((queries.shape[0], k), dtype=np.int64)
    for i, q in enumerate(queries):
        d = ((base - q) ** 2).sum(axis=1)
        out[i] = np.argsort(d, kind="stable")[:k]
    return out


def write_fvecs(path, m):
    with open(path, "wb") as f:
        for row in m:
            f.write(struct.pack("<i", row.shape[0]))
            f.write(row.astype(np.float32).tobytes())


def write_bvecs(path, m):
    with open(path, "wb") as f:
        for row in m:
            f.write(struct.pack("<i", row.shape[0]))
            f.write(row.astype(np.uint8).tobytes())


def main():
    rng = np.random.default_rng(7)
    n, dim, nq, k = 1500, 24, 40, 10
    base = rng.standard_normal((n, dim)).astype(np.float32)
    queries = rng.standard_normal((nq, dim)).astype(np.float32)

    # 1) ground truth == exhaustive sort (ids, and distance-ordered)
    gt = B.exact_topk(base, queries, k, chunk=7)  # odd chunk to exercise the loop
    ref = naive_topk(base, queries, k)
    same_ids = np.array_equal(gt, ref)
    # ties aside, the returned ids must be non-decreasing in distance
    d = ((queries[:, None, :] - base[gt]) ** 2).sum(axis=2)
    ordered = bool(np.all(d[:, 1:] >= d[:, :-1] - 1e-5))
    assert same_ids, "exact_topk disagrees with the exhaustive sort"
    assert ordered, "exact_topk returned ids that are not in distance order"

    # 2) recall: perfect answer scores 1.0, a degraded answer scores below it
    assert B.recall_at(gt, gt, k) == 1.0
    worse = gt.copy()
    worse[:, : k // 2] = (worse[:, : k // 2] + 1) % n  # replace half the ids
    r_worse = B.recall_at(gt, worse, k)
    assert 0.3 < r_worse < 0.7, f"recall of a half-right answer was {r_worse}"

    # 3) result validation rejects the shapes the evaluator would reject
    good = np.arange(k, dtype=np.int64).reshape(1, k)
    assert B.check_rows(good, k, n) == 0, "a valid result row was rejected"
    bad_len = np.arange(k - 1, dtype=np.int64).reshape(1, k - 1)
    dup = np.zeros((1, k), dtype=np.int64)
    oor = np.full((1, k), n + 5, dtype=np.int64)
    assert B.check_rows(bad_len, k, n) == 1, "short row accepted"
    assert B.check_rows(dup, k, n) == 1, "duplicate ids accepted"
    assert B.check_rows(oor, k, n) == 1, "out-of-range id accepted"

    # 4) fvecs / bvecs readers round-trip
    with tempfile.TemporaryDirectory() as td:
        fp = os.path.join(td, "x.fvecs")
        write_fvecs(fp, base[:17])
        got = B.load_vecs(fp)
        assert got.shape == (17, dim) and np.allclose(got, base[:17])

        bp = os.path.join(td, "x.bvecs")
        u8 = rng.integers(0, 256, size=(11, dim)).astype(np.uint8)
        write_bvecs(bp, u8)
        got8 = B.load_vecs(bp)
        assert got8.shape == (11, dim) and np.array_equal(got8, u8.astype(np.float32))

        trunc = os.path.join(td, "x.fvecs")
        got_lim = B.load_vecs(trunc, limit=5)
        assert got_lim.shape == (5, dim), "limit= was not honoured"

    print("bench_official self-test: OK "
          f"(exact_topk==exhaustive on {n}x{dim}/{nq}q, recall/validation/readers checked)")


if __name__ == "__main__":
    main()
