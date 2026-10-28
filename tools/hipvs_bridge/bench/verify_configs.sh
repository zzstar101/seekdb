#!/usr/bin/env bash
# Run several bridge configurations against the same data and compare them.
#
#   SEEKDB_HOME=/root/work/cand_home ./verify_configs.sh              # 250k x 128
#   SEEKDB_HOME=/root/work/cand_home N=250000 DIM=256 ./verify_configs.sh
#
# CONFIGS is a space separated list of "label:ENV=VALUE[,ENV=VALUE]".  Every run
# rebuilds the index from scratch in a fresh base directory with the same
# vectors, the contest DDL settings (m=16, ef_construction=200) and the contest
# query session setting (ob_hnsw_ef_search=64).  Only the labelled knobs change.
#
# The default sweep answers the two open questions in one go:
#   base   frozen-baseline graph reproduced by this binary (100 NN-descent rounds)
#   n50    the recall-safe budget measured earlier (1.79x)
#   n30    the aggressive budget that is shipped by default (2.10x, recall -0.007)
#   ivfpq  IVF-PQ initial graph + 50 refinement rounds: if the refinement repairs
#          the quality of the cheap initial graph, this is 2.0x with no recall bet
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PY:-/opt/venv/bin/python}"
N="${N:-250000}"
DIM="${DIM:-128}"
Q="${Q:-4096}"
THREADS="${THREADS:-8}"
PORT="${PORT:-2881}"
ROOT="${ROOT:-/root/work/ab-${N}x${DIM}}"
SEEKDB_HOME="${SEEKDB_HOME:-/seekdb_workspace}"
DATA_FILE="${DATA_FILE:-}"
QUERY_FILE="${QUERY_FILE:-}"
CONFIGS="${CONFIGS:-base:SEEKDB_CUVS_NITER=100 n50:SEEKDB_CUVS_NITER=50 n30:SEEKDB_CUVS_NITER=30 ivfpq:SEEKDB_CUVS_NITER=50,SEEKDB_CUVS_BUILD_ALGO=0}"
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
fi
if [ ${#DATA_ARGS[@]} -eq 0 ]; then
  DATA_ARGS+=(--norm)
fi

echo "== seekdb: $SEEKDB_HOME/bin/seekdb"
echo "== data: ${DATA_FILE:-synthetic normalised n=$N dim=$DIM}, queries=$Q, threads=$THREADS"

for cfg in $CONFIGS; do
  label="${cfg%%:*}"
  envs="${cfg#*:}"
  base="$ROOT/$label"
  rm -rf "$base" "$base.out"
  echo "== $label  [$envs]  start $(date +%H:%M:%S)"
  env $envs "$PY" "$HERE/bench_official.py" \
    --n "$N" --dim "$DIM" --queries "$Q" --threads "$THREADS" \
    --port "$PORT" --base "$base" --out "$ROOT/$label.json" \
    "${DATA_ARGS[@]}" 2>&1 | tail -2
  stop_observer "$base"
  sleep 2
done

echo
"$PY" "$HERE/summarize_runs.py" "$ROOT" --ref base
