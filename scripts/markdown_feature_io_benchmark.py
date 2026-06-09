#!/usr/bin/env python3
"""Benchmark native batch and serve modes for cheap Markdown features."""

from __future__ import annotations

import argparse
import csv
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

from markdown_feature_benchmark import write_doc


def copy_case_docs(source_doc: Path, target_dir: Path, files: int) -> list[Path]:
    target_dir.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    text = source_doc.read_text(encoding="utf-8")
    for idx in range(files):
        path = target_dir / f"{idx + 1:03d}.md"
        path.write_text(text, encoding="utf-8", newline="\n")
        paths.append(path)
    return paths


def parse_serve_times(output: str) -> list[float]:
    times: list[float] = []
    for line in output.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] == "OK":
            try:
                times.append(float(parts[1]))
            except ValueError:
                pass
    return times


def run_batch(binary: Path, input_dir: Path, output_dir: Path) -> float:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    subprocess.run(
        [
            str(binary),
            "--batch",
            str(input_dir),
            str(output_dir),
            "native",
            "modern",
            "normal",
        ],
        check=True,
    )
    return (time.perf_counter() - start) * 1000.0


def run_serve(binary: Path, input_paths: list[Path], output_dir: Path) -> tuple[float, float]:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    payload = "\n".join(str(path) for path in input_paths) + "\nquit\n"
    start = time.perf_counter()
    proc = subprocess.run(
        [
            str(binary),
            "--serve",
            str(output_dir),
            "native",
            "modern",
            "normal",
        ],
        input=payload,
        text=True,
        capture_output=True,
        check=True,
    )
    total_ms = (time.perf_counter() - start) * 1000.0
    per_file_times = parse_serve_times(proc.stdout)
    if len(per_file_times) != len(input_paths):
        raise RuntimeError(
            f"Expected {len(input_paths)} serve timings, got {len(per_file_times)}. "
            f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
        )
    return total_ms, statistics.median(per_file_times)


def pct_delta(value: float, baseline: float) -> float:
    if baseline == 0:
        return 0.0
    return ((value - baseline) / baseline) * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", default=Path("benchmark-output/markdown-feature-io"), type=Path)
    parser.add_argument("--sections", default=24, type=int)
    parser.add_argument("--files", default=10, type=int)
    parser.add_argument("--rounds", default=5, type=int)
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        print(f"Binary not found: {binary}", file=sys.stderr)
        return 2

    root = args.output
    inputs_root = root / "inputs"
    results_root = root / "results"
    inputs_root.mkdir(parents=True, exist_ok=True)
    results_root.mkdir(parents=True, exist_ok=True)

    cases = [
        ("baseline", "baseline"),
        ("rules", "rules"),
        ("pagebreaks", "pagebreaks"),
        ("comments", "comments"),
    ]

    rows: list[dict[str, str]] = []
    for case_name, mode in cases:
        source_doc = inputs_root / f"{case_name}.md"
        write_doc(source_doc, args.sections, mode)
        input_paths = copy_case_docs(source_doc, inputs_root / case_name, args.files)

        batch_rounds = [
            run_batch(binary, inputs_root / case_name, results_root / f"{case_name}-batch-{idx + 1}")
            for idx in range(max(1, args.rounds))
        ]
        serve_rounds = [
            run_serve(binary, input_paths, results_root / f"{case_name}-serve-{idx + 1}")
            for idx in range(max(1, args.rounds))
        ]
        serve_totals = [item[0] for item in serve_rounds]
        serve_file_medians = [item[1] for item in serve_rounds]

        rows.append(
            {
                "case": case_name,
                "input_bytes_each": str(source_doc.stat().st_size),
                "files": str(args.files),
                "batch_total_median_ms": f"{statistics.median(batch_rounds):.2f}",
                "batch_per_file_median_ms": f"{statistics.median(batch_rounds) / args.files:.2f}",
                "serve_total_median_ms": f"{statistics.median(serve_totals):.2f}",
                "serve_reported_file_median_ms": f"{statistics.median(serve_file_medians):.2f}",
            }
        )

    baseline = rows[0]
    baseline_batch = float(baseline["batch_per_file_median_ms"])
    baseline_serve = float(baseline["serve_reported_file_median_ms"])
    for row in rows:
        row["batch_per_file_delta_pct"] = f"{pct_delta(float(row['batch_per_file_median_ms']), baseline_batch):.2f}"
        row["serve_file_delta_pct"] = f"{pct_delta(float(row['serve_reported_file_median_ms']), baseline_serve):.2f}"

    csv_path = root / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    lines = [
        "# Markdown Feature IO Benchmark",
        "",
        f"- binary: `{binary}`",
        f"- files per case: {args.files}",
        f"- sections per file: {args.sections}",
        f"- rounds: {max(1, args.rounds)}",
        "",
        "| Case | Bytes/file | Batch total median ms | Batch/file median ms | Batch delta | Serve total median ms | Serve reported file median ms | Serve delta |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            "| {case} | {input_bytes_each} | {batch_total_median_ms} | "
            "{batch_per_file_median_ms} | {batch_per_file_delta_pct}% | "
            "{serve_total_median_ms} | {serve_reported_file_median_ms} | "
            "{serve_file_delta_pct}% |".format(**row)
        )
    md_path = root / "summary.md"
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(md_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
