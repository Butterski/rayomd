#!/usr/bin/env python3
"""Order-balanced fresh-process A/B benchmark for RayoMD one-shot exports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


PINNED_CASES = ("tiny_readme", "medium_features", "table_500_rows")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[float], percentile_value: float) -> float:
    """Linear interpolation between closest ranks, matching inclusive quantiles."""
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentile_value
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def validate_image_free(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if re.search(r"!\s*\[", text) or re.search(r"<\s*img\b", text, re.IGNORECASE):
        raise RuntimeError(f"pinned input contains image syntax: {path}")


def validate_pdf(path: Path) -> tuple[int, str]:
    data = path.read_bytes()
    if len(data) < 128 or not data.startswith(b"%PDF-") or b"%%EOF" not in data[-2048:]:
        raise RuntimeError(f"invalid PDF output: {path}")
    return len(data), hashlib.sha256(data).hexdigest()


def version(binary: Path) -> str:
    result = subprocess.run(
        [str(binary), "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
        check=True,
    )
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() else "unknown"


def run_export(binary: Path, source: Path, output: Path, timeout: int) -> dict[str, Any]:
    started = time.perf_counter_ns()
    result = subprocess.run(
        [
            str(binary),
            "--export",
            str(source),
            str(output),
            "native",
            "modern",
            "normal",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
    if result.returncode:
        raise RuntimeError(
            f"export failed ({result.returncode}): {binary}\n"
            f"stdout={result.stdout[-2000:]!r}\nstderr={result.stderr[-2000:]!r}"
        )
    size, digest = validate_pdf(output)
    return {
        "wall_ms": elapsed_ms,
        "pdf_bytes": size,
        "pdf_sha256": digest,
    }


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median_ms": statistics.median(values),
        "min_ms": min(values),
        "max_ms": max(values),
        "p95_ms": percentile(values, 0.95),
    }


def rounded(value: Any) -> Any:
    if isinstance(value, float):
        return round(value, 4)
    if isinstance(value, dict):
        return {key: rounded(item) for key, item in value.items()}
    if isinstance(value, list):
        return [rounded(item) for item in value]
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(rounded(value), indent=2) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--runs", default=31, type=int)
    parser.add_argument("--preflights", default=3, type=int)
    parser.add_argument("--timeout", default=30, type=int)
    args = parser.parse_args()

    if args.runs < 31:
        raise RuntimeError("fresh-process acceptance runs must be at least 31")
    if args.preflights < 1 or args.timeout < 1:
        raise RuntimeError("preflights and timeout must be positive")

    binaries = {
        "baseline": args.baseline.resolve(),
        "candidate": args.candidate.resolve(),
    }
    for label, binary in binaries.items():
        if not binary.is_file():
            raise RuntimeError(f"{label} binary does not exist: {binary}")

    cases = {name: (args.corpus / f"{name}.md").resolve() for name in PINNED_CASES}
    for path in cases.values():
        validate_image_free(path)

    root = args.root.resolve()
    output_root = root / "outputs"
    output_root.mkdir(parents=True, exist_ok=True)
    samples: list[dict[str, Any]] = []

    # Uncounted preflights exercise both binary orders and rotate case order.
    for preflight in range(args.preflights):
        labels = ("baseline", "candidate") if preflight % 2 == 0 else ("candidate", "baseline")
        ordered_cases = PINNED_CASES[preflight % len(PINNED_CASES) :] + PINNED_CASES[: preflight % len(PINNED_CASES)]
        for case in ordered_cases:
            for label in labels:
                run_export(
                    binaries[label], cases[case], output_root / f"{case}-{label}.pdf", args.timeout
                )

    # Each pair contains one baseline and one candidate run. Alternating binary
    # order and rotating/reversing case order balance short-lived machine drift.
    for pair in range(1, args.runs + 1):
        labels = ("baseline", "candidate") if pair % 2 == 1 else ("candidate", "baseline")
        offset = (pair - 1) % len(PINNED_CASES)
        ordered_cases = PINNED_CASES[offset:] + PINNED_CASES[:offset]
        if ((pair - 1) // len(PINNED_CASES)) % 2 == 1:
            ordered_cases = tuple(reversed(ordered_cases))
        for case in ordered_cases:
            for order, label in enumerate(labels, start=1):
                measurement = run_export(
                    binaries[label], cases[case], output_root / f"{case}-{label}.pdf", args.timeout
                )
                samples.append(
                    {
                        "pair": pair,
                        "case": case,
                        "order": order,
                        "label": label,
                        **measurement,
                    }
                )

    case_summaries: dict[str, Any] = {}
    baseline_medians: list[float] = []
    candidate_medians: list[float] = []
    all_hashes_match = True
    for case in PINNED_CASES:
        by_label = {
            label: [sample for sample in samples if sample["case"] == case and sample["label"] == label]
            for label in binaries
        }
        baseline_times = [sample["wall_ms"] for sample in by_label["baseline"]]
        candidate_times = [sample["wall_ms"] for sample in by_label["candidate"]]
        baseline_by_pair = {sample["pair"]: sample["wall_ms"] for sample in by_label["baseline"]}
        candidate_by_pair = {sample["pair"]: sample["wall_ms"] for sample in by_label["candidate"]}
        paired_ms = [candidate_by_pair[pair] - baseline_by_pair[pair] for pair in range(1, args.runs + 1)]
        paired_pct = [
            (baseline_by_pair[pair] - candidate_by_pair[pair]) / baseline_by_pair[pair] * 100.0
            for pair in range(1, args.runs + 1)
        ]
        baseline_hashes = sorted({sample["pdf_sha256"] for sample in by_label["baseline"]})
        candidate_hashes = sorted({sample["pdf_sha256"] for sample in by_label["candidate"]})
        hashes_match = baseline_hashes == candidate_hashes and len(baseline_hashes) == 1
        all_hashes_match &= hashes_match
        baseline_summary = summarize(baseline_times)
        candidate_summary = summarize(candidate_times)
        baseline_medians.append(baseline_summary["median_ms"])
        candidate_medians.append(candidate_summary["median_ms"])
        case_summaries[case] = {
            "baseline": baseline_summary,
            "candidate": candidate_summary,
            "paired_median_delta_ms": statistics.median(paired_ms),
            "paired_median_improvement_pct": statistics.median(paired_pct),
            "candidate_faster_pairs": sum(delta < 0 for delta in paired_ms),
            "pdf_hashes_match": hashes_match,
            "pdf_sha256": baseline_hashes[0] if hashes_match else None,
            "pdf_bytes": by_label["baseline"][0]["pdf_bytes"],
        }

    baseline_geomean = statistics.geometric_mean(baseline_medians)
    candidate_geomean = statistics.geometric_mean(candidate_medians)
    summary = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
        "method": "fresh process through exit; filesystem/OS caches may be warm; order-balanced A/B pairs",
        "configuration": {
            "runs_per_binary_per_case": args.runs,
            "uncounted_preflights_per_binary_per_case": args.preflights,
            "url_images_enabled": False,
            "image_free_corpus_validated": True,
        },
        "binaries": {
            label: {
                "path": str(binary),
                "bytes": binary.stat().st_size,
                "sha256": sha256_file(binary),
                "version": version(binary),
            }
            for label, binary in binaries.items()
        },
        "inputs": {
            case: {
                "path": str(path),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for case, path in cases.items()
        },
        "cases": case_summaries,
        "geometric_mean": {
            "baseline_ms": baseline_geomean,
            "candidate_ms": candidate_geomean,
            "improvement_pct": (baseline_geomean - candidate_geomean) / baseline_geomean * 100.0,
        },
        "all_candidate_pdfs_match_frozen_baseline": all_hashes_match,
    }
    write_json(root / "record.json", samples)
    write_json(root / "summary.json", summary)
    print(json.dumps(rounded(summary), indent=2))
    return 0 if all_hashes_match else 2


if __name__ == "__main__":
    raise SystemExit(main())
