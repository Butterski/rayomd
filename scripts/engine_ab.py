#!/usr/bin/env python3
"""Warm-engine A/B benchmark of two RayoMD binaries, with an A/A noise control.

Runs `--bench` for a baseline binary, an identical copy of that binary, and a candidate
binary on the same fixtures, rotating the order every round, and reports the median of
the per-round paired changes. The copy shows how far two identical binaries drift apart
on this machine; a candidate change of that size is noise. It also reports whether the
two binaries write the same PDF bytes.

With --instructions (Linux, valgrind) it counts the instructions of one warm build per
fixture: callgrind runs N and 2N iterations and takes the difference, so process start
and font loading drop out. Instruction counts do not move with code placement, which
release-build timings do by about a percent.

    python3 tools/benchmark.py ab -- --baseline build/before/rayomd --candidate build/linux/rayomd --cpu 3
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]


def load_perf_watch() -> Any:
    spec = importlib.util.spec_from_file_location("perf_watch", ROOT / "scripts" / "perf_watch.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def default_fixtures(work: Path, seed: int) -> list[Path]:
    """The watch suite's single documents, sized documents and plain feature document."""
    perf_watch = load_perf_watch()
    corpus = perf_watch.generate_corpus(work / "watch", "watch", seed, "off")
    fixtures = list(corpus["single_paths"]) + list(corpus["sized_paths"].values())
    fixtures.append(corpus["feature_paths"]["baseline"])
    tester = ROOT / "tester.md"
    if tester.exists():
        fixtures.append(tester)
    return fixtures


def expand_fixtures(values: list[str]) -> list[Path]:
    fixtures: list[Path] = []
    for value in values:
        path = Path(value)
        if path.is_dir():
            fixtures.extend(sorted(p for p in path.iterdir() if p.suffix.lower() == ".md"))
        elif path.is_file():
            fixtures.append(path)
        else:
            raise SystemExit(f"fixture not found: {value}")
    return fixtures


def bench(binary: Path, fixture: Path, iterations: int, style: str, margin: str, work: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(dir=work) as out:
        proc = subprocess.run(
            [str(binary), "--bench", str(fixture), out, str(iterations), style, margin],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if proc.returncode != 0:
            raise SystemExit(f"{binary} --bench {fixture} failed ({proc.returncode}): {proc.stderr.strip()}")
        values = {}
        for line in (Path(out) / "bench-results.txt").read_text(encoding="utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
        sample = Path(out) / "sample.pdf"
        digest = hashlib.sha256(sample.read_bytes()).hexdigest() if sample.exists() else ""
    # total_ms over all iterations is far more precise than the rounded avg_ms.
    return {
        "us": float(values["total_ms"]) * 1000.0 / int(values["iterations"]),
        "path": values.get("path", ""),
        "sha256": digest,
    }


def instructions_per_build(binary: Path, fixture: Path, iterations: int, style: str, margin: str, work: Path) -> float:
    counts = []
    for count in (iterations, iterations * 2):
        with tempfile.TemporaryDirectory(dir=work) as out:
            proc = subprocess.run(
                ["valgrind", "--tool=callgrind", "--callgrind-out-file=" + os.devnull,
                 str(binary), "--bench", str(fixture), out, str(count), style, margin],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
        match = re.search(r"Collected\s*:\s*(\d+)", proc.stderr)
        if proc.returncode != 0 or not match:
            raise SystemExit(f"callgrind failed for {binary} on {fixture}: {proc.stderr[-400:]}")
        counts.append(int(match.group(1)))
    return (counts[1] - counts[0]) / iterations


def quartiles(values: list[float]) -> tuple[float, float]:
    if len(values) < 4:
        return min(values), max(values)
    q = statistics.quantiles(values, n=4)
    return q[0], q[2]


def geomean_pct(changes_pct: list[float]) -> float:
    return (math.exp(sum(math.log1p(c / 100.0) for c in changes_pct) / len(changes_pct)) - 1.0) * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--fixture", action="append", default=[],
                        help="Markdown file or directory of .md files; repeatable. Default: watch-suite documents and tester.md")
    parser.add_argument("--seed", type=int, default=1337, help="seed of the generated watch-suite fixtures")
    parser.add_argument("--rounds", type=int, default=11)
    parser.add_argument("--target-ms", type=float, default=300.0, help="approximate length of one --bench run")
    parser.add_argument("--cpu", type=int, help="pin the benchmark processes to this CPU (Linux)")
    parser.add_argument("--style", default="modern")
    parser.add_argument("--margin", default="normal")
    parser.add_argument("--instructions", action="store_true", help="also count instructions per build with callgrind")
    parser.add_argument("--instruction-iterations", type=int, default=6)
    parser.add_argument("--json", type=Path, help="write the results to this file")
    parser.add_argument("--fail-above", type=float,
                        help="exit 3 when the candidate's geometric-mean change exceeds this percentage")
    args = parser.parse_args()

    for binary in (args.baseline, args.candidate):
        if not binary.is_file():
            raise SystemExit(f"binary not found: {binary}")
    if args.cpu is not None:
        if hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(0, {args.cpu})
        else:
            print("warning: --cpu is ignored on this platform", file=sys.stderr)
    if args.instructions and shutil.which("valgrind") is None:
        raise SystemExit("--instructions needs valgrind on PATH")

    with tempfile.TemporaryDirectory(prefix="rayomd-engine-ab-") as work_name:
        work = Path(work_name)
        fixtures = expand_fixtures(args.fixture) if args.fixture else default_fixtures(work, args.seed)
        control_dir = work / "control"
        control_dir.mkdir()
        control = control_dir / args.baseline.name
        shutil.copy2(args.baseline, control)
        binaries = {"baseline": args.baseline, "control": control, "candidate": args.candidate}
        names = list(binaries)

        print(f"{platform.system()} {platform.machine()}, {args.rounds} rounds of ~{args.target_ms:g} ms, "
              f"cpu {args.cpu if args.cpu is not None else 'unpinned'}")
        header = f"{'fixture':28s} {'path':8s} {'baseline µs':>12s} {'A/A':>8s} {'candidate':>10s} {'IQR':>17s} {'bytes':>6s}"
        if args.instructions:
            header += f" {'instr':>8s}"
        print(header)

        results = []
        for fixture in fixtures:
            probe = bench(args.baseline, fixture, 5, args.style, args.margin, work)
            iterations = max(3, min(100000, int(math.ceil(args.target_ms * 1000.0 / max(probe["us"], 1.0)))))
            times = {name: [] for name in names}
            control_changes, candidate_changes = [], []
            digests = {}
            for round_index in range(args.rounds):
                order = names[round_index % 3:] + names[:round_index % 3]
                measured = {}
                for name in order:
                    result = bench(binaries[name], fixture, iterations, args.style, args.margin, work)
                    measured[name] = result["us"]
                    times[name].append(result["us"])
                    digests.setdefault(name, result["sha256"])
                control_changes.append((measured["control"] / measured["baseline"] - 1.0) * 100.0)
                candidate_changes.append((measured["candidate"] / measured["baseline"] - 1.0) * 100.0)
            low, high = quartiles(candidate_changes)
            row = {
                "fixture": str(fixture),
                "path": "ASCII" if probe["path"].startswith("standard") else "Unicode",
                "iterations": iterations,
                "baseline_us": statistics.median(times["baseline"]),
                "candidate_us": statistics.median(times["candidate"]),
                "control_change_pct": statistics.median(control_changes),
                "candidate_change_pct": statistics.median(candidate_changes),
                "candidate_iqr_pct": [low, high],
                "same_bytes": digests["baseline"] == digests["candidate"],
            }
            line = (f"{fixture.name[:28]:28s} {row['path']:8s} {row['baseline_us']:12.1f} "
                    f"{row['control_change_pct']:+7.2f}% {row['candidate_change_pct']:+9.2f}% "
                    f"{low:+7.2f}..{high:+6.2f}% {'same' if row['same_bytes'] else 'differ':>6s}")
            if args.instructions:
                base_ir = instructions_per_build(args.baseline, fixture, args.instruction_iterations,
                                                 args.style, args.margin, work)
                cand_ir = instructions_per_build(args.candidate, fixture, args.instruction_iterations,
                                                 args.style, args.margin, work)
                row["baseline_instructions"] = base_ir
                row["candidate_instructions"] = cand_ir
                row["instruction_change_pct"] = (cand_ir / base_ir - 1.0) * 100.0
                line += f" {row['instruction_change_pct']:+7.2f}%"
            print(line, flush=True)
            results.append(row)

        summary = {
            "control_geomean_pct": geomean_pct([r["control_change_pct"] for r in results]),
            "candidate_geomean_pct": geomean_pct([r["candidate_change_pct"] for r in results]),
        }
        line = (f"{'geometric mean':37s} {'':12s} {summary['control_geomean_pct']:+7.2f}% "
                f"{summary['candidate_geomean_pct']:+9.2f}%")
        if args.instructions:
            summary["instruction_geomean_pct"] = geomean_pct([r["instruction_change_pct"] for r in results])
            line += f" {'':17s} {'':6s} {summary['instruction_geomean_pct']:+7.2f}%"
        print(line)

    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "fixtures": results}, indent=2) + "\n", encoding="utf-8")
    if args.fail_above is not None and summary["candidate_geomean_pct"] > args.fail_above:
        print(f"candidate is {summary['candidate_geomean_pct']:+.2f}% slower than the baseline "
              f"(limit {args.fail_above:+.2f}%)", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
