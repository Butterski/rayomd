#!/usr/bin/env python3
"""Fresh-process Markdown-to-PDF comparison for RayoMD, Pandoc, and md-to-pdf."""

from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def corpus(root: Path) -> dict[str, Path]:
    tiny = """# Release notes

RayoMD converts Markdown to PDF with a small native renderer.

- Fast startup
- Tables and links
- No browser required

See [the project](https://github.com/Butterski/rayomd) for details.
"""
    medium_parts = ["# Engineering report", ""]
    for index in range(50):
        medium_parts += [
            f"## Component {index + 1}", "",
            "A practical paragraph with **bold text**, *emphasis*, `inline_code()`, and a [link](https://example.com).", "",
            "> The native path intentionally implements a focused Markdown subset.", "",
            "- Parse input", "  - Validate spans", "  - Lay out pages", "- Write PDF", "",
            "```cpp", f"render_component({index}, options);", "```", "",
        ]
    table = [
        "# Service inventory", "",
        "| Row | Service | Region | Status | Latency | Notes |",
        "|---:|---|---|:---:|---:|---|",
    ]
    for index in range(1, 501):
        table.append(f"| {index} | gateway-{index % 17} | eu-central | OK | {20 + index % 180} ms | synthetic row {index} |")
    values = {
        "tiny_readme": tiny,
        "medium_features": "\n".join(medium_parts),
        "table_500_rows": "\n".join(table) + "\n",
    }
    paths = {}
    for name, value in values.items():
        paths[name] = root / "corpus" / f"{name}.md"
        write(paths[name], value)
    return paths


def version(command: list[str]) -> str:
    proc = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    return proc.stdout.splitlines()[0].strip() if proc.stdout else "unknown"


def valid_pdf(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 128 or not data.startswith(b"%PDF-") or b"%%EOF" not in data[-2048:]:
        raise RuntimeError(f"invalid PDF: {path}")
    return len(data)


def measure(name: str, command: list[str], output: Path, runs: int, timeout: int) -> dict[str, object]:
    times = []
    sizes = []
    for index in range(runs + 1):
        output.unlink(missing_ok=True)
        started = time.perf_counter()
        proc = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        elapsed = (time.perf_counter() - started) * 1000
        if proc.returncode:
            raise RuntimeError(f"{name} failed ({proc.returncode}):\n{proc.stderr[-4000:]}")
        size = valid_pdf(output)
        if index:
            times.append(elapsed)
            sizes.append(size)
    return {
        "median_ms": round(statistics.median(times), 2),
        "min_ms": round(min(times), 2),
        "max_ms": round(max(times), 2),
        "runs": runs,
        "pdf_bytes_median": int(statistics.median(sizes)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rayomd", required=True, type=Path)
    parser.add_argument("--node-modules", required=True, type=Path)
    parser.add_argument("--root", default=Path("benchmark-output/markdown-pdf-tools"), type=Path)
    parser.add_argument("--runs", default=7, type=int)
    parser.add_argument("--timeout", default=180, type=int)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    inputs = corpus(args.root)
    runner = Path(__file__).with_name("md_to_pdf_runner.cjs").resolve()
    node_modules = args.node_modules.resolve()
    tools = {
        "rayomd": lambda src, dst: [str(args.rayomd.resolve()), "--export", str(src.resolve()), str(dst.resolve()), "native", "modern", "normal"],
        "pandoc_xelatex": lambda src, dst: ["pandoc", str(src.resolve()), "-o", str(dst.resolve()), "--pdf-engine=xelatex", "--no-highlight", "-V", "mainfont=Arial", "-V", "monofont=Courier New"],
        "md_to_pdf": lambda src, dst: ["node", str(runner), str(node_modules), str(src.resolve()), str(dst.resolve())],
    }
    results: dict[str, dict[str, object]] = {}
    for case, src in inputs.items():
        results[case] = {}
        for tool, command_for in tools.items():
            dst = args.root / "outputs" / case / f"{tool}.pdf"
            dst.parent.mkdir(parents=True, exist_ok=True)
            print(f"measuring {case}: {tool}", flush=True)
            results[case][tool] = measure(tool, command_for(src, dst), dst, args.runs, args.timeout)

    metadata = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
        "method": "fresh process; one uncounted warm-up; median of measured runs; output file overwritten each run",
        "versions": {
            "rayomd": version([str(args.rayomd.resolve()), "--version"]),
            "pandoc": version(["pandoc", "--version"]),
            "xelatex": version(["xelatex", "--version"]),
            "node": version(["node", "--version"]),
            "md-to-pdf": json.loads((node_modules / "md-to-pdf" / "package.json").read_text(encoding="utf-8"))["version"],
        },
        "inputs": {name: {"path": str(path), "bytes": path.stat().st_size} for name, path in inputs.items()},
        "results": results,
    }
    write(args.root / "record.json", json.dumps(metadata, indent=2, ensure_ascii=False) + "\n")
    lines = [
        "| Case | Input | RayoMD | Pandoc + XeLaTeX | md-to-pdf | vs next fastest |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for case, src in inputs.items():
        row = results[case]
        rayo = float(row["rayomd"]["median_ms"])
        next_fastest = min(float(row[name]["median_ms"]) for name in ("pandoc_xelatex", "md_to_pdf"))
        lines.append(f"| `{case}` | `{src.stat().st_size:,} B` | `{rayo:.2f} ms` | `{row['pandoc_xelatex']['median_ms']:.2f} ms` | `{row['md_to_pdf']['median_ms']:.2f} ms` | `{next_fastest / rayo:.1f}x` |")
    write(args.root / "summary.md", "\n".join(lines) + "\n")
    print("\n" + "\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
