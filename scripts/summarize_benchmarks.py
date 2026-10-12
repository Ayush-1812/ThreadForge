#!/usr/bin/env python3
"""Print median batch times and sequential speedups from Google Benchmark JSON."""

import argparse
import json
import re
import sys


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
        pool, workload, workers, tasks, rounds = match.groups()
        key = workload, int(tasks), int(rounds), pool, int(workers or 0)
        quality = rank.get(entry.get("aggregate_name"), 0)
        if key not in chosen or quality > chosen[key][0]:
            chosen[key] = quality, entry["real_time"] * units[entry["time_unit"]]
    if not chosen:
        raise ValueError("No recognized ThreadForge benchmark cases in report")
    print("Batch wall times use the median when repeated measurements are available.")
    print("Speedup = sequential batch time / measured batch time; >1 means faster.")
    print("\n| Workload | Tasks | Rounds | Scheduler | Workers | Batch (us) | Speedup |")
    print("| --- | ---: | ---: | --- | ---: | ---: | ---: |")
    pool_order = {"Sequential": 0, "Centralized": 1, "ThreadForge": 2}
    for key in sorted(chosen, key=lambda k: (*k[:3], pool_order[k[3]], k[4])):
        workload, tasks, rounds, pool, workers = key
        duration = chosen[key][1]
        sequential = chosen.get((workload, tasks, rounds, "Sequential", 0))
        speedup = f"{sequential[1] / duration:.3f}" if sequential and duration > 0 else "—"
        print(f"| {workload} | {tasks} | {rounds} | {pool} | {workers or '—'} "
              f"| {duration:.3f} | {speedup} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="Google Benchmark JSON output file")
    args = parser.parse_args()
    try:
        with open(args.report, encoding="utf-8") as source:
            summarize(json.load(source))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Cannot summarize benchmarks: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
