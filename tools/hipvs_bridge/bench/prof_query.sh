#!/usr/bin/env bash
# Start a server on an already built base directory and decompose one query.
#
#   SEEKDB_HOME=/root/work/cand_home BASE=/root/work/run/eval ./prof_query.sh
#
# BASE must be a base directory whose table t(vid, vec) already carries the
# contest index (bench_official.py leaves one behind).  The script never builds
# anything: it only measures, so it is safe to run repeatedly.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PY:-/opt/venv/bin/python}"
SEEKDB_HOME="${SEEKDB_HOME:-/seekdb_workspace}"
BASE="${BASE:-/root/work/run/eval}"
PORT="${PORT:-2881}"
N="${N:-250000}"
DIM="${DIM:-128}"
QUERIES="${QUERIES:-500}"
export SEEKDB_HOME

if [ ! -d "$BASE" ]; then
  echo "no base directory at $BASE" >&2
  exit 2
fi

"$SEEKDB_HOME/bin/seekdb" --base-dir="$BASE" --port="$PORT" --nodaemon \
  --parameter _memory_budget=16G --parameter datafile_size=20G \
  --parameter datafile_maxsize=40G --parameter log_disk_size=10G \
  --parameter _max_syslog_file_count=10 --log-level=INFO \
  >"$BASE.prof.out" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

for _ in $(seq 1 180); do
  if "$PY" - "$PORT" <<'PY' 2>/dev/null
import sys, pymysql
pymysql.connect(host="127.0.0.1", port=int(sys.argv[1]), user="root", password="").cursor().execute("select 1")
PY
  then break; fi
  sleep 2
done

"$PY" "$HERE/prof_query.py" --port "$PORT" --n "$N" --dim "$DIM" --queries "$QUERIES" \
  --out "$BASE.prof.json"
