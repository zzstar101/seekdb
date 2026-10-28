#!/usr/bin/env bash
# Settle the NN-descent budget question on a real machine with one command.
#
#   SEEKDB_HOME=/root/work/cand_home ./verify_niter_ab.sh                 # 250k x 128
#   SEEKDB_HOME=/root/work/cand_home N=250000 DIM=256 ./verify_niter_ab.sh # 250k x 256
#   NITERS="100 50 30" DATA_FILE=/data/sift/sift_base.fvecs \
#   QUERY_FILE=/data/sift/sift_query.fvecs ./verify_niter_ab.sh
#
# Every run rebuilds the index from scratch in a fresh base directory with the
# same vectors, the contest DDL settings (m=16, ef_construction=200) and the
# contest query session setting (ob_hnsw_ef_search=64).  The budget is the only
# thing that changes between runs, and niter=100 is what the frozen baseline
# hard-codes, so the deltas printed at the end are exactly the recall deltas the
# 0.005 gate is about and the speedup is what the build score is computed from.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PY:-/opt/venv/bin/python}"
N="${N:-250000}"
DIM="${DIM:-128}"
Q="${Q:-4096}"
THREADS="${THREADS:-8}"
PORT="${PORT:-2881}"
NITERS="${NITERS:-100 50 40 30}"
ROOT="${ROOT:-/root/work/ab-${N}x${DIM}}"
SEEKDB_HOME="${SEEKDB_HOME:-/seekdb_workspace}"
DATA_FILE="${DATA_FILE:-}"
QUERY_FILE="${QUERY_FILE:-}"
export SEEKDB_HOME

if [ ! -x "$SEEKDB_HOME/bin/seekdb" ]; then
  echo "no seekdb binary under SEEKDB_HOME=$SEEKDB_HOME" >&2
  exit 2
fi
mkdir -p "$ROOT"

stop_observer() {
  local pat="seekdb --base-dir=$1"
  local pid
  for pid in $(pgrep -f "$pat" 2>/dev/null || true); do
    kill "$pid" 2>/dev/null || true
  done
  for _ in $(seq 1 30); do
    pgrep -f "$pat" >/dev/null 2>&1 || return 0
    sleep 1
  done
  for pid in $(pgrep -f "$pat" 2>/dev/null || true); do
    kill -9 "$pid" 2>/dev/null || true
  done
}

DATA_ARGS=()
if [ -n "$DATA_FILE" ]; then
  DATA_ARGS+=(--data-file "$DATA_FILE")
fi
if [ -n "$QUERY_FILE" ]; then
  DATA_ARGS+=(--query-file "$QUERY_FILE")
else
  DATA_ARGS+=(--norm)
fi

echo "== seekdb: $SEEKDB_HOME/bin/seekdb"
echo "== data: ${DATA_FILE:-synthetic normalised n=$N dim=$DIM}, queries=$Q, threads=$THREADS"
echo "== budgets: $NITERS  (niter=100 reproduces the frozen baseline graph)"

for it in $NITERS; do
  base="$ROOT/niter$it"
  rm -rf "$base" "$base.out"
  echo "== niter=$it  start $(date +%H:%M:%S)"
  SEEKDB_CUVS_NITER="$it" "$PY" "$HERE/bench_official.py" \
    --n "$N" --dim "$DIM" --queries "$Q" --threads "$THREADS" \
    --port "$PORT" --base "$base" --out "$ROOT/niter$it.json" \
    "${DATA_ARGS[@]}" 2>&1 | tail -2
  stop_observer "$base"
  sleep 2
done

echo
"$PY" "$HERE/summarize_niter.py" "$ROOT"
