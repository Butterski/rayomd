#!/usr/bin/env python3
"""Benchmark native Markdown features against a comparable baseline.

The script generates deterministic ASCII documents and runs the project's
native warm `--bench` command. It is intentionally focused on features that
should stay cheap in the lightweight renderer: thematic breaks and explicit
page-break markers.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import subprocess
import sys
from pathlib import Path


def write_doc(path: Path, sections: int, mode: str) -> None:
    parts: list[str] = ["# Markdown Feature Benchmark", ""]
    for idx in range(1, sections + 1):
        parts.extend(
            [
                f"## Section {idx}",
                "",
                (
                    "This paragraph exercises normal wrapping, inline **bold** cleanup, "
                    "`code spans`, and [a link](https://example.com). The words are "
                    "kept stable so feature-marker overhead is easier to compare."
                ),
                "",
                "- alpha item",
                "- beta item",
                "- gamma item",
                "",
            ]
        )

        if idx % 12 == 0:
            parts.extend(
                [
                    "| Name | Value |",
                    "|---|---:|",
                    f"| item-{idx} | {idx} |",
                    "",
                ]
            )

        if mode == "rules" and idx % 8 == 0:
            parts.extend(["---", ""])
        elif mode == "pagebreaks" and idx % 18 == 0:
            parts.extend([r"\pagebreak", ""])
        elif mode == "comments" and idx % 18 == 0:
            parts.extend(["<!-- pagebreak -->", ""])

    path.write_text("\n".join(parts), encoding="utf-8", newline="\n")


def parse_bench(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def run_case(binary: Path, input_path: Path, output_dir: Path, iterations: int) -> dict[str, str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(binary),
        "--bench",
        str(input_path),
        str(output_dir),
        str(iterations),
        "modern",
        "normal",
    ]
    subprocess.run(cmd, check=True)
    return parse_bench(output_dir / "bench-results.txt")


def pct_delta(value: float, baseline: float) -> float:
    if baseline == 0:
        return 0.0
    return ((value - baseline) / baseline) * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="fast-markdown binary with --bench support")
    parser.add_argument("--output", default=Path("benchmark-output/markdown-features"), type=Path)
    parser.add_argument("--sections", default=240, type=int)
    parser.add_argument("--iterations", default=300, type=int)
    parser.add_argument("--rounds", default=3, type=int, help="Sequential bench rounds per case")
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        print(f"Binary not found: {binary}", file=sys.stderr)
        return 2

    root = args.output
    inputs = root / "inputs"
    results = root / "results"
    inputs.mkdir(parents=True, exist_ok=True)
    results.mkdir(parents=True, exist_ok=True)

    cases = [
        ("baseline", "baseline.md"),
        ("rules", "horizontal-rules.md"),
        ("pagebreaks", "pagebreaks-command.md"),
        ("comments", "pagebreaks-comment.md"),
    ]

    rows: list[dict[str, str]] = []
    for mode, filename in cases:
        input_path = inputs / filename
        write_doc(input_path, args.sections, mode)
        benches = [
            run_case(binary, input_path, results / f"{mode}-{round_idx + 1}", args.iterations)
            for round_idx in range(max(1, args.rounds))
        ]
        times = [float(bench["avg_ms"]) for bench in benches]
        best_idx = min(range(len(times)), key=times.__getitem__)
        bench = benches[best_idx]
        rows.append(
            {
                "case": mode,
                "input_bytes": bench["input_bytes"],
                "best_ms": f"{min(times):.2f}",
                "median_ms": f"{statistics.median(times):.2f}",
                "avg_pdf_bytes": bench["avg_pdf_bytes"],
                "path": bench["path"],
            }
        )

    baseline_best_ms = float(rows[0]["best_ms"])
    baseline_median_ms = float(rows[0]["median_ms"])
    baseline_pdf = float(rows[0]["avg_pdf_bytes"])
    for row in rows:
        row["delta_best_ms_pct"] = f"{pct_delta(float(row['best_ms']), baseline_best_ms):.2f}"
        row["delta_median_ms_pct"] = f"{pct_delta(float(row['median_ms']), baseline_median_ms):.2f}"
        row["delta_pdf_pct"] = f"{pct_delta(float(row['avg_pdf_bytes']), baseline_pdf):.2f}"

    csv_path = root / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    md_path = root / "summary.md"
    lines = [
        "# Markdown Feature Benchmark",
        "",
        f"- binary: `{binary}`",
        f"- sections: {args.sections}",
        f"- iterations: {args.iterations}",
        f"- rounds: {max(1, args.rounds)}",
        "",
        "| Case | Input bytes | Best ms | Delta best | Median ms | Delta median | Avg PDF bytes | Delta PDF | Path |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        lines.append(
            "| {case} | {input_bytes} | {best_ms} | {delta_best_ms_pct}% | "
            "{median_ms} | {delta_median_ms_pct}% | {avg_pdf_bytes} | "
            "{delta_pdf_pct}% | {path} |".format(**row)
        )
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    print(md_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
