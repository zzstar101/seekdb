#!/usr/bin/env python3
"""Summarise the per-run score JSON files of the fts_large_bench workflow.

Every repeat of the benchmark writes ``score-<i>.json`` (the output of
``tools/benchmark/fts_large_bench_score.py --json``). Each of those files stores
the measured value of every metric under ``categories.<name>.metrics[].current``.

Two runs of the workflow -- one on a pristine ref, one on the candidate -- are
what actually answers "did this patch help": the ``improvement`` numbers inside
the score files are computed against the upstream VLDB-CI baseline, which was
measured on a different (slower) machine, so they are not comparable here.
Printing mean/min/max per metric keeps the noise visible instead of hiding it
behind a single number.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path


def collect(score_dir: Path) -> tuple[dict[str, list[float]], dict[str, list[float]]]:
    metrics: dict[str, list[float]] = {}
    categories: dict[str, list[float]] = {}
    for path in sorted(score_dir.glob("score-*.json")):
        try:
            # utf-8-sig: a score file may have been copied around by a tool that
            # prepends a BOM, and a BOM must not hide a whole sample.
            data = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as exc:
            print(f"> {path.name} unreadable: {exc}")
            continue
        for category, payload in (data.get("categories") or {}).items():
            if not isinstance(payload, dict):
                continue
            improvement = payload.get("improvement")
            if improvement is not None:
                categories.setdefault(category, []).append(float(improvement))
            for metric in payload.get("metrics") or []:
                current = metric.get("current")
                if current is not None:
                    metrics.setdefault(metric["metric"], []).append(float(current))
    return metrics, categories


def render(metrics: dict[str, list[float]], categories: dict[str, list[float]]) -> str:
    if not metrics:
        return "\n_no scoreable benchmark output_\n"

    lines = [
        "",
        "### Measured values (mean over repeats)",
        "",
        "Compare the `mean` column with the same table of the pristine run;",
        "`improvement` versus the upstream CI baseline is a reference only.",
        "",
        "| metric | n | mean | min | max |",
        "| --- | --- | --- | --- | --- |",
    ]
    for name in sorted(metrics):
        values = metrics[name]
        lines.append(
            f"| `{name}` | {len(values)} | {statistics.mean(values):.6g} "
            f"| {min(values):.6g} | {max(values):.6g} |"
        )

    if categories:
        lines += [
            "",
            "### Category improvements versus the upstream CI baseline",
            "",
            "| category | mean improvement |",
            "| --- | --- |",
        ]
        for name in sorted(categories):
            lines.append(f"| `{name}` | {statistics.mean(categories[name]) * 100:.2f}% |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("score_dir", type=Path, help="Directory holding score-*.json")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args()

    metrics, categories = collect(args.score_dir)
    report = render(metrics, categories)
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
