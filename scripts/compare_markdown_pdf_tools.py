#!/usr/bin/env python3
"""Fresh-process Markdown-to-PDF comparison for RayoMD, Pandoc, and md-to-pdf."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_SOURCE_URL = "https://daringfireball.net/projects/markdown/syntax.text"
MIB = 1024 * 1024


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch_source(url: str, cache_path: Path, offline: bool) -> tuple[bytes, str]:
    if cache_path.exists():
        return cache_path.read_bytes(), "cache"
    if offline:
        raise RuntimeError(f"offline source cache is missing: {cache_path}")

    request = urllib.request.Request(url, headers={"User-Agent": "RayoMD comparison benchmark/1"})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read(2 * MIB + 1)
    if not data:
        raise RuntimeError(f"downloaded source is empty: {url}")
    if len(data) > 2 * MIB:
        raise RuntimeError(f"downloaded source exceeds the 2 MiB safety limit: {url}")
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_bytes(data)
    return data, "download"


def scaled_document(source: bytes, target_bytes: int) -> tuple[bytes, int]:
    source = source.rstrip(b"\r\n") + b"\n"
    parts: list[bytes] = []
    current_bytes = 0
    copies = 0
    while current_bytes < target_bytes:
        copies += 1
        separator = b"" if copies == 1 else f"\n\n<!-- RayoMD scale copy {copies} -->\n\n".encode("ascii")
        part = separator + source
        parts.append(part)
        current_bytes += len(part)
    return b"".join(parts), copies



def corpus(root: Path, source_url: str, offline: bool, pinned_only: bool = False) -> tuple[dict[str, Path], dict[str, object]]:
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
    paths: dict[str, Path] = {}
    cases: dict[str, dict[str, object]] = {}
    for name, value in values.items():
        paths[name] = root / "corpus" / f"{name}.md"
        write(paths[name], value)
        cases[name] = {"kind": "synthetic", "copies": 1}

    if pinned_only:
        return paths, {"source": None, "cases": cases}

    cache_name = f"{sha256(source_url.encode('utf-8'))[:16]}.text"
    source_bytes, source_origin = fetch_source(source_url, root / "downloads" / cache_name, offline)
    authentic = root / "corpus" / "gruber_markdown_syntax.md"
    authentic.parent.mkdir(parents=True, exist_ok=True)
    authentic.write_bytes(source_bytes)
    paths["gruber_syntax_authentic"] = authentic
    cases["gruber_syntax_authentic"] = {"kind": "authentic", "copies": 1}

    for name, target in (("gruber_syntax_scaled_1mib", MIB), ("gruber_syntax_scaled_5mib", 5 * MIB)):
        data, copies = scaled_document(source_bytes, target)
        path = root / "corpus" / f"{name}.md"
        path.write_bytes(data)
        paths[name] = path
        cases[name] = {"kind": "scaled", "copies": copies, "target_bytes": target}

    return paths, {
        "source": {
            "url": source_url,
            "bytes": len(source_bytes),
            "sha256": sha256(source_bytes),
            "origin": source_origin,
            "contains_inline_html": True,
        },
        "cases": cases,
    }


def version(command: list[str]) -> str:
    proc = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    return proc.stdout.splitlines()[0].strip() if proc.stdout else "unknown"


def valid_pdf(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 128 or not data.startswith(b"%PDF-") or b"%%EOF" not in data[-2048:]:
        raise RuntimeError(f"invalid PDF: {path}")
    return len(data)


def measure(name: str, command: list[str], output: Path, runs: int, timeout: int, warmups: int, preflight_timeout: int) -> dict[str, object]:
    times: list[float] = []
    sizes: list[int] = []
    for index in range(warmups + runs):
        output.unlink(missing_ok=True)
        started = time.perf_counter()
        attempt_timeout = preflight_timeout if index < warmups else timeout
        try:
            proc = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=attempt_timeout,
            )
        except subprocess.TimeoutExpired as error:
            output.unlink(missing_ok=True)
            return {
                "status": "timeout",
                "timeout_seconds": attempt_timeout,
                "attempt": index + 1,
                "stderr": (error.stderr or "")[-4000:] if isinstance(error.stderr, str) else "",
            }
        elapsed = (time.perf_counter() - started) * 1000
        if proc.returncode:
            output.unlink(missing_ok=True)
            return {"status": "error", "returncode": proc.returncode, "stderr": proc.stderr[-4000:]}
        try:
            size = valid_pdf(output)
        except (OSError, RuntimeError) as error:
            return {"status": "invalid_output", "error": str(error)}
        if index >= warmups:
            times.append(elapsed)
            sizes.append(size)
    return {
        "status": "ok",
        "median_ms": round(statistics.median(times), 2),
        "min_ms": round(min(times), 2),
        "max_ms": round(max(times), 2),
        "runs": runs,
        "warmups": warmups,
        "pdf_bytes_median": int(statistics.median(sizes)),
    }

def format_measurement(result: dict[str, object]) -> str:
    status = str(result["status"])
    if status == "ok":
        return f"`{float(result['median_ms']):.2f} ms`"
    if status == "timeout":
        return f"**TIMEOUT >{int(result['timeout_seconds'])} s**"
    if status == "skipped_after_timeout":
        return "SKIPPED after smaller-case timeout"
    if status == "skipped_unavailable":
        return f"SKIPPED ({result.get('reason', 'tool unavailable')})"
    if status == "error":
        return f"ERROR ({int(result['returncode'])})"
    return status.upper()



def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rayomd", required=True, type=Path)
    parser.add_argument("--node-modules", required=True, type=Path)
    parser.add_argument("--root", default=Path("benchmark-output/markdown-pdf-tools"), type=Path)
    parser.add_argument("--runs", default=7, type=int)
    parser.add_argument("--timeout", default=60, type=int)
    parser.add_argument("--large-runs", default=1, type=int)
    parser.add_argument("--large-timeout", default=60, type=int)
    parser.add_argument("--preflight-timeout", default=15, type=int)
    parser.add_argument("--source-url", default=DEFAULT_SOURCE_URL)
    parser.add_argument("--offline", action="store_true", help="reuse the cached real-world source")
    parser.add_argument("--pinned-only", action="store_true", help="measure only the image-free synthetic README cases")
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    if min(args.runs, args.large_runs, args.timeout, args.large_timeout, args.preflight_timeout) < 1:
        raise RuntimeError("run counts and timeouts must be at least 1")

    inputs, corpus_metadata = corpus(args.root, args.source_url, args.offline, args.pinned_only)
    runner = Path(__file__).with_name("md_to_pdf_runner.cjs").resolve()
    node_modules = args.node_modules.resolve()
    tools = {
        "rayomd": lambda src, dst: [str(args.rayomd.resolve()), "--export", str(src.resolve()), str(dst.resolve()), "native", "modern", "normal"],
        "pandoc_xelatex": lambda src, dst: ["pandoc", str(src.resolve()), "-o", str(dst.resolve()), "--pdf-engine=xelatex", "--no-highlight", "-V", "mainfont=Arial", "-V", "monofont=Courier New"],
        "md_to_pdf": lambda src, dst: ["node", str(runner), str(node_modules), str(src.resolve()), str(dst.resolve())],
    }
    case_metadata = corpus_metadata["cases"]
    results: dict[str, dict[str, object]] = {}
    unavailable_tools: dict[str, str] = {}
    first_case = next(iter(inputs))
    partial_path = args.root / "record.partial.json"
    for case, src in inputs.items():
        results[case] = {}
        for tool, command_for in tools.items():
            dst = args.root / "outputs" / case / f"{tool}.pdf"
            dst.parent.mkdir(parents=True, exist_ok=True)
            large_case = case_metadata[case]["kind"] == "scaled"
            if tool in unavailable_tools:
                reason = unavailable_tools[tool]
                print(f"skipping {case}: {tool} ({reason})", flush=True)
                result = {"status": "skipped_unavailable", "reason": reason}
            else:
                print(f"measuring {case}: {tool}", flush=True)
                result = measure(
                    tool, command_for(src, dst), dst,
                    args.large_runs if large_case else args.runs,
                    args.large_timeout if large_case else args.timeout,
                    0 if large_case else 1, args.preflight_timeout,
                )
            results[case][tool] = result
            if result["status"] == "timeout":
                unavailable_tools[tool] = f"timed out in {case}"
            elif case == first_case and result["status"] != "ok":
                unavailable_tools[tool] = f"preflight {result['status']}"
            write(
                partial_path,
                json.dumps({
                    "status": "running",
                    "updated_at": datetime.now(timezone.utc).isoformat(),
                    "corpus": corpus_metadata,
                    "results": results,
                }, indent=2, ensure_ascii=False) + "\n",
            )

    speedups: dict[str, float | None] = {}
    wins = 0
    large_cases: list[str] = []
    scored_cases = 0
    for case, row in results.items():
        rayo_result = row["rayomd"]
        competitors = [
            float(row[name]["median_ms"])
            for name in ("pandoc_xelatex", "md_to_pdf")
            if row[name]["status"] == "ok"
        ]
        if rayo_result["status"] != "ok" or not competitors:
            speedups[case] = None
            continue
        rayo = float(rayo_result["median_ms"])
        fastest_competitor = min(competitors)
        speedups[case] = fastest_competitor / rayo
        scored_cases += 1
        wins += int(rayo <= fastest_competitor)
        if case_metadata[case]["kind"] in ("authentic", "scaled"):
            large_cases.append(case)
    scored_speedups = [value for value in speedups.values() if value is not None]
    large_speedups = [speedups[case] for case in large_cases if speedups[case] is not None]
    overall_geomean = round(statistics.geometric_mean(scored_speedups), 2) if scored_speedups else None
    large_geomean = round(statistics.geometric_mean(large_speedups), 2) if large_speedups else None
    scores = {
        "wins": wins,
        "scored_cases": scored_cases,
        "total_cases": len(results),
        "overall_geomean_vs_fastest_competitor": overall_geomean,
        "large_geomean_vs_fastest_competitor": large_geomean,
    }

    metadata = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
        "method": "fresh process; one uncounted warm-up for non-scaled cases; no warm-up for scaled cases; output file overwritten each run",
        "compatibility_note": "The Gruber source contains inline/block HTML and Markdown features outside RayoMD's focused native subset; timings do not imply rendering equivalence.",
        "corpus": corpus_metadata,
        "configuration": {
            "runs": args.runs,
            "timeout_seconds": args.timeout,
            "large_runs": args.large_runs,
            "large_timeout_seconds": args.large_timeout,
            "preflight_timeout_seconds": args.preflight_timeout,
            "disable_tool_after_timeout": True,
            "scope": "pinned-image-free" if args.pinned_only else "full",
        },
        "versions": {
            "rayomd": version([str(args.rayomd.resolve()), "--version"]),
            "pandoc": version(["pandoc", "--version"]),
            "xelatex": version(["xelatex", "--version"]),
            "node": version(["node", "--version"]),
            "md-to-pdf": json.loads((node_modules / "md-to-pdf" / "package.json").read_text(encoding="utf-8"))["version"],
        },
        "inputs": {
            name: {"path": str(path), "bytes": path.stat().st_size, **case_metadata[name]}
            for name, path in inputs.items()
        },
        "results": results,
        "speedups_vs_fastest_competitor": speedups,
        "scores": scores,
    }
    write(args.root / "record.json", json.dumps(metadata, indent=2, ensure_ascii=False) + "\n")
    lines = [
        "# Markdown-to-PDF comparison", "",
        "- Scope: pinned image-free synthetic README cases." if args.pinned_only else f"- Real-world source: {args.source_url}",
    ]
    if args.pinned_only:
        lines.append("- The measured inputs contain no Markdown or HTML image syntax; RayoMD URL-image fetching is disabled.")
    else:
        lines.extend([
            f"- Source SHA-256: `{corpus_metadata['source']['sha256']}`",
            "- The source contains HTML and features outside RayoMD's native subset; timings do not imply rendering equivalence.",
            "- Scaled cases repeat the complete source and measure size scaling, not additional syntax diversity.",
        ])
    lines.extend(["", "| Case | Kind | Input | RayoMD | Pandoc + XeLaTeX | md-to-pdf | vs fastest competitor |",
        "|---|---|---:|---:|---:|---:|---:|",
    ])
    for case, src in inputs.items():
        row = results[case]
        kind = case_metadata[case]["kind"]
        speedup = speedups[case]
        speedup_text = f"`{speedup:.1f}x`" if speedup is not None else "not scored"
        lines.append(
            f"| `{case}` | {kind} | `{src.stat().st_size:,} B` | "
            f"{format_measurement(row['rayomd'])} | {format_measurement(row['pandoc_xelatex'])} | "
            f"{format_measurement(row['md_to_pdf'])} | {speedup_text} |"
        )
    lines.append("")
    if overall_geomean is None:
        lines.append(f"Overall score: no cases were scored ({scores['wins']}/{scores['scored_cases']} wins).")
    else:
        lines.append(f"Overall score: RayoMD won **{scores['wins']}/{scores['scored_cases']}** scored cases and was **{overall_geomean:.1f}x** faster than the fastest completed competitor by geometric mean.")
    if large_geomean is not None:
        lines.append(f"Large-document score: RayoMD was **{large_geomean:.1f}x** faster across the authentic, 1 MiB, and 5 MiB cases.")
    write(args.root / "summary.md", "\n".join(lines) + "\n")
    partial_path.unlink(missing_ok=True)
    print("\n" + "\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
