#!/usr/bin/env python3
"""Print median batch times and sequential speedups from Google Benchmark JSON."""

import argparse
import json
import math
from pathlib import Path
import re
import sys


def checked_time(entry, field, units, positive):
    value = entry[field]
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or value < 0 or (positive and value == 0)):
        requirement = "positive and finite" if positive else "nonnegative and finite"
        raise ValueError(f"{entry['name']}: {field} must be {requirement}")
    converted = value * units[entry["time_unit"]]
    if not math.isfinite(converted) or (positive and converted == 0):
        raise ValueError(f"{entry['name']}: {field} cannot be represented in microseconds")
    return converted


def summarize(report):
    pattern = re.compile(
        r"BM_(Sequential|Centralized|ThreadForge)(Uniform|UnevenFanout)/"
        r"(?:workers:(\d+)/)?tasks:(\d+)/rounds:(\d+)/manual_time(?:_\w+)?$"
    )
    chosen = {}
    rank = {"median": 2, "mean": 1}
    units = {"ns": 0.001, "us": 1, "ms": 1000, "s": 1000000}
    for entry in report["benchmarks"]:
        if entry.get("error_occurred"):
            raise ValueError(f"{entry['name']}: {entry.get('error_message', 'benchmark failed')}")
        match = pattern.fullmatch(entry["name"])
        if not match or entry.get("aggregate_name") in ("stddev", "cv"):
            continue
        duration = checked_time(entry, "real_time", units, positive=True)
        if "cpu_time" in entry:
            checked_time(entry, "cpu_time", units, positive=False)
        pool, workload, workers, tasks, rounds = match.groups()
        key = workload, int(tasks), int(rounds), pool, int(workers or 0)
        quality = rank.get(entry.get("aggregate_name"), 0)
        if key not in chosen or quality > chosen[key][0]:
            chosen[key] = quality, duration
    if not chosen:
        raise ValueError("No recognized ThreadForge benchmark cases in report")
    lines = [
        "Batch wall times use the median when repeated measurements are available.",
        "Speedup = sequential batch time / measured batch time; >1 means faster.",
        "",
        "| Workload | Tasks | Rounds | Scheduler | Workers | Batch (us) | Speedup |",
        "| --- | ---: | ---: | --- | ---: | ---: | ---: |",
    ]
    pool_order = {"Sequential": 0, "Centralized": 1, "ThreadForge": 2}
    for key in sorted(chosen, key=lambda k: (*k[:3], pool_order[k[3]], k[4])):
        workload, tasks, rounds, pool, workers = key
        duration = chosen[key][1]
        sequential = chosen.get((workload, tasks, rounds, "Sequential", 0))
        speedup = "—"
        if sequential:
            ratio = sequential[1] / duration
            if not math.isfinite(ratio) or ratio == 0:
                raise ValueError(f"{pool}/{workload}: speedup is not positive and finite")
            speedup = f"{ratio:.4g}"
        lines.append(f"| {workload} | {tasks} | {rounds} | {pool} | {workers or '—'} "
                     f"| {duration:.3f} | {speedup} |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="Google Benchmark JSON output file")
    parser.add_argument("--normalized-json", metavar="PATH",
                        help="write portable JSON with nonfinite constants replaced by null")
    args = parser.parse_args()
    try:
        with open(args.report, encoding="utf-8") as source:
            # Google Benchmark can emit NaN for undefined CVs of zero counters.
            report = json.load(source, parse_constant=lambda _: None)
        summary = summarize(report)
        if args.normalized_json:
            destination = Path(args.normalized_json)
            if destination.resolve() == Path(args.report).resolve():
                raise ValueError("normalized JSON must use a different path from the raw report")
            normalized = json.dumps(report, allow_nan=False, indent=2)
            destination.write_text(normalized + "\n", encoding="utf-8")
        print(summary)
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        print(f"Cannot summarize benchmarks: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
