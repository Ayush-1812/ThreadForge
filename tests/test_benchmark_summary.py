#!/usr/bin/env python3
"""Regression tests for benchmark validation and portable report generation."""

import importlib.util
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "summarize_benchmarks.py"
SPEC = importlib.util.spec_from_file_location("threadforge_benchmark_summary", SCRIPT)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def measurement(pool="Sequential", workers=None, duration=2.0, unit="us", aggregate=None):
    name = f"BM_{pool}Uniform/"
    if workers is not None:
        name += f"workers:{workers}/"
    name += "tasks:1024/rounds:256/manual_time"
    record = {"name": name, "real_time": duration, "time_unit": unit}
    if aggregate is not None:
        record["name"] += f"_{aggregate}"
        record["aggregate_name"] = aggregate
    return record


class BenchmarkSummaryTests(unittest.TestCase):
    def test_median_preferred_independent_of_entry_order_and_units_converted(self):
        sequential = measurement(duration=2000, unit="ns", aggregate="median")
        centralized = measurement("Centralized", 2, duration=0.001, unit="ms")
        threadforge = [
            measurement("ThreadForge", 4, duration=999),
            measurement("ThreadForge", 4, duration=17, aggregate="mean"),
            measurement("ThreadForge", 4, duration=0.004, unit="ms", aggregate="median"),
        ]
        for records in itertools.permutations(threadforge):
            with self.subTest(order=[record.get("aggregate_name") for record in records]):
                output = SUMMARY.summarize({"benchmarks": [sequential, *records, centralized]})
                self.assertIn("| Sequential | \u2014 | 2.000 | 1 |", output)
                self.assertIn("| Centralized | 2 | 1.000 | 2 |", output)
                self.assertIn("| ThreadForge | 4 | 4.000 | 0.5 |", output)

    def test_mean_used_when_median_absent_and_spread_aggregates_ignored(self):
        records = [
            measurement("ThreadForge", 1, duration=91),
            measurement("ThreadForge", 1, duration=8, aggregate="mean"),
            measurement("ThreadForge", 1, duration=float("nan"), aggregate="cv"),
            measurement("ThreadForge", 1, duration=0, aggregate="stddev"),
        ]
        output = SUMMARY.summarize({"benchmarks": records})
        self.assertIn("| ThreadForge | 1 | 8.000 | \u2014 |", output)

    def test_filtered_case_without_sequential_baseline_has_no_invented_speedup(self):
        output = SUMMARY.summarize({"benchmarks": [measurement("ThreadForge", 8, duration=3)]})
        self.assertIn("| ThreadForge | 8 | 3.000 | \u2014 |", output)
        self.assertNotIn("| Sequential |", output)

    def test_sequential_baseline_must_match_workload_task_count_and_rounds(self):
        mismatches = [measurement(), measurement(), measurement()]
        mismatches[0]["name"] = mismatches[0]["name"].replace("Uniform", "UnevenFanout")
        mismatches[1]["name"] = mismatches[1]["name"].replace("tasks:1024", "tasks:2048")
        mismatches[2]["name"] = mismatches[2]["name"].replace("rounds:256", "rounds:1")
        output = SUMMARY.summarize({"benchmarks": [*mismatches, measurement("ThreadForge", 4)]})
        self.assertIn("| ThreadForge | 4 | 2.000 | \u2014 |", output)

    def test_tiny_positive_speedup_keeps_significant_digits(self):
        output = SUMMARY.summarize({"benchmarks": [
            measurement(duration=0.004), measurement("ThreadForge", 1, duration=100),
        ]})
        self.assertIn("| ThreadForge | 1 | 100.000 | 4e-05 |", output)

    def test_invalid_wall_times_are_rejected(self):
        for duration in (float("nan"), float("inf"), -float("inf"), 0, -1, True, None, "2"):
            with self.subTest(duration=duration):
                with self.assertRaisesRegex(ValueError, "positive and finite"):
                    SUMMARY.summarize({"benchmarks": [measurement(duration=duration)]})

    def test_invalid_cpu_times_are_rejected_but_zero_is_valid(self):
        for duration in (float("nan"), float("inf"), -float("inf"), -1, False, None, "2"):
            with self.subTest(duration=duration):
                record = measurement()
                record["cpu_time"] = duration
                with self.assertRaisesRegex(ValueError, "nonnegative and finite"):
                    SUMMARY.summarize({"benchmarks": [record]})
        record = measurement()
        record["cpu_time"] = 0
        self.assertIn("| Sequential |", SUMMARY.summarize({"benchmarks": [record]}))

    def test_unit_conversion_overflow_and_unknown_units_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "cannot be represented"):
            SUMMARY.summarize({"benchmarks": [measurement(duration=1e308, unit="s")]})
        with self.assertRaises(KeyError):
            SUMMARY.summarize({"benchmarks": [measurement(unit="minutes")]})

    def test_error_entries_reject_report_even_when_name_is_unrelated(self):
        records = [measurement(), {"name": "OtherBenchmark", "error_occurred": True,
                                   "error_message": "checksum mismatch"}]
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            SUMMARY.summarize({"benchmarks": records})

    def test_reports_without_recognized_cases_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "No recognized"):
            SUMMARY.summarize({"benchmarks": []})

    def run_cli(self, raw, normalized):
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(raw), "--normalized-json", str(normalized)],
            text=True, capture_output=True, check=False,
        )

    def test_normalization_preserves_metadata_finite_values_and_raw_report(self):
        with tempfile.TemporaryDirectory(prefix="threadforge-summary-") as directory:
            raw = Path(directory) / "results.raw.json"
            normalized = Path(directory) / "results.json"
            record = measurement()
            record.update({"cpu_time": 0, "checksum_hi": 4294967295,
                           "checksum_lo": 17, "worker0_completed_share": 0.25})
            report = {"context": {"compiler": "fixture compiler", "timing": "manual"},
                      "benchmarks": [record], "undefined_counters": [
                          float("nan"), float("inf"), -float("inf"), 42.5,
                      ]}
            raw_contents = json.dumps(report) + "\n"
            raw.write_text(raw_contents, encoding="utf-8")
            completed = self.run_cli(raw, normalized)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(raw.read_text(encoding="utf-8"), raw_contents)
            portable = json.loads(normalized.read_text(encoding="utf-8"),
                                  parse_constant=lambda value: self.fail(f"Nonportable constant: {value}"))
            self.assertEqual(portable["context"], report["context"])
            self.assertEqual(portable["benchmarks"], [record])
            self.assertEqual(portable["undefined_counters"], [None, None, None, 42.5])
            self.assertIn("| Sequential |", completed.stdout)

    def test_normalization_refuses_to_overwrite_raw_report(self):
        with tempfile.TemporaryDirectory(prefix="threadforge-summary-") as directory:
            raw = Path(directory) / "results.raw.json"
            raw_contents = json.dumps({"benchmarks": [measurement()]})
            raw.write_text(raw_contents, encoding="utf-8")
            completed = self.run_cli(raw, raw)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("different path", completed.stderr)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(raw.read_text(encoding="utf-8"), raw_contents)

    def test_invalid_timing_is_not_repaired_or_written_by_normalization(self):
        with tempfile.TemporaryDirectory(prefix="threadforge-summary-") as directory:
            raw = Path(directory) / "results.raw.json"
            normalized = Path(directory) / "results.json"
            raw_contents = json.dumps({"benchmarks": [measurement(duration=float("nan"))]})
            raw.write_text(raw_contents, encoding="utf-8")
            completed = self.run_cli(raw, normalized)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("positive and finite", completed.stderr)
            self.assertEqual(completed.stdout, "")
            self.assertFalse(normalized.exists())
            self.assertEqual(raw.read_text(encoding="utf-8"), raw_contents)


if __name__ == "__main__":
    unittest.main()
