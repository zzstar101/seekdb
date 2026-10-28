#!/usr/bin/env python3
"""Summarise the interleaved A/B benchmark runs produced by fts-ab.yml.

The two versions are benchmarked on the same runner, alternating inside one
loop, so the only trustworthy number is the paired difference: for every pair
i we compare B against A taken minutes earlier on the same machine. Absolute
means are printed as well, but they include any drift the pair design is there
to cancel.

Usage: ab_summary.py <results-dir>
"""

from __future__ import annotations

import re
import statistics
import sys
from pathlib import Path

METRIC_RE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*:\s*(-?\d+(?:\.\d+)?)\s*$")

CATEGORIES = {
    "build": ["build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec"],
    "tokenize": ["tokenize_ik_avg_ms", "tokenize_beng_avg_ms"],
    "query": [
        "query_cn_avg_ms",
        "query_beng_avg_ms",
        "query_mixed_avg_ms",
        "query_limit_avg_ms",
    ],
}

ORDER = [m for metrics in CATEGORIES.values() for m in metrics]


def read_metrics(path: Path) -> dict[str, float]:
    values: dict[str, float] = {}
    # utf-8-sig: files written on Windows in earlier rounds carried a BOM.
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = METRIC_RE.match(line)
        if match:
            values[match.group(1)] = float(match.group(2))
    return values


RUN_RE = re.compile(r"^run-([AB])-(\d+)\.txt$")


def collect(results: Path) -> tuple[dict[int, dict[str, float]], dict[int, dict[str, float]]]:
    # The workflow tees each round to run-<v>-<i>.stdout.txt as well; only the
    # plain run-<v>-<i>.txt is the report that carries the metrics.
    a: dict[int, dict[str, float]] = {}
    b: dict[int, dict[str, float]] = {}
    for path in sorted(results.iterdir()):
        match = RUN_RE.match(path.name)
        if not match:
            continue
        version, index = match.group(1), int(match.group(2))
        (a if version == "A" else b)[index] = read_metrics(path)
    return a, b


def fmt(value: float, digits: int = 3) -> str:
    return f"{value:.{digits}f}"


def main(argv: list[str]) -> int:
    results = Path(argv[1] if len(argv) > 1 else ".")
    a, b = collect(results)
    pairs = sorted(set(a) & set(b))
    if not pairs:
        print("no complete A/B pairs found in", results)
        return 1

    print(f"# Interleaved A/B — {len(pairs)} complete pair(s)")
    print()
    print("Versions alternate inside one loop (A then B per pair) on one runner,")
    print("so `paired delta` cancels machine drift; negative = B is faster.")
    print()
    print("| metric | A mean | A min–max | B mean | B min–max | paired delta | B faster in |")
    print("| --- | --- | --- | --- | --- | --- | --- |")

    category_deltas: dict[str, list[float]] = {name: [] for name in CATEGORIES}
    for metric in ORDER:
        a_values = [a[i][metric] for i in pairs if metric in a[i]]
        b_values = [b[i][metric] for i in pairs if metric in b[i]]
        deltas = [
            (b[i][metric] - a[i][metric]) / a[i][metric] * 100.0
            for i in pairs
            if metric in a[i] and metric in b[i] and a[i][metric]
        ]
        if not deltas:
            continue
        for name, metrics in CATEGORIES.items():
            if metric in metrics:
                category_deltas[name].extend(deltas)
        print(
            "| {metric} | {a_mean} | {a_min}–{a_max} | {b_mean} | {b_min}–{b_max} | "
            "{delta} | {faster}/{total} |".format(
                metric=metric,
                a_mean=fmt(statistics.fmean(a_values)),
                a_min=fmt(min(a_values)),
                a_max=fmt(max(a_values)),
                b_mean=fmt(statistics.fmean(b_values)),
                b_min=fmt(min(b_values)),
                b_max=fmt(max(b_values)),
                delta=f"{statistics.fmean(deltas):+.1f}%",
                faster=sum(1 for d in deltas if d < 0),
                total=len(deltas),
            )
        )

    print()
    print("## Category view")
    print()
    print("| category | paired delta (mean of metrics) | improves in every pair |")
    print("| --- | --- | --- |")
    for name, deltas in category_deltas.items():
        if not deltas:
            continue
        mean = statistics.fmean(deltas)
        faster = sum(1 for d in deltas if d < 0)
        print(f"| {name} | {mean:+.1f}% | {faster}/{len(deltas)} metric-pairs |")

    # Platform score moves 1 point per 0.01 of mean improvement (see the
    # scoring module: raw = clamp(mean, 0, 0.5) / 0.5 * 100, platform = raw / 2).
    deltas_all = [d for values in category_deltas.values() for d in values]
    if deltas_all:
        # A 1% relative speed-up on one metric is worth (1/3) * 0.01 of the
        # category mean only when that metric's baseline improvement is ~0;
        # report the raw relative deltas instead of inventing a score.
        print()
        print(f"Overall paired delta across all metrics: {statistics.fmean(deltas_all):+.1f}%")
        print()
        print("`build` is the noisy category: within-run spread reached 40-60% in")
        print("earlier rounds, so read its paired column together with the")
        print("`B faster in` count, not the mean alone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
