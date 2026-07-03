#!/usr/bin/env python3
"""Watch RayoMD conversion speed across versions.

It generates a deterministic supported-Markdown corpus, runs the existing
no-UI CLI modes, appends one JSONL history record for local reporting, compares
against explicit baseline records for pass/fail gates, and can write compact
version benchmark records for release tracking.
"""

from __future__ import annotations

import argparse
import base64
import json
import platform as platform_module
import random
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4"
    "z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"
)

WORDS = [
    "markdown",
    "renderer",
    "glyph",
    "unicode",
    "latency",
    "pipeline",
    "cache",
    "allocation",
    "paragraph",
    "table",
    "conversion",
    "baseline",
    "vector",
    "document",
    "profile",
    "batch",
    "layout",
    "export",
    "polski",
    "za\u017c\u00f3\u0142\u0107",
    "g\u0119\u015bl\u0105",
    "ja\u017a\u0144",
    "resume",
    "naive",
    "caf\u00e9",
    "\u6771\u4eac",
    "\u041c\u043e\u0441\u043a\u0432\u0430",
    "\u0394\u03bf\u03ba\u03b9\u03bc\u03ae",
]

INLINE = [
    "**bold text**",
    "*italic span*",
    "~~strike marker~~",
    "`inline_code()`",
    "[link label](https://example.com/docs/perf-watch)",
    "$x^2 + y^2 = z^2$",
    ":)",
    ":warning:",
]

STYLES = ("elegant", "modern", "tech")
MARGINS = ("compact", "normal", "wide", "margin=0.75in")
FEATURE_MODES = ("baseline", "rules", "pagebreaks", "comments")
SIZED_KINDS = ("ascii", "unicode", "table")
KIB = 1024
WATCH_VERSION = 3
COMPARISON_KEY_FIELDS = ("platform", "suite", "seed", "style", "margin", "image_mode", "watch_version")

SUITES = {
    "quick": {
        "single_docs": 2,
        "batch_docs": 8,
        "sections_min": 3,
        "sections_max": 7,
        "paragraphs_min": 1,
        "paragraphs_max": 3,
        "bench_iterations": 80,
        "bench_rounds": 1,
        "cold_runs": 3,
        "feature_sections": 12,
        "feature_iterations": 60,
        "sized_doc_bytes": 24 * KIB,
        "sized_iterations": 40,
    },
    "watch": {
        "single_docs": 4,
        "batch_docs": 40,
        "sections_min": 5,
        "sections_max": 14,
        "paragraphs_min": 2,
        "paragraphs_max": 5,
        "bench_iterations": 250,
        "bench_rounds": 2,
        "cold_runs": 8,
        "feature_sections": 72,
        "feature_iterations": 150,
        "sized_doc_bytes": 96 * KIB,
        "sized_iterations": 80,
    },
    "full": {
        "single_docs": 8,
        "batch_docs": 160,
        "sections_min": 8,
        "sections_max": 30,
        "paragraphs_min": 2,
        "paragraphs_max": 6,
        "bench_iterations": 500,
        "bench_rounds": 3,
        "cold_runs": 16,
        "feature_sections": 180,
        "feature_iterations": 300,
        "sized_doc_bytes": 512 * KIB,
        "sized_iterations": 60,
    },
}

TIME_METRICS = [
    "warm_avg_ms_median",
    "warm_avg_ms_p95",
    "cold_export_ms_median",
    "cold_export_ms_p95",
    "batch_ms_total",
    "batch_ms_per_file",
    "stdin_batch_ms_total",
    "stdin_batch_ms_per_file",
    "serve_wall_ms_total",
    "serve_wall_ms_per_file",
    "serve_reported_ms_median",
    "serve_reported_ms_p95",
]

VERSION_INDEX_METRICS = [
    "warm_avg_ms_median",
    "cold_export_ms_median",
    "batch_ms_per_file",
    "stdin_batch_ms_per_file",
    "serve_reported_ms_median",
]


def now_utc() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def safe_name(value: str) -> str:
    keep = []
    for ch in value.lower():
        keep.append(ch if ch.isalnum() or ch in ("-", "_") else "-")
    return "".join(keep).strip("-") or "run"


def safe_version_name(value: str) -> str:
    keep = []
    for ch in value.lower().lstrip("v"):
        keep.append(ch if ch.isalnum() or ch in ("-", "_", ".") else "-")
    return "".join(keep).strip("-.") or "unknown"


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * pct
    low = int(pos)
    high = min(low + 1, len(ordered) - 1)
    frac = pos - low
    return ordered[low] * (1.0 - frac) + ordered[high] * frac


def f2(value: float) -> float:
    return round(value, 4)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def read_project_version() -> str:
    version_path = repo_root() / "VERSION"
    if not version_path.exists():
        return ""
    lines = version_path.read_text(encoding="utf-8").splitlines()
    return lines[0].strip() if lines else ""


def read_binary_version(binary: Path) -> str:
    try:
        proc = subprocess.run(
            [str(binary), "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
        )
    except Exception:
        return ""
    if proc.returncode != 0:
        return ""
    text = proc.stdout.strip() or proc.stderr.strip()
    parts = text.split()
    if len(parts) >= 2 and parts[0].lower() == "rayomd":
        return parts[1]
    return text


def sentence(rng: random.Random, min_words: int = 10, max_words: int = 28) -> str:
    parts: list[str] = []
    for _ in range(rng.randint(min_words, max_words)):
        parts.append(rng.choice(INLINE) if rng.random() < 0.15 else rng.choice(WORDS))
    text = " ".join(parts)
    return text[:1].upper() + text[1:] + "."


def table(rng: random.Random, rows: int, cols: int) -> str:
    headers = [f"Column {idx + 1}" for idx in range(cols)]
    aligns = ["---", ":---", "---:", ":---:"]
    out = ["| " + " | ".join(headers) + " |"]
    out.append("| " + " | ".join(aligns[idx % len(aligns)] for idx in range(cols)) + " |")
    for _ in range(rows):
        cells = [sentence(rng, 3, 8).replace("|", "/") for _ in range(cols)]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def code_block(rng: random.Random, lines: int) -> str:
    out = ["```cpp"]
    for idx in range(lines):
        name = rng.choice(("width", "cid", "glyph", "line", "offset"))
        out.append(f"auto {name}_{idx} = compute_{name}({idx}, \"{rng.choice(WORDS)}\");")
    out.append("```")
    return "\n".join(out)


def make_doc(seed: int, sections: int, paragraphs: int, image_mode: str) -> str:
    rng = random.Random(seed)
    out = [
        "---",
        f"title: Perf Watch {seed}",
        "---",
        "",
        f"# Perf Watch Document {seed}",
        "",
    ]

    for section in range(sections):
        level = 2 + (section % 3)
        out.extend(["#" * level + f" Section {section + 1}", ""])

        for _ in range(paragraphs):
            out.append(" ".join(sentence(rng) for _ in range(rng.randint(1, 4))))
            out.append("")

        out.append("> " + " ".join(sentence(rng, 8, 18) for _ in range(2)))
        out.append("")

        item_count = rng.randint(3, 7)
        for idx in range(item_count):
            out.append(f"- {sentence(rng, 5, 14)}")
            if idx % 3 == 1:
                out.append(f"  - {sentence(rng, 4, 10)}")
        out.append("")

        for idx in range(rng.randint(3, 6)):
            out.append(f"{idx + 1}. {sentence(rng, 5, 14)}")
        out.append("")

        if section % 2 == 0:
            out.extend([table(rng, rows=rng.randint(3, 9), cols=rng.randint(3, 5)), ""])

        if section % 3 == 0:
            out.extend([code_block(rng, lines=rng.randint(6, 16)), ""])

        if section % 4 == 0:
            out.extend(
                [
                    "$$",
                    r"\int_0^\infty e^{-x^2} dx = \frac{\sqrt{\pi}}{2}",
                    "$$",
                    "",
                ]
            )

        if section % 5 == 0:
            out.extend(["---", ""])

        if section % 7 == 0:
            out.extend([r"\pagebreak", ""])

        if image_mode != "off" and section == 0:
            out.extend(
                [
                    "![Local sample](assets/tiny.png)",
                    "",
                    "![Missing local fallback](assets/missing-image.png)",
                    "",
                ]
            )

    return "\n".join(out)


def make_feature_doc(sections: int, mode: str) -> str:
    parts = ["# Markdown Feature Watch", ""]
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

    return "\n".join(parts)


def sized_block(kind: str, idx: int) -> str:
    if kind == "unicode":
        return f"""
# Unicode Sized Section {idx}

Zażółć gęślą jaźń, résumé, naïve café, Αθήνα, 東京, Москва, and Δοκιμή appear
next to **bold Unicode**, *italic Unicode*, ~~struck Unicode~~, `kod_inline()`,
and [international links](https://example.com/produkt/sciezka).

> A quoted paragraph keeps diacritics and multilingual words together while testing wrapping and embedded-font output.

1. Polish and European text with diacritics.
2. Greek, Cyrillic, and CJK text.
3. Mixed punctuation, math cleanup, and status fallback markers.

| Locale | Text | Marker |
| :--- | :--- | ---: |
| PL | Zażółć gęślą jaźń | {idx} |
| JP | 東京の文書変換 | {idx} |
| GR | Δοκιμή εγγράφου | {idx} |

"""
    if kind == "table":
        return f"""
# Dense Tables And Code {idx}

| Column A | Column B | Column C | Column D | Column E |
| :--- | ---: | :---: | ---: | :--- |
| alpha {idx} | 12345 | centered value | 67.89 | markdown renderer |
| beta {idx} | 67890 | styled **cell** | 12.34 | batch conversion |
| gamma {idx} | 24680 | `inline_table_code()` | 98.76 | predictable output |
| delta {idx} | 13579 | [cell link](https://example.com) | 54.32 | perf watch |

```python
def run_case_{idx}(document, style, margin):
    result = convert(document, style=style, margin=margin)
    assert result.pdf_bytes > 0
    return result.elapsed_ms
```

Paragraph after a dense block tests transitions between tables, code blocks, lists, and regular wrapping.

"""
    return f"""
# ASCII Sized Section {idx}

This paragraph is plain ASCII text for the standard-font fast path. It includes
**bold text**, *italic spans*, ~~strike markers~~, `inline_code()`,
[a link](https://example.com/product/rayomd), and enough normal words to
exercise wrapping, width calculation, page splitting, and PDF text output.

- Bullet item one with short operational text.
- Bullet item two with measurable conversion throughput claims.
- Bullet item three with deployment and batch processing details.

| Feature | Value | Status |
| --- | ---: | :---: |
| Input size | {idx} | Ready |
| Output | PDF | OK |
| Engine | Native | Fast |

```cpp
auto sample_{idx} = convert_markdown_to_pdf(input, output);
if (!sample_{idx}) return benchmark_failure;
```

$$
x^2 + y^2 = z^2
$$

"""


def make_sized_doc(target_bytes: int, kind: str) -> str:
    parts = ["---", f"title: Perf Watch Sized {kind}", "---", ""]
    idx = 0
    while len("\n".join(parts).encode("utf-8")) < target_bytes:
        parts.append(sized_block(kind, idx))
        idx += 1
    return "\n".join(parts)


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def generate_corpus(root: Path, suite: str, seed: int, image_mode: str) -> dict[str, Any]:
    cfg = SUITES[suite]
    corpus = root / "corpus"
    write_bytes(corpus / "single" / "assets" / "tiny.png", PNG_1X1)
    write_bytes(corpus / "batch" / "assets" / "tiny.png", PNG_1X1)

    rng = random.Random(seed)
    single_paths: list[Path] = []
    for idx in range(cfg["single_docs"]):
        sections = rng.randint(cfg["sections_min"], cfg["sections_max"])
        paragraphs = rng.randint(cfg["paragraphs_min"], cfg["paragraphs_max"])
        path = corpus / "single" / f"single_{idx + 1:02d}.md"
        write_text(path, make_doc(seed + idx * 17, sections, paragraphs, image_mode))
        single_paths.append(path)

    batch_paths: list[Path] = []
    for idx in range(cfg["batch_docs"]):
        sections = rng.randint(cfg["sections_min"], cfg["sections_max"])
        paragraphs = rng.randint(cfg["paragraphs_min"], cfg["paragraphs_max"])
        path = corpus / "batch" / f"doc_{idx + 1:04d}.md"
        write_text(path, make_doc(seed + 1000 + idx, sections, paragraphs, image_mode))
        batch_paths.append(path)

    feature_paths: dict[str, Path] = {}
    for mode in FEATURE_MODES:
        path = corpus / "features" / f"{mode}.md"
        write_text(path, make_feature_doc(cfg["feature_sections"], mode))
        feature_paths[mode] = path

    sized_paths: dict[str, Path] = {}
    for kind in SIZED_KINDS:
        path = corpus / "sized" / f"{kind}_{cfg['sized_doc_bytes'] // KIB}kb.md"
        write_text(path, make_sized_doc(cfg["sized_doc_bytes"], kind))
        sized_paths[kind] = path

    all_paths = single_paths + batch_paths + list(feature_paths.values()) + list(sized_paths.values())
    return {
        "single_paths": single_paths,
        "batch_paths": batch_paths,
        "feature_paths": feature_paths,
        "sized_paths": sized_paths,
        "bytes": sum(path.stat().st_size for path in all_paths),
    }


def parse_bench(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def run_command(
    cmd: list[str],
    stdin_text: str | None = None,
    timeout: int = 7200,
) -> tuple[int, float, str, str]:
    start = time.perf_counter()
    proc = subprocess.run(
        cmd,
        input=stdin_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    wall_ms = (time.perf_counter() - start) * 1000.0
    return proc.returncode, wall_ms, proc.stdout, proc.stderr


def check_pdf(path: Path) -> int:
    if not path.exists():
        raise RuntimeError(f"Missing PDF: {path}")
    data = path.read_bytes()
    if len(data) < 128:
        raise RuntimeError(f"PDF is too small: {path} ({len(data)} bytes)")
    if not data.startswith(b"%PDF-"):
        raise RuntimeError(f"PDF header is invalid: {path}")
    if b"%%EOF" not in data[-2048:]:
        raise RuntimeError(f"PDF EOF marker is missing: {path}")
    return len(data)


def pdf_name_for_markdown(path: Path) -> str:
    return path.with_suffix(".pdf").name


def parse_serve_times(stdout: str) -> list[float]:
    times: list[float] = []
    for line in stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] == "OK":
            try:
                times.append(float(parts[1]))
            except ValueError:
                pass
    return times


def git_value(args: list[str]) -> str:
    try:
        proc = subprocess.run(
            ["git", *args],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        return proc.stdout.strip()
    except Exception:
        return ""


def command_failure(name: str, rc: int, stdout: str, stderr: str) -> RuntimeError:
    return RuntimeError(
        f"{name} failed rc={rc}\nstdout:\n{stdout[-4000:]}\nstderr:\n{stderr[-4000:]}"
    )


def run_bench_case(
    binary: Path,
    input_path: Path,
    out_dir: Path,
    iterations: int,
    style: str,
    margin: str,
    name: str,
) -> dict[str, Any]:
    cmd = [
        str(binary),
        "--bench",
        str(input_path),
        str(out_dir),
        str(iterations),
        style,
        margin,
    ]
    rc, wall_ms, stdout, stderr = run_command(cmd)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "stdout.txt").write_text(stdout, encoding="utf-8", newline="\n")
    (out_dir / "stderr.txt").write_text(stderr, encoding="utf-8", newline="\n")
    if rc != 0:
        raise command_failure(name, rc, stdout, stderr)
    bench = parse_bench(out_dir / "bench-results.txt")
    sample_size = check_pdf(out_dir / "sample.pdf")
    return {
        "bench": bench,
        "wall_ms": wall_ms,
        "sample_pdf_bytes": sample_size,
        "avg_ms": float(bench["avg_ms"]),
    }


def run_watch(
    binary: Path,
    run_root: Path,
    suite: str,
    style: str,
    margin: str,
    seed: int,
    image_mode: str,
) -> dict[str, Any]:
    cfg = SUITES[suite]
    corpus = generate_corpus(run_root, suite, seed, image_mode)
    single_paths = corpus["single_paths"]
    batch_paths = corpus["batch_paths"]
    feature_paths = corpus["feature_paths"]
    sized_paths = corpus["sized_paths"]
    results_root = run_root / "results"

    warm_values: list[float] = []
    pdf_sizes: list[int] = []
    bench_cases: list[dict[str, Any]] = []
    for doc_idx, input_path in enumerate(single_paths):
        for round_idx in range(cfg["bench_rounds"]):
            out_dir = results_root / "bench" / f"doc_{doc_idx + 1:02d}_round_{round_idx + 1:02d}"
            result = run_bench_case(
                binary,
                input_path,
                out_dir,
                cfg["bench_iterations"],
                style,
                margin,
                "bench",
            )
            bench = result["bench"]
            avg_ms = result["avg_ms"]
            warm_values.append(avg_ms)
            pdf_sizes.append(result["sample_pdf_bytes"])
            bench_cases.append(
                {
                    "input": str(input_path),
                    "round": round_idx + 1,
                    "wall_ms": f2(result["wall_ms"]),
                    "avg_ms": f2(avg_ms),
                    "input_bytes": int(bench["input_bytes"]),
                    "avg_pdf_bytes": int(bench["avg_pdf_bytes"]),
                    "path": bench.get("path", ""),
                }
            )

    feature_cases: list[dict[str, Any]] = []
    feature_medians: dict[str, float] = {}
    for mode, input_path in feature_paths.items():
        times: list[float] = []
        for round_idx in range(cfg["bench_rounds"]):
            out_dir = results_root / "features" / f"{mode}_round_{round_idx + 1:02d}"
            result = run_bench_case(
                binary,
                input_path,
                out_dir,
                cfg["feature_iterations"],
                style,
                margin,
                f"feature {mode}",
            )
            times.append(result["avg_ms"])
            pdf_sizes.append(result["sample_pdf_bytes"])
        feature_medians[mode] = statistics.median(times)
        feature_cases.append(
            {
                "case": mode,
                "input": str(input_path),
                "input_bytes": input_path.stat().st_size,
                "warm_ms_median": f2(feature_medians[mode]),
                "warm_ms_p95": f2(percentile(times, 0.95)),
            }
        )

    sized_cases: list[dict[str, Any]] = []
    sized_medians: dict[str, float] = {}
    for kind, input_path in sized_paths.items():
        times = []
        for round_idx in range(cfg["bench_rounds"]):
            out_dir = results_root / "sized" / f"{kind}_round_{round_idx + 1:02d}"
            result = run_bench_case(
                binary,
                input_path,
                out_dir,
                cfg["sized_iterations"],
                style,
                margin,
                f"sized {kind}",
            )
            times.append(result["avg_ms"])
            pdf_sizes.append(result["sample_pdf_bytes"])
        sized_medians[kind] = statistics.median(times)
        sized_cases.append(
            {
                "case": kind,
                "input": str(input_path),
                "input_bytes": input_path.stat().st_size,
                "warm_ms_median": f2(sized_medians[kind]),
                "warm_ms_p95": f2(percentile(times, 0.95)),
            }
        )

    cold_values: list[float] = []
    cold_doc = single_paths[0]
    for idx in range(cfg["cold_runs"]):
        out_dir = results_root / "cold"
        out_dir.mkdir(parents=True, exist_ok=True)
        output_pdf = out_dir / f"cold_{idx + 1:03d}.pdf"
        cmd = [
            str(binary),
            "--export",
            str(cold_doc),
            str(output_pdf),
            "native",
            style,
            margin,
        ]
        rc, wall_ms, stdout, stderr = run_command(cmd)
        if rc != 0:
            raise command_failure("cold export", rc, stdout, stderr)
        check_pdf(output_pdf)
        cold_values.append(wall_ms)

    batch_dir = results_root / "batch_pdf"
    batch_cmd = [
        str(binary),
        "--batch",
        str(run_root / "corpus" / "batch"),
        str(batch_dir),
        "native",
        style,
        margin,
    ]
    rc, batch_ms, stdout, stderr = run_command(batch_cmd)
    (results_root / "batch_stdout.txt").write_text(stdout, encoding="utf-8", newline="\n")
    (results_root / "batch_stderr.txt").write_text(stderr, encoding="utf-8", newline="\n")
    if rc != 0:
        raise command_failure("batch", rc, stdout, stderr)
    for input_path in batch_paths:
        pdf_sizes.append(check_pdf(batch_dir / pdf_name_for_markdown(input_path)))

    stdin_dir = results_root / "stdin_batch_pdf"
    stdin_payload = "\n".join(str(path) for path in batch_paths) + "\n"
    stdin_cmd = [
        str(binary),
        "--stdin-batch",
        str(stdin_dir),
        "native",
        style,
        margin,
    ]
    rc, stdin_ms, stdout, stderr = run_command(stdin_cmd, stdin_payload)
    (results_root / "stdin_batch_stdout.txt").write_text(stdout, encoding="utf-8", newline="\n")
    (results_root / "stdin_batch_stderr.txt").write_text(stderr, encoding="utf-8", newline="\n")
    if rc != 0:
        raise command_failure("stdin-batch", rc, stdout, stderr)
    for input_path in batch_paths:
        check_pdf(stdin_dir / pdf_name_for_markdown(input_path))

    serve_dir = results_root / "serve_pdf"
    serve_payload = "\n".join(str(path) for path in batch_paths) + "\nquit\n"
    serve_cmd = [
        str(binary),
        "--serve",
        str(serve_dir),
        "native",
        style,
        margin,
    ]
    rc, serve_ms, stdout, stderr = run_command(serve_cmd, serve_payload)
    (results_root / "serve_stdout.txt").write_text(stdout, encoding="utf-8", newline="\n")
    (results_root / "serve_stderr.txt").write_text(stderr, encoding="utf-8", newline="\n")
    if rc != 0:
        raise command_failure("serve", rc, stdout, stderr)
    serve_reported = parse_serve_times(stdout)
    if len(serve_reported) != len(batch_paths):
        raise RuntimeError(f"serve reported {len(serve_reported)} files, expected {len(batch_paths)}")
    for input_path in batch_paths:
        check_pdf(serve_dir / pdf_name_for_markdown(input_path))

    file_count = len(batch_paths)
    metrics = {
        "warm_avg_ms_median": f2(statistics.median(warm_values)),
        "warm_avg_ms_p95": f2(percentile(warm_values, 0.95)),
        "cold_export_ms_median": f2(statistics.median(cold_values)),
        "cold_export_ms_p95": f2(percentile(cold_values, 0.95)),
        "batch_ms_total": f2(batch_ms),
        "batch_ms_per_file": f2(batch_ms / file_count),
        "stdin_batch_ms_total": f2(stdin_ms),
        "stdin_batch_ms_per_file": f2(stdin_ms / file_count),
        "serve_wall_ms_total": f2(serve_ms),
        "serve_wall_ms_per_file": f2(serve_ms / file_count),
        "serve_reported_ms_median": f2(statistics.median(serve_reported)),
        "serve_reported_ms_p95": f2(percentile(serve_reported, 0.95)),
        "pdf_bytes_median": int(statistics.median(pdf_sizes)),
    }
    for mode in FEATURE_MODES:
        metrics[f"feature_{mode}_warm_ms_median"] = f2(feature_medians[mode])
        metrics[f"feature_{mode}_warm_ms_p95"] = f2(
            next(case["warm_ms_p95"] for case in feature_cases if case["case"] == mode)
        )
    feature_baseline = feature_medians["baseline"]
    for mode in FEATURE_MODES:
        if mode == "baseline":
            continue
        metrics[f"feature_{mode}_overhead_pct"] = f2(delta_pct(feature_medians[mode], feature_baseline))
    for kind in SIZED_KINDS:
        metrics[f"sized_{kind}_warm_ms_median"] = f2(sized_medians[kind])
        metrics[f"sized_{kind}_warm_ms_p95"] = f2(
            next(case["warm_ms_p95"] for case in sized_cases if case["case"] == kind)
        )

    return {
        "metrics": metrics,
        "bench_cases": bench_cases,
        "feature_cases": feature_cases,
        "sized_cases": sized_cases,
        "counts": {
            "single_docs": len(single_paths),
            "batch_docs": len(batch_paths),
            "feature_docs": len(feature_paths),
            "sized_docs": len(sized_paths),
            "bench_rounds": cfg["bench_rounds"],
            "bench_iterations": cfg["bench_iterations"],
            "feature_iterations": cfg["feature_iterations"],
            "sized_iterations": cfg["sized_iterations"],
            "cold_runs": cfg["cold_runs"],
            "corpus_bytes": corpus["bytes"],
        },
    }


def read_history(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


def write_history(path: Path, record: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")


def matching_previous(records: list[dict[str, Any]], current: dict[str, Any]) -> dict[str, Any] | None:
    for record in reversed(records):
        if not record.get("success"):
            continue
        if all(record.get(field) == current.get(field) for field in COMPARISON_KEY_FIELDS):
            return record
    return None


def delta_pct(value: float, base: float) -> float:
    if base == 0:
        return 0.0
    return ((value - base) / base) * 100.0


def is_time_metric(metric: str) -> bool:
    return "_ms" in metric


def comparable_metric_keys(current: dict[str, Any], previous: dict[str, Any]) -> list[str]:
    common = set(current).intersection(previous)
    keys = [metric for metric in TIME_METRICS if metric in common]
    for metric in sorted(common):
        if metric in keys:
            continue
        if metric.endswith("_pct"):
            continue
        try:
            float(current[metric])
            float(previous[metric])
        except (TypeError, ValueError):
            continue
        keys.append(metric)
    return keys


def build_comparison(current: dict[str, Any], previous: dict[str, Any] | None) -> list[dict[str, Any]]:
    if previous is None:
        return []
    rows: list[dict[str, Any]] = []
    current_metrics = current["metrics"]
    previous_metrics = previous["metrics"]
    for metric in comparable_metric_keys(current_metrics, previous_metrics):
        cur = float(current_metrics[metric])
        old = float(previous_metrics[metric])
        rows.append(
            {
                "metric": metric,
                "previous": f2(old),
                "current": f2(cur),
                "delta_pct": f2(delta_pct(cur, old)),
            }
        )
    return rows


def read_json_record(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RuntimeError(f"could not read benchmark record {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON benchmark record {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise RuntimeError(f"benchmark record must be a JSON object: {path}")
    if not isinstance(data.get("metrics"), dict):
        raise RuntimeError(f"benchmark record is missing a metrics object: {path}")
    return data


def comparison_key_mismatches(current: dict[str, Any], baseline: dict[str, Any]) -> list[str]:
    mismatches: list[str] = []
    for field in COMPARISON_KEY_FIELDS:
        current_value = current.get(field)
        baseline_value = baseline.get(field)
        if baseline_value != current_value:
            mismatches.append(f"{field}: baseline={baseline_value!r}, current={current_value!r}")
    return mismatches


def compact_version_record(record: dict[str, Any], storage_note: str) -> dict[str, Any]:
    benchmark_version = record.get("benchmark_version") or record.get("binary_version") or record.get("project_version") or "unknown"
    return {
        "schema": "rayomd-version-benchmark-v1",
        "project_version": benchmark_version,
        "binary_version": record.get("binary_version") or "",
        "benchmark_version": benchmark_version,
        "created_at": record["created_at"],
        "run_id": record["run_id"],
        "platform": record["platform"],
        "suite": record["suite"],
        "seed": record["seed"],
        "style": record["style"],
        "margin": record["margin"],
        "image_mode": record["image_mode"],
        "watch_version": record["watch_version"],
        "binary_bytes": record["binary_bytes"],
        "git_commit": record.get("git_commit") or "",
        "git_dirty": record.get("git_dirty", False),
        "system": record.get("system") or "",
        "storage_note": storage_note,
        "counts": record.get("counts", {}),
        "metrics": record.get("metrics", {}),
    }


def version_record_filename(record: dict[str, Any]) -> str:
    parts = [
        safe_name(str(record.get("platform") or "platform")),
        safe_name(str(record.get("suite") or "suite")),
        safe_name(str(record.get("style") or "style")),
        safe_name(str(record.get("margin") or "margin")),
        safe_name(str(record.get("image_mode") or "images")),
    ]
    return "-".join(parts) + ".json"


def write_version_benchmark(log_dir: Path, record: dict[str, Any], storage_note: str) -> Path:
    compact = compact_version_record(record, storage_note)
    version = safe_version_name(str(compact["project_version"]))
    version_dir = log_dir / f"v{version}"
    version_dir.mkdir(parents=True, exist_ok=True)
    out_path = version_dir / version_record_filename(compact)
    out_path.write_text(
        json.dumps(compact, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    write_version_index(log_dir)
    return out_path


def version_sort_key(version: str) -> tuple[int, ...]:
    parts: list[int] = []
    for part in version.lstrip("vV").split("."):
        try:
            parts.append(int(part))
        except ValueError:
            parts.append(-1)
    return tuple(parts)


def load_version_benchmarks(log_dir: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    if not log_dir.exists():
        return records
    for path in sorted(log_dir.glob("v*/*.json")):
        try:
            record = read_json_record(path)
        except RuntimeError:
            continue
        try:
            record["_relative_path"] = path.relative_to(log_dir).as_posix()
        except ValueError:
            record["_relative_path"] = path.as_posix()
        records.append(record)
    records.sort(
        key=lambda record: (
            version_sort_key(str(record.get("project_version") or "0")),
            str(record.get("created_at") or ""),
            str(record.get("platform") or ""),
            str(record.get("suite") or ""),
        ),
        reverse=True,
    )
    return records


def metric_cell(record: dict[str, Any], metric: str) -> str:
    value = record.get("metrics", {}).get(metric, "")
    return f"`{value}`" if value != "" else ""


def table_cell(value: Any) -> str:
    return str(value or "").replace("|", "/")


def write_version_index(log_dir: Path) -> None:
    records = load_version_benchmarks(log_dir)
    lines = [
        "# RayoMD Version Benchmarks",
        "",
        "Compact release benchmark records generated by `scripts/perf_watch.py`.",
        "Raw run artifacts stay under ignored `benchmark-output/`; these tracked",
        "records keep only comparison keys, environment notes, and headline metrics",
        "needed for release-to-release review.",
        "",
        "Generate or refresh a version record with:",
        "",
        "```sh",
        "python scripts/perf_watch.py --binary build/linux/rayomd --platform linux-wsl --suite watch --label release --version-log-dir docs/benchmarks/versions --storage-note \"WSL ext4\"",
        "```",
        "",
        "Archive published Linux release binaries from v1.1.0 onward with:",
        "",
        "```sh",
        "python scripts/archive_release_benchmarks.py --from-version 1.1.0 --suite quick",
        "```",
        "",
        "If `gh` is only available outside WSL, download and extract assets first, then",
        "run the helper with `--skip-download --binary-root <extracted-root>`.",
        "",
        "## Highlights",
        "",
    ]
    if not records:
        lines.append("No version benchmark records have been committed yet.")
    else:
        lines.extend(
            [
                "| Version | Platform | Suite | Storage | Warm ms | Cold ms | Batch ms/file | Stdin ms/file | Serve ms |",
                "|---|---|---|---|---:|---:|---:|---:|---:|",
            ]
        )
        for record in records:
            version = table_cell(record.get("project_version") or "unknown")
            rel_path = table_cell(record.get("_relative_path") or "")
            version_link = f"[`v{version}`]({rel_path})" if rel_path else f"`v{version}`"
            lines.append(
                "| "
                + " | ".join(
                    [
                        version_link,
                        table_cell(record.get("platform")),
                        table_cell(record.get("suite")),
                        table_cell(record.get("storage_note")),
                        metric_cell(record, "warm_avg_ms_median"),
                        metric_cell(record, "cold_export_ms_median"),
                        metric_cell(record, "batch_ms_per_file"),
                        metric_cell(record, "stdin_batch_ms_per_file"),
                        metric_cell(record, "serve_reported_ms_median"),
                    ]
                )
                + " |"
            )
    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def append_comparison_table(lines: list[str], comparison: list[dict[str, Any]]) -> None:
    lines.extend(["| Metric | Previous | Current | Delta |", "|---|---:|---:|---:|"])
    for row in comparison:
        lines.append(f"| {row['metric']} | {row['previous']} | {row['current']} | {row['delta_pct']}% |")


def write_summary(
    path: Path,
    record: dict[str, Any],
    previous: dict[str, Any] | None,
    comparison: list[dict[str, Any]],
    baseline: dict[str, Any] | None,
    baseline_comparison: list[dict[str, Any]],
    baseline_path: Path | None,
) -> None:
    lines = [
        "# RayoMD Perf Watch",
        "",
        f"- run: `{record['run_id']}`",
        f"- platform: `{record['platform']}`",
        f"- suite: `{record['suite']}`",
        f"- binary: `{record['binary']}`",
        f"- project version: `{record.get('project_version') or 'unknown'}`",
        f"- binary version: `{record.get('binary_version') or 'unknown'}`",
        f"- benchmark version: `{record.get('benchmark_version') or 'unknown'}`",
        f"- git: `{record.get('git_commit') or 'unknown'}`",
        f"- generated corpus bytes: {record['counts']['corpus_bytes']}",
    ]
    if record.get("storage_note"):
        lines.append(f"- storage: `{record['storage_note']}`")
    if record.get("version_benchmark_path"):
        lines.append(f"- version benchmark: `{record['version_benchmark_path']}`")
    lines.extend(["", "## Current", "", "| Metric | Value |", "|---|---:|"])
    for key, value in record["metrics"].items():
        lines.append(f"| {key} | {value} |")

    lines.extend(["", "## Previous Local History Match", ""])
    lines.append("Local history comparison is report-only and is not used for pass/fail gating.")
    lines.append("")
    if previous is None:
        lines.append("No previous matching run in the history file.")
    else:
        lines.extend(
            [
                f"- run: `{previous['run_id']}`",
                f"- created: `{previous['created_at']}`",
                f"- git: `{previous.get('git_commit') or 'unknown'}`",
                "",
            ]
        )
        append_comparison_table(lines, comparison)

    lines.extend(["", "## Explicit Baseline Gate", ""])
    if baseline is None:
        lines.append("No explicit baseline record was provided.")
    else:
        lines.extend(
            [
                f"- record: `{baseline_path}`",
                f"- run: `{baseline.get('run_id') or 'unknown'}`",
                f"- created: `{baseline.get('created_at') or 'unknown'}`",
                f"- git: `{baseline.get('git_commit') or 'unknown'}`",
                "",
            ]
        )
        if record.get("baseline_mismatches"):
            lines.append("Baseline keys do not match the current run:")
            for mismatch in record["baseline_mismatches"]:
                lines.append(f"- `{mismatch}`")
        elif baseline_comparison:
            append_comparison_table(lines, baseline_comparison)
        else:
            lines.append("No comparable metrics were found in the explicit baseline record.")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def print_console_summary(
    record: dict[str, Any],
    previous: dict[str, Any] | None,
    comparison: list[dict[str, Any]],
    baseline: dict[str, Any] | None,
    baseline_comparison: list[dict[str, Any]],
) -> None:
    print(f"Perf watch run: {record['run_id']}")
    print(f"Platform: {record['platform']}  Suite: {record['suite']}")
    print(f"Summary: {record['summary_path']}")
    if record.get("version_benchmark_path"):
        print(f"Version benchmark: {record['version_benchmark_path']}")
    print("")
    for metric in (
        "warm_avg_ms_median",
        "cold_export_ms_median",
        "batch_ms_per_file",
        "stdin_batch_ms_per_file",
        "serve_reported_ms_median",
    ):
        print(f"{metric}: {record['metrics'][metric]}")

    print("")
    if previous is None:
        print("No previous matching run found. Local history is report-only.")
    else:
        print(f"Report-only history comparison: {previous['run_id']} ({previous['created_at']})")
        for row in comparison:
            if row["metric"] not in TIME_METRICS:
                continue
            sign = "+" if row["delta_pct"] >= 0 else ""
            print(f"{row['metric']}: {sign}{row['delta_pct']}%")

    if baseline is None:
        return

    print("")
    print(f"Explicit baseline gate: {baseline.get('run_id') or 'unknown'} ({baseline.get('created_at') or 'unknown'})")
    for row in baseline_comparison:
        if row["metric"] not in TIME_METRICS:
            continue
        sign = "+" if row["delta_pct"] >= 0 else ""
        print(f"{row['metric']}: {sign}{row['delta_pct']}%")


def detect_platform() -> str:
    system = platform_module.system().lower()
    if system == "windows":
        return "windows"
    if "microsoft" in platform_module.release().lower():
        return "linux-wsl"
    return system or "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="rayomd CLI-capable binary")
    parser.add_argument("--platform", default=detect_platform(), help="history key, for example windows or linux-wsl")
    parser.add_argument("--suite", choices=sorted(SUITES), default="watch")
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--style", choices=STYLES, default="modern")
    parser.add_argument("--margin", choices=MARGINS, default="normal")
    parser.add_argument("--image-mode", choices=("off", "local"), default="local")
    parser.add_argument("--label", default="", help="optional run label")
    parser.add_argument("--root", default=Path("benchmark-output/perf-watch"), type=Path)
    parser.add_argument("--history", default=None, type=Path, help="JSONL history path for report-only local comparisons")
    parser.add_argument("--baseline-record", default=None, type=Path, help="explicit record.json or version benchmark JSON for pass/fail comparisons")
    parser.add_argument("--fail-on-slower-pct", default=None, type=float, help="exit nonzero if explicit-baseline time metrics regress by this percent")
    parser.add_argument("--version-log-dir", default=None, type=Path, help="write a compact version benchmark record and refresh README.md in this directory")
    parser.add_argument("--benchmark-version", default="", help="version label to use for archived benchmark records, for example 1.1.0")
    parser.add_argument("--storage-note", default="", help="short storage/environment note for version benchmark records, for example WSL ext4")
    args = parser.parse_args()

    if args.fail_on_slower_pct is not None and args.baseline_record is None:
        print("--fail-on-slower-pct requires --baseline-record; local history is report-only.", file=sys.stderr)
        return 2

    baseline_record: dict[str, Any] | None = None
    if args.baseline_record is not None:
        try:
            baseline_record = read_json_record(args.baseline_record)
        except RuntimeError as exc:
            print(str(exc), file=sys.stderr)
            return 2
        requested = {
            "platform": args.platform,
            "suite": args.suite,
            "seed": args.seed,
            "style": args.style,
            "margin": args.margin,
            "image_mode": args.image_mode,
            "watch_version": WATCH_VERSION,
        }
        requested_mismatches = comparison_key_mismatches(requested, baseline_record)
        if requested_mismatches:
            print("Explicit baseline record is not comparable to the requested run:", file=sys.stderr)
            for mismatch in requested_mismatches:
                print(f"  {mismatch}", file=sys.stderr)
            return 2

    binary = args.binary.resolve()
    if not binary.exists():
        print(f"Binary not found: {binary}", file=sys.stderr)
        return 2

    root = args.root
    history_path = args.history or (root / "history.jsonl")
    label = safe_name(args.label) if args.label else "run"
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_id = f"{timestamp}-{safe_name(args.platform)}-{args.suite}-{label}"
    run_root = root / safe_name(args.platform) / run_id
    run_root.mkdir(parents=True, exist_ok=True)

    previous_records = read_history(history_path)
    binary_version = read_binary_version(binary)
    project_version = read_project_version()
    benchmark_version = args.benchmark_version or binary_version or project_version
    record: dict[str, Any] = {
        "run_id": run_id,
        "created_at": now_utc(),
        "platform": args.platform,
        "suite": args.suite,
        "seed": args.seed,
        "style": args.style,
        "margin": args.margin,
        "image_mode": args.image_mode,
        "watch_version": WATCH_VERSION,
        "project_version": project_version,
        "binary_version": binary_version,
        "benchmark_version": benchmark_version,
        "binary": str(binary),
        "binary_bytes": binary.stat().st_size,
        "python": sys.version.split()[0],
        "system": platform_module.platform(),
        "storage_note": args.storage_note,
        "cwd": str(Path.cwd()),
        "run_root": str(run_root),
        "git_commit": git_value(["rev-parse", "--short", "HEAD"]),
        "git_dirty": bool(git_value(["status", "--short"])),
        "success": False,
    }

    try:
        result = run_watch(binary, run_root, args.suite, args.style, args.margin, args.seed, args.image_mode)
        record.update(result)
        record["success"] = True
    except Exception as exc:
        record["error"] = str(exc)
        write_history(history_path, record)
        print(str(exc), file=sys.stderr)
        return 1

    previous = matching_previous(previous_records, record)
    comparison = build_comparison(record, previous)
    baseline_comparison: list[dict[str, Any]] = []
    baseline_mismatches: list[str] = []
    if baseline_record is not None:
        baseline_mismatches = comparison_key_mismatches(record, baseline_record)
        baseline_comparison = build_comparison(record, baseline_record)
        record["baseline_record"] = str(args.baseline_record)
        record["baseline_comparison"] = baseline_comparison
        if baseline_mismatches:
            record["baseline_mismatches"] = baseline_mismatches

    if args.version_log_dir is not None:
        version_path = write_version_benchmark(args.version_log_dir, record, args.storage_note)
        record["version_benchmark_path"] = str(version_path)

    summary_path = run_root / "summary.md"
    record["summary_path"] = str(summary_path)
    record["comparison"] = comparison
    write_summary(summary_path, record, previous, comparison, baseline_record, baseline_comparison, args.baseline_record)
    write_history(history_path, record)
    (run_root / "record.json").write_text(
        json.dumps(record, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    shutil.copyfile(summary_path, root / f"latest-{safe_name(args.platform)}-{args.suite}.md")

    print_console_summary(record, previous, comparison, baseline_record, baseline_comparison)

    if baseline_mismatches:
        print("Explicit baseline record is not comparable to the current run:", file=sys.stderr)
        for mismatch in baseline_mismatches:
            print(f"  {mismatch}", file=sys.stderr)
        return 2

    if args.fail_on_slower_pct is not None:
        if not baseline_comparison:
            print("Explicit baseline record has no comparable metrics.", file=sys.stderr)
            return 2
        threshold = args.fail_on_slower_pct
        slower = [row for row in baseline_comparison if is_time_metric(row["metric"]) and row["delta_pct"] >= threshold]
        if slower:
            print(f"Performance regression threshold hit against explicit baseline: {threshold}%", file=sys.stderr)
            for row in slower:
                print(f"{row['metric']}: +{row['delta_pct']}%", file=sys.stderr)
            return 3

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
