#!/usr/bin/env bash
# Reproduce the contest A/B measurement: frozen V5 baseline vs. candidate build,
# on the two scoring shapes (250K x 128 and 250K x 256) with the official SQL
# protocol from bench_official.py.
#
#   BASE_HOME=/root/work/base_home CAND_HOME=/root/work/cand_home \
#     ./run_matrix.sh
#
# Each *_HOME must contain bin/seekdb and bridge/libseekdb_cuvs_bridge.so.
# Optional: N, Q, DIMS, OUT, NORM (set NORM="" to disable row normalisation),
#           DATA_FILE / QUERY_FILE to run on real contest vectors instead of
#           the synthetic generator.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BASE_HOME=${BASE_HOME:-/root/work/base_home}
CAND_HOME=${CAND_HOME:-/root/work/cand_home}
OUT=${OUT:-/root/work/run/matrix}
PY=${PY:-/opt/venv/bin/python}
N=${N:-250000}
Q=${Q:-4096}
DIMS=${DIMS:-128 256}
NORM=${NORM---norm}
DATA_FILE=${DATA_FILE:-}
QUERY_FILE=${QUERY_FILE:-}
mkdir -p "$OUT"

run_one() {  # home dim tag port
  local home=$1 dim=$2 tag=$3 port=$4
  local base="$OUT/base_${tag}_${dim}"
  rm -rf "$base" "$base.out"
  SEEKDB_HOME="$home" "$PY" "$HERE/bench_official.py" \
    --n "$N" --dim "$dim" --queries "$Q" --port "$port" --base "$base" \
    ${NORM:+"$NORM"} ${DATA_FILE:+--data-file "$DATA_FILE"} \
    ${QUERY_FILE:+--query-file "$QUERY_FILE"} \
    --out "$OUT/res_${tag}_${dim}.json" 2>&1 | tail -1
}

for d in $DIMS; do
  run_one "$BASE_HOME" "$d" base $((2910 + d))
  run_one "$CAND_HOME" "$d" cand $((2920 + d))
done

"$PY" - "$OUT" <<'PYEOF'
import glob, json, os, sys
out = sys.argv[1]
rows = {}
for f in sorted(glob.glob(os.path.join(out, "res_*.json"))):
    r = json.load(open(f))
    tag = "base" if os.path.basename(f).startswith("res_base") else "cand"
    rows.setdefault(r["dim"], {})[tag] = r

print("\n| dim | metric | baseline | candidate | ratio |")
print("| --- | --- | --- | --- | --- |")
for dim in sorted(rows):
    b, c = rows[dim].get("base"), rows[dim].get("cand")
    if not b or not c:
        continue
    for key, name, better in (
        ("e2e_s", "build e2e (s)", "lower"),
        ("p50_ms_k10", "p50 top-10 (ms)", "lower"),
        ("qps_batch_k100", "batch QPS top-100", "higher"),
        ("recall@10", "recall@10", "higher"),
        ("recall@100", "recall@100", "higher"),
        ("invalid_rows_k10", "invalid rows", "lower"),
    ):
        bv, cv = b.get(key), c.get(key)
        if bv in (None, 0) and cv in (None, 0):
            continue
        ratio = (bv / cv) if cv else 0.0
        print("| %d | %s | %s | %s | %.2fx |" % (dim, name, bv, cv, ratio))
PYEOF
