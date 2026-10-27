#!/usr/bin/env bash
# Build-time benchmark for the hipVS/cuVS GPU backend.
#
# Measures the quantity the challenge asks for: "start when the index does not
# exist yet, stop when it is queryable" -- i.e. wall time of
# `create vector index ... with (lib=cuvs)` over an already-loaded table,
# compared against the same statement with `lib=vsag` on the same data.
#
# Every group is repeated (default 3x) and the median is reported. Each group also
# reports recall@k against ground truth computed from the generated vectors (and
# query latency), so a build speedup can never be claimed at the cost of quality.
#
# Usage (observer already running, built with -DOB_BUILD_CUVS=ON):
#   docs/gpu-vector-index-hipvs-cuvs/bench/run_build_bench.sh \
#     --port 2981 --rows 2000,10000,50000 --repeat 3 --out /tmp/seekdb-cuvs-bench
#
# Options: --metric l2|cosine|"l2 cosine" (default both), --dim 128,
#          --queries 100, --topk 10, --data DIR, --out DIR
set -euo pipefail

PORT=2981
ROWS="2000,10000,50000"
REPEAT=3
DIM=128
QUERIES=100
TOPK=10
METRICS="l2 cosine"
OUT=/tmp/seekdb-cuvs-bench
DATA=""
DB=hipvs_bench

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    --queries) QUERIES=$2; shift 2 ;;
    --topk) TOPK=$2; shift 2 ;;
    --metric) METRICS=$2; shift 2 ;;
    --data) DATA=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

DATA=${DATA:-$OUT/data}
mkdir -p "$OUT" "$DATA"
JSONL=$OUT/bench.jsonl
MD=$OUT/bench.md
: > "$JSONL"

mysql_cmd()
{
  timeout 900 mysql --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$PORT" -uroot -A -N -B "$@"
}

now_ms()
{
  echo $(( $(date +%s%N) / 1000000 ))
}

median()
{
  sort -n | awk '{v[NR]=$1} END {if (NR==0) {print 0; exit}
    if (NR%2) {print v[(NR+1)/2]} else {print (v[NR/2]+v[NR/2+1])/2}}'
}

echo "== generating deterministic data (dim=$DIM, queries=$QUERIES, rows=$ROWS) =="
python3 - "$DATA" "$ROWS" "$DIM" "$QUERIES" "$TOPK" "$DB" <<'PY'
import pathlib
import sys

import numpy as np

data, rows_arg, dim, nq, topk, database = (
    pathlib.Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
    int(sys.argv[5]), sys.argv[6])
rows = [int(value) for value in rows_arg.split(",")]
rng = np.random.default_rng(1314)
largest = max(rows)
base = rng.normal(size=(largest, dim)).astype(np.float32)
base /= np.linalg.norm(base, axis=1, keepdims=True)
probe_rows = np.linspace(0, largest - 1, nq, dtype=np.int64)
queries = base[probe_rows] + rng.normal(scale=0.05, size=(nq, dim)).astype(np.float32)
queries /= np.linalg.norm(queries, axis=1, keepdims=True)


def vector(values):
    return "[" + ",".join(f"{value:.7f}" for value in values) + "]"


for n in rows:
    # 250 rows per statement keeps each insert well under max_allowed_packet.
    with (data / f"rows-{n}.sql").open("w") as sql:
        sql.write(f"use {database};\n")
        for start in range(0, n, 250):
            chunk = base[start:start + 250]
            rows_sql = [f"({start + i},'{vector(row)}')" for i, row in enumerate(chunk)]
            sql.write("insert into bench values " + ",".join(rows_sql) + ";\n")
    ground_truth = [np.argsort(np.sum((base[:n] - query) ** 2, axis=1))[:topk] for query in queries]
    with (data / f"gt-{n}.txt").open("w") as gt:
        for ids in ground_truth:
            gt.write(",".join(str(int(identifier)) for identifier in ids) + "\n")
with (data / "queries.txt").open("w") as probe_file:
    for query in queries:
        probe_file.write(vector(query) + "\n")
PY

mysql_cmd -e "create database if not exists $DB;" >/dev/null

emit()
{
  printf '{"kind":"%s","metric":"%s","rows":%s,"load_ms":%s,"build_ms":%s,"recall_at_%s":%s,"query_ms":%s}\n' \
    "$1" "$2" "$3" "$4" "$5" "$TOPK" "$6" "$7" >> "$JSONL"
}

bench_group()
{
  local lib=$1 metric=$2 rows=$3
  local index_opts distance_fn
  if [[ "$metric" == "cosine" ]]; then
    index_opts="distance=cosine,type=hnsw,lib=$lib"
    distance_fn=cosine_distance
  else
    index_opts="distance=l2,type=hnsw,lib=$lib"
    distance_fn=l2_distance
  fi

  mysql_cmd -e "use $DB; drop table if exists bench;" >/dev/null

  local load_start load_end
  load_start=$(now_ms)
  mysql_cmd -e "use $DB; create table bench(id int primary key, v vector($DIM));" >/dev/null
  mysql_cmd < "$DATA/rows-$rows.sql" >/dev/null
  load_end=$(now_ms)

  # Index build: timed from "no index exists" to "the statement returned, index
  # queryable". Repeated; the median is reported.
  local -a builds=()
  for _ in $(seq 1 "$REPEAT"); do
    mysql_cmd -e "use $DB; drop index idx on bench;" >/dev/null 2>&1 || true
    local t0 t1
    t0=$(now_ms)
    mysql_cmd -e "use $DB; create vector index idx on bench(v) with ($index_opts,m=16,ef_construction=200,ef_search=200);" >/dev/null
    mysql_cmd -e "use $DB; select count(*) from (select id from bench order by $distance_fn(v,'$(head -n 1 "$DATA/queries.txt")') APPROXIMATE limit $TOPK) x;" >/dev/null
    t1=$(now_ms)
    builds+=($((t1 - t0)))
  done
  local build_ms
  build_ms=$(printf '%s\n' "${builds[@]}" | median)

  # One client round trip with all queries: honest per-query latency, plus the
  # ANN ids needed for recall.
  local query_sql=$DATA/ann-$lib-$metric-$rows.sql
  python3 - "$DATA/queries.txt" "$distance_fn" "$TOPK" "$query_sql" "$DB" <<'PY'
import sys

queries, distance_fn, topk, out, database = sys.argv[1:6]
lines = [f"use {database};"]
for query in open(queries).read().split("\n"):
    if not query.strip():
        continue
    lines.append(f"select group_concat(id order by id) from (select id from bench "
                 f"order by {distance_fn}(v,'{query}') APPROXIMATE limit {topk}) x;")
open(out, "w").write("\n".join(lines) + "\n")
PY
  local ann_start ann_end
  ann_start=$(now_ms)
  mysql_cmd < "$query_sql" > "$DATA/ann-$lib-$metric-$rows.out"
  ann_end=$(now_ms)
  local query_ms
  query_ms=$(python3 -c "print(round(($ann_end - $ann_start) / max(1, $QUERIES), 2))")

  local recall
  recall=$(python3 - "$DATA/ann-$lib-$metric-$rows.out" "$DATA/gt-$rows.txt" <<'PY'
import sys

ann_path, gt_path = sys.argv[1], sys.argv[2]
ann = [line for line in open(ann_path).read().split("\n") if line.strip()]
gt = [line for line in open(gt_path).read().split("\n") if line.strip()]
hits = total = 0
for got, want in zip(ann, gt):
    got_ids = {value for value in got.split(",") if value}
    want_ids = {value for value in want.split(",") if value}
    hits += len(got_ids & want_ids)
    total += len(want_ids)
print(f"{hits / total:.4f}" if total else "nan")
PY
)

  emit "$lib" "$metric" "$rows" "$((load_end - load_start))" "$build_ms" "$recall" "$query_ms"
  printf '%-4s %-6s rows=%-7s load=%-7sms build_median=%-8sms recall@%s=%-7s query=%sms\n' \
    "$lib" "$metric" "$rows" "$((load_end - load_start))" "$build_ms" "$TOPK" "$recall" "$query_ms"
}

for rows in ${ROWS//,/ }; do
  for metric in $METRICS; do
    bench_group cuvs "$metric" "$rows"
    bench_group vsag "$metric" "$rows"
  done
done

python3 - "$JSONL" "$MD" <<'PY'
import json
import sys
from collections import defaultdict

jsonl, out_md = sys.argv[1], sys.argv[2]
groups = defaultdict(dict)
for line in open(jsonl):
    row = json.loads(line)
    groups[(row["metric"], row["rows"])][row["kind"]] = row

topk = "10"
lines = [f"| metric | rows | cpu build (ms) | gpu build (ms) | build speedup | cpu recall@{topk} | gpu recall@{topk} | cpu query (ms) | gpu query (ms) |",
         "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
for (metric, rows), value in sorted(groups.items()):
    cpu, gpu = value.get("vsag"), value.get("cuvs")
    if not cpu or not gpu:
        continue
    speedup = cpu["build_ms"] / gpu["build_ms"] if gpu["build_ms"] else float("nan")
    lines.append(f"| {metric} | {rows} | {cpu['build_ms']} | {gpu['build_ms']} | {speedup:.2f}x | "
                 f"{cpu.get(f'recall_at_{topk}')} | {gpu.get(f'recall_at_{topk}')} | "
                 f"{cpu['query_ms']} | {gpu['query_ms']} |")
open(out_md, "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "wrote $JSONL"
echo "wrote $MD"
