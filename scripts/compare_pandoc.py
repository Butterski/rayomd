#!/usr/bin/env python3
"""Compare RayoMD end-to-end PDF export against Pandoc.

The benchmark intentionally avoids remote images and other network-dependent
features. It measures process startup, parsing, PDF generation, and writing the
output file for both tools.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def check_pdf(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 128:
        raise RuntimeError(f"PDF is too small: {path}")
    if not data.startswith(b"%PDF-"):
        raise RuntimeError(f"PDF header is invalid: {path}")
    if b"%%EOF" not in data[-2048:]:
        raise RuntimeError(f"PDF EOF marker is missing: {path}")
    return len(data)


def make_large_table(rows: int) -> str:
    lines = [
        "# Large Table Stress",
        "",
        "A simple pipe-table workload with enough rows to force pagination.",
        "",
        "| Row | Service | Region | Status | Latency | Notes |",
        "|---:|---|---|:---:|---:|---|",
    ]
    regions = ["eu-central", "us-east", "ap-south", "local"]
    statuses = ["OK", "WARN", "OK", "OK"]
    for idx in range(1, rows + 1):
        lines.append(
            f"| {idx} | API gateway {idx % 17} | {regions[idx % len(regions)]} | "
            f"{statuses[idx % len(statuses)]} | {25 + (idx % 190)} ms | "
            f"Batch item {idx} with wrapped report text |"
        )
    return "\n".join(lines) + "\n"


def make_nested_lists_and_code(blocks: int, code_lines: int) -> str:
    parts = [
        "# Nested Lists And Code",
        "",
        "This document checks indentation-heavy Markdown and long fenced code blocks.",
        "",
    ]
    for idx in range(1, blocks + 1):
        parts.extend(
            [
                f"## Workflow {idx}",
                "",
                "- Build",
                "  - Configure",
                "    - Validate cache",
                "    - Emit diagnostics",
                "  - Compile",
                "    - Core renderer",
                "    - CLI wrapper",
                "- Verify",
                "  - Export sample",
                "  - Check PDF header",
                "",
            ]
        )
    parts.extend(["```cpp"])
    for idx in range(code_lines):
        parts.append(
            f"if (case_{idx} && renderer.ready()) {{ write_pdf(page_{idx}, options); }}"
        )
    parts.extend(["```", ""])
    return "\n".join(parts)


def make_mixed_doc(target_bytes: int) -> str:
    parts = [
        "# Mixed 100 KiB Report",
        "",
        "Zażółć gęślą jaźń, resume, naive, cafe, Москва, Δοκιμή.",
        "",
    ]
    idx = 0
    while len("\n".join(parts).encode("utf-8")) < target_bytes:
        parts.extend(
            [
                f"## Section {idx}",
                "",
                "This paragraph includes **bold**, *italic*, `inline_code()`, "
                "[a link](https://example.com), and simple math $x^2 + y^2 = z^2$.",
                "",
                "> A block quote stays visually distinct while the document grows.",
                "",
                "- Alpha item",
                "- Beta item",
                "  - Nested beta item",
                "  - Another nested item",
                "",
                "| Metric | Value | Status |",
                "|---|---:|:---:|",
                f"| Rows | {idx * 7 + 3} | OK |",
                f"| Latency | {20 + (idx % 80)} ms | OK |",
                "",
                "```text",
                f"render section={idx} table=true links=true unicode=true",
                "```",
                "",
            ]
        )
        idx += 1
    return "\n".join(parts)


def generate_corpus(root: Path) -> dict[str, Path]:
    corpus = root / "corpus"
    cases = {
        "large_table_500_rows": make_large_table(500),
        "nested_lists_code": make_nested_lists_and_code(80, 240),
        "mixed_100kb": make_mixed_doc(100 * 1024),
    }
    paths: dict[str, Path] = {}
    for name, text in cases.items():
        path = corpus / f"{name}.md"
        write_text(path, text)
        paths[name] = path
    return paths


def run_command(cmd: list[str], timeout: int) -> tuple[int, float, str, str]:
    start = time.perf_counter()
    proc = subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return proc.returncode, elapsed_ms, proc.stdout, proc.stderr


def run_case(
    tool_name: str,
    cmd_template: list[str],
    input_path: Path,
    output_dir: Path,
    runs: int,
    timeout: int,
) -> dict[str, object]:
    output_dir.mkdir(parents=True, exist_ok=True)

    times: list[float] = []
    pdf_sizes: list[int] = []
    for run_idx in range(runs + 1):
        output_path = output_dir / f"{tool_name}_{run_idx:02d}.pdf"
        cmd = [part.format(input=str(input_path), output=str(output_path)) for part in cmd_template]
        rc, elapsed_ms, stdout, stderr = run_command(cmd, timeout)
        (output_dir / f"{tool_name}_{run_idx:02d}.stdout.txt").write_text(
            stdout, encoding="utf-8", newline="\n"
        )
        (output_dir / f"{tool_name}_{run_idx:02d}.stderr.txt").write_text(
            stderr, encoding="utf-8", newline="\n"
        )
        if rc != 0:
            raise RuntimeError(
                f"{tool_name} failed for {input_path.name}, run {run_idx}, rc={rc}\n"
                f"stderr:\n{stderr[-4000:]}"
            )
        pdf_size = check_pdf(output_path)
        if run_idx > 0:
            times.append(elapsed_ms)
            pdf_sizes.append(pdf_size)

    return {
        "median_ms": round(statistics.median(times), 2),
        "min_ms": round(min(times), 2),
        "max_ms": round(max(times), 2),
        "runs": runs,
        "pdf_bytes_median": int(statistics.median(pdf_sizes)),
    }


def markdown_table(results: dict[str, dict[str, dict[str, object]]], inputs: dict[str, Path]) -> str:
    lines = [
        "| Case | Input | RayoMD median | Pandoc median | Ratio | RayoMD PDF | Pandoc PDF |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for name, path in inputs.items():
        rayomd = results[name]["rayomd"]
        pandoc = results[name]["pandoc"]
        ratio = float(pandoc["median_ms"]) / float(rayomd["median_ms"])
        lines.append(
            f"| `{name}` | `{path.stat().st_size:,} bytes` | "
            f"`{rayomd['median_ms']} ms` | `{pandoc['median_ms']} ms` | "
            f"`{ratio:.1f}x` | `{rayomd['pdf_bytes_median']:,}` | "
            f"`{pandoc['pdf_bytes_median']:,}` |"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rayomd", required=True, type=Path)
    parser.add_argument("--pandoc", default="pandoc")
    parser.add_argument("--root", default=Path("benchmark-output/pandoc-comparison"), type=Path)
    parser.add_argument("--runs", default=5, type=int)
    parser.add_argument("--timeout", default=180, type=int)
    parser.add_argument("--style", default="modern")
    parser.add_argument("--margin", default="normal")
    args = parser.parse_args()

    root = args.root
    inputs = generate_corpus(root)
    outputs = root / "outputs"

    rayomd_template = [
        str(args.rayomd),
        "--export",
        "{input}",
        "{output}",
        "native",
        args.style,
        args.margin,
    ]
    pandoc_template = [
        args.pandoc,
        "{input}",
        "-o",
        "{output}",
        "--pdf-engine=xelatex",
        "--no-highlight",
        "-V",
        "mainfont=Arial",
        "-V",
        "monofont=Courier New",
    ]

    results: dict[str, dict[str, dict[str, object]]] = {}
    for name, input_path in inputs.items():
        results[name] = {
            "rayomd": run_case(
                "rayomd",
                rayomd_template,
                input_path,
                outputs / name,
                args.runs,
                args.timeout,
            ),
            "pandoc": run_case(
                "pandoc",
                pandoc_template,
                input_path,
                outputs / name,
                args.runs,
                args.timeout,
            ),
        }

    record = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "runs_after_warmup": args.runs,
        "rayomd": str(args.rayomd),
        "rayomd_bytes": args.rayomd.stat().st_size,
        "pandoc": args.pandoc,
        "style": args.style,
        "margin": args.margin,
        "cases": {
            name: {
                "input": str(path),
                "input_bytes": path.stat().st_size,
                **results[name],
            }
            for name, path in inputs.items()
        },
    }
    root.mkdir(parents=True, exist_ok=True)
    (root / "record.json").write_text(
        json.dumps(record, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    table = markdown_table(results, inputs)
    (root / "summary.md").write_text(table + "\n", encoding="utf-8", newline="\n")
    print(table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
