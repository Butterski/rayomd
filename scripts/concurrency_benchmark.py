#!/usr/bin/env python3
"""Benchmark deterministic RayoMD batch scaling at explicit worker counts."""

from __future__ import annotations

import argparse
import json
import shutil
import statistics
from pathlib import Path

from perf_watch import check_pdf, percentile, run_command


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "min": min(values),
        "median": statistics.median(values),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workers", default="1,2,4,6")
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()

    workers = [int(value) for value in args.workers.split(",")]
    inputs = sorted(args.corpus.glob("*.md"))
    if not inputs:
        raise RuntimeError(f"no Markdown files found under {args.corpus}")
    args.output.mkdir(parents=True, exist_ok=True)
    payload = chr(10).join(str(path.resolve()) for path in inputs) + chr(10)
    report: dict[str, object] = {
        "binary": str(args.binary.resolve()),
        "binary_bytes": args.binary.stat().st_size,
        "corpus": str(args.corpus.resolve()),
        "files": len(inputs),
        "rounds": args.rounds,
        "workers": {},
    }

    for worker_count in workers:
        worker_report: dict[str, object] = {}
        for mode in ("folder", "stdin"):
            elapsed: list[float] = []
            rss: list[int] = []
            failures = 0
            for round_index in range(args.rounds):
                destination = args.output / f"{mode}-w{worker_count}-r{round_index + 1}"
                if destination.exists():
                    shutil.rmtree(destination)
                if mode == "folder":
                    command = [
                        str(args.binary.resolve()), "--batch", str(args.corpus.resolve()),
                        str(destination), "native", "modern", "normal",
                        f"--workers={worker_count}",
                    ]
                    stdin = None
                else:
                    command = [
                        str(args.binary.resolve()), "--stdin-batch", str(destination),
                        "native", "modern", "normal", f"--workers={worker_count}",
                    ]
                    stdin = payload
                return_code, wall_ms, stdout, stderr, peak_rss = run_command(command, stdin)
                if return_code != 0:
                    failures += 1
                    continue
                for input_path in inputs:
                    check_pdf(destination / input_path.with_suffix(".pdf").name)
                elapsed.append(wall_ms)
                rss.append(peak_rss)
            if not elapsed:
                raise RuntimeError(f"all {mode} runs failed for {worker_count} workers")
            worker_report[mode] = {
                "failures": failures,
                "total_ms": summarize(elapsed),
                "ms_per_file": summarize([value / len(inputs) for value in elapsed]),
                "peak_rss_bytes": summarize([float(value) for value in rss]),
            }
        report["workers"][str(worker_count)] = worker_report

    json_path = args.output / "concurrency.json"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + chr(10), encoding="utf-8")
    lines = [
        "# RayoMD batch concurrency",
        "",
        f"- binary: {report['binary']}",
        f"- corpus: {report['corpus']}",
        f"- files: {report['files']}",
        f"- rounds per worker/mode: {report['rounds']}",
        "",
        "| Workers | Mode | Median ms/file | p95 ms/file | Peak RSS max | Failures |",
        "|---:|---|---:|---:|---:|---:|",
    ]
    for worker_count in workers:
        for mode in ("folder", "stdin"):
            item = report["workers"][str(worker_count)][mode]
            lines.append(
                f"| {worker_count} | {mode} | {item['ms_per_file']['median']:.3f} | "
                f"{item['ms_per_file']['p95']:.3f} | {item['peak_rss_bytes']['max'] / 1048576:.2f} MiB | "
                f"{item['failures']} |"
            )
    (args.output / "concurrency.md").write_text(chr(10).join(lines) + chr(10), encoding="utf-8")
    print(json_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())