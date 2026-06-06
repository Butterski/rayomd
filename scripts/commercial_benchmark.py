#!/usr/bin/env python3
import argparse
import ctypes
import json
import math
import os
import platform as platform_mod
import shutil
import subprocess
import sys
import time
from pathlib import Path


MIB = 1024 * 1024
KIB = 1024
DEFAULT_ROOT = Path("benchmark-output") / "commercial"
STYLES = ["elegant", "modern", "tech"]
MARGINS = ["compact", "normal", "wide", "margin=0.75in"]


DOC_SPECS = {
    "ascii_100kb": (100 * KIB, "ascii"),
    "ascii_250kb": (250 * KIB, "ascii"),
    "ascii_500kb": (500 * KIB, "ascii"),
    "ascii_1mb": (1 * MIB, "ascii"),
    "ascii_2_5mb": (int(2.5 * MIB), "ascii"),
    "ascii_5mb": (5 * MIB, "ascii"),
    "ascii_10mb": (10 * MIB, "ascii"),
    "ascii_25mb": (25 * MIB, "ascii"),
    "ascii_50mb": (50 * MIB, "ascii"),
    "ascii_100mb": (100 * MIB, "ascii"),
    "unicode_100kb": (100 * KIB, "unicode"),
    "unicode_250kb": (250 * KIB, "unicode"),
    "unicode_500kb": (500 * KIB, "unicode"),
    "unicode_1mb": (1 * MIB, "unicode"),
    "unicode_2_5mb": (int(2.5 * MIB), "unicode"),
    "unicode_5mb": (5 * MIB, "unicode"),
    "table_100kb": (100 * KIB, "table"),
    "table_500kb": (500 * KIB, "table"),
    "table_1mb": (1 * MIB, "table"),
    "table_5mb": (5 * MIB, "table"),
}


BATCH_SPECS = {
    "batch_tiny100": (100, 20 * KIB, ["ascii", "unicode", "table"]),
    "batch_small20": (20, 100 * KIB, ["ascii", "unicode", "table"]),
    "batch_mixed12": (12, 512 * KIB, ["ascii", "unicode", "table"]),
    "batch_large5": (5, 2 * MIB, ["ascii", "table"]),
}


ASCII_BLOCK = """
# Benchmark Section {i}

This paragraph is plain ASCII text for the standard-font fast path. It includes **bold text**, *italic spans*, ~~strike markers~~, `inline_code()`, [a link](https://example.com/product/fast-markdown), and enough normal words to exercise wrapping, width calculation, page splitting, and PDF text output.

- Bullet item one with short operational text.
- Bullet item two with measurable conversion throughput claims.
- Bullet item three with deployment and batch processing details.

| Feature | Value | Status |
| --- | ---: | :---: |
| Input size | {i} | Ready |
| Output | PDF | OK |
| Engine | Native | Fast |

```cpp
auto sample_{i} = convert_markdown_to_pdf(input, output);
if (!sample_{i}) return benchmark_failure;
```

$$
x^2 + y^2 = z^2
$$

"""


UNICODE_BLOCK = """
# Unicode Benchmark Section {i}

Zażółć gęślą jaźń, résumé, naïve facade, Αθήνα, 東京, Москва, and status symbols like ✅ ⚠️ ❌ appear next to **bold Unicode**, *italic Unicode*, ~~struck Unicode~~, `kod_inline()`, and [international links](https://example.com/produkt/ścieżka).

> A quoted paragraph keeps diacritics and multilingual words together while testing wrapping and embedded-font output.

1. Polish and European text with diacritics.
2. Greek, Cyrillic, and CJK text.
3. Mixed punctuation, math cleanup, and status fallback symbols.

| Locale | Text | Marker |
| :--- | :--- | ---: |
| PL | Zażółć gęślą jaźń | {i} |
| JP | 東京の文書変換 | {i} |
| GR | Δοκιμή εγγράφου | {i} |

"""


TABLE_BLOCK = """
# Dense Tables And Code {i}

| Column A | Column B | Column C | Column D | Column E |
| :--- | ---: | :---: | ---: | :--- |
| alpha {i} | 12345 | centered value | 67.89 | markdown renderer |
| beta {i} | 67890 | styled **cell** | 12.34 | batch conversion |
| gamma {i} | 24680 | `inline_table_code()` | 98.76 | predictable output |
| delta {i} | 13579 | [cell link](https://example.com) | 54.32 | commercial benchmark |

```python
def run_case_{i}(document, style, margin):
    result = convert(document, style=style, margin=margin)
    assert result.pdf_bytes > 0
    return result.elapsed_ms
```

Paragraph after a dense block tests transitions between tables, code blocks, lists, and regular wrapping.

"""


def block_for(kind, i):
    if kind == "unicode":
        return UNICODE_BLOCK.format(i=i)
    if kind == "table":
        return TABLE_BLOCK.format(i=i)
    return ASCII_BLOCK.format(i=i)


def write_sized_doc(path, target_bytes, kind):
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    i = 0
    with path.open("wb") as f:
        header = f"---\ntitle: Commercial Benchmark {path.stem}\n---\n\n".encode("utf-8")
        f.write(header)
        written += len(header)
        while written < target_bytes:
            data = block_for(kind, i).encode("utf-8")
            f.write(data)
            written += len(data)
            i += 1


def generate_corpus(root):
    corpus = root / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    docs_dir = corpus / "docs"
    docs_dir.mkdir(parents=True, exist_ok=True)

    for name, (size, kind) in DOC_SPECS.items():
        path = docs_dir / f"{name}.md"
        if not path.exists() or path.stat().st_size < size:
            write_sized_doc(path, size, kind)

    for batch_name, (count, size, kinds) in BATCH_SPECS.items():
        batch_dir = corpus / "batches" / batch_name
        batch_dir.mkdir(parents=True, exist_ok=True)
        existing = list(batch_dir.glob("*.md"))
        if len(existing) >= count:
            continue
        for i in range(count):
            kind = kinds[i % len(kinds)]
            write_sized_doc(batch_dir / f"{batch_name}_{i:04d}_{kind}.md", size, kind)


def bench_iterations(input_bytes):
    if input_bytes <= 128 * KIB:
        return 200
    if input_bytes <= 512 * KIB:
        return 80
    if input_bytes <= 1 * MIB:
        return 40
    if input_bytes <= 5 * MIB:
        return 10
    if input_bytes <= 10 * MIB:
        return 5
    if input_bytes <= 25 * MIB:
        return 3
    return 1


def doc_path(root, name):
    return root / "corpus" / "docs" / f"{name}.md"


def batch_path(root, name):
    return root / "corpus" / "batches" / name


def linux_plan(root):
    tests = []

    ascii_docs = [
        "ascii_100kb", "ascii_250kb", "ascii_500kb", "ascii_1mb", "ascii_2_5mb",
        "ascii_5mb", "ascii_10mb", "ascii_25mb", "ascii_50mb", "ascii_100mb",
    ]
    for idx, doc in enumerate(ascii_docs):
        tests.append({"mode": "bench", "doc": doc, "style": STYLES[idx % 3], "margin": MARGINS[idx % 4]})

    unicode_docs = ["unicode_100kb", "unicode_250kb", "unicode_500kb", "unicode_1mb", "unicode_2_5mb", "unicode_5mb"]
    for idx, doc in enumerate(unicode_docs):
        tests.append({"mode": "bench", "doc": doc, "style": "elegant", "margin": MARGINS[idx % 4]})
        tests.append({"mode": "bench", "doc": doc, "style": "tech", "margin": MARGINS[(idx + 1) % 4]})

    table_docs = ["table_100kb", "table_500kb", "table_1mb"]
    for idx, doc in enumerate(table_docs):
        tests.append({"mode": "bench", "doc": doc, "style": "modern", "margin": MARGINS[idx % 4]})
        tests.append({"mode": "bench", "doc": doc, "style": "tech", "margin": MARGINS[(idx + 2) % 4]})

    export_docs = ["ascii_100kb", "unicode_100kb", "table_500kb", "ascii_5mb", "unicode_5mb", "ascii_25mb", "ascii_50mb", "ascii_100mb"]
    for idx, doc in enumerate(export_docs):
        tests.append({"mode": "export", "doc": doc, "style": STYLES[idx % 3], "margin": MARGINS[idx % 4]})

    batch_names = ["batch_tiny100", "batch_small20", "batch_mixed12", "batch_large5", "batch_small20"]
    for idx, batch in enumerate(batch_names):
        tests.append({"mode": "batch", "batch": batch, "style": STYLES[idx % 3], "margin": MARGINS[idx % 4]})

    stdin_batches = ["batch_tiny100", "batch_small20", "batch_mixed12", "batch_large5"]
    for idx, batch in enumerate(stdin_batches):
        tests.append({"mode": "stdin-batch", "batch": batch, "style": STYLES[(idx + 1) % 3], "margin": MARGINS[(idx + 2) % 4]})

    serve_batches = ["batch_tiny100", "batch_small20", "batch_large5"]
    for idx, batch in enumerate(serve_batches):
        tests.append({"mode": "serve", "batch": batch, "style": STYLES[(idx + 2) % 3], "margin": MARGINS[(idx + 1) % 4]})

    assert len(tests) == 48, len(tests)
    return assign_ids("linux", tests)


def windows_plan(root):
    tests = [
        {"mode": "bench", "doc": "ascii_100kb", "style": "elegant", "margin": "compact"},
        {"mode": "bench", "doc": "ascii_1mb", "style": "modern", "margin": "normal"},
        {"mode": "bench", "doc": "ascii_10mb", "style": "tech", "margin": "wide"},
        {"mode": "bench", "doc": "unicode_100kb", "style": "elegant", "margin": "normal"},
        {"mode": "bench", "doc": "unicode_1mb", "style": "tech", "margin": "compact"},
        {"mode": "bench", "doc": "table_500kb", "style": "modern", "margin": "margin=0.75in"},
        {"mode": "bench", "doc": "ascii_25mb", "style": "elegant", "margin": "normal"},
        {"mode": "export", "doc": "ascii_100kb", "style": "modern", "margin": "normal"},
        {"mode": "export", "doc": "unicode_500kb", "style": "elegant", "margin": "compact"},
        {"mode": "export", "doc": "ascii_100mb", "style": "tech", "margin": "wide"},
        {"mode": "batch", "batch": "batch_small20", "style": "modern", "margin": "normal"},
        {"mode": "serve", "batch": "batch_large5", "style": "tech", "margin": "compact"},
    ]
    assert len(tests) == 12, len(tests)
    return assign_ids("windows", tests)


def assign_ids(platform_name, tests):
    for idx, test in enumerate(tests, 1):
        target = test.get("doc") or test.get("batch")
        test["id"] = f"{platform_name}_{idx:02d}_{test['mode'].replace('-', '_')}_{target}"
    return tests


def parse_bench_results(path):
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def parse_time_v(stderr):
    values = {}
    for raw in stderr.splitlines():
        line = raw.strip()
        if line.startswith("User time"):
            values["user_s"] = line.split(":", 1)[1].strip()
        elif line.startswith("System time"):
            values["sys_s"] = line.split(":", 1)[1].strip()
        elif line.startswith("Elapsed (wall clock) time"):
            values["elapsed_time"] = line.rsplit("):", 1)[-1].strip()
        elif line.startswith("Maximum resident set size"):
            values["max_rss_kb"] = line.split(":", 1)[1].strip()
    return values


class ProcessMemoryCountersEx(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_ulong),
        ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


def windows_peak_rss_kb(process_handle):
    counters = ProcessMemoryCountersEx()
    counters.cb = ctypes.sizeof(ProcessMemoryCountersEx)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(
        ctypes.c_void_p(process_handle), ctypes.byref(counters), counters.cb
    )
    if not ok:
        return None
    return int(counters.PeakWorkingSetSize // 1024)


def run_command(cmd, cwd, stdin_text=None, platform_name="linux", timeout=7200):
    start = time.perf_counter()
    peak_kb = None
    if platform_name == "linux":
        time_bin = "/usr/bin/time" if Path("/usr/bin/time").exists() else None
        run_cmd = [time_bin, "-v"] + cmd if time_bin else cmd
        proc = subprocess.run(
            run_cmd,
            cwd=cwd,
            input=stdin_text,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        wall_s = time.perf_counter() - start
        meta = parse_time_v(proc.stderr)
        return proc.returncode, wall_s, proc.stdout, proc.stderr, meta

    creationflags = 0
    if sys.platform.startswith("win"):
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdin=subprocess.PIPE if stdin_text is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        creationflags=creationflags,
    )
    if stdin_text is not None and proc.stdin is not None:
        proc.stdin.write(stdin_text)
        proc.stdin.close()
        proc.stdin = None
    while proc.poll() is None:
        if sys.platform.startswith("win"):
            sample = windows_peak_rss_kb(proc._handle)
            if sample is not None:
                peak_kb = max(peak_kb or 0, sample)
        if time.perf_counter() - start > timeout:
            proc.kill()
            raise subprocess.TimeoutExpired(cmd, timeout)
        time.sleep(0.05)
    stdout, stderr = proc.communicate()
    wall_s = time.perf_counter() - start
    meta = {}
    if peak_kb is not None:
        meta["max_rss_kb"] = str(peak_kb)
    return proc.returncode, wall_s, stdout or "", stderr or "", meta


def path_list_for_batch(input_dir):
    return "\n".join(str(p) for p in sorted(input_dir.glob("*.md"))) + "\n"


def sum_input_bytes_for_test(root, test):
    if "doc" in test:
        return doc_path(root, test["doc"]).stat().st_size
    return sum(p.stat().st_size for p in batch_path(root, test["batch"]).glob("*.md"))


def count_output_pdfs(path):
    if not path.exists():
        return 0
    return len(list(path.glob("*.pdf")))


def sum_output_bytes(path):
    if not path.exists():
        return 0
    return sum(p.stat().st_size for p in path.glob("*.pdf"))


def run_test(root, bin_path, test, platform_name, label):
    out_root = root / "results" / label / test["id"]
    if out_root.exists():
        shutil.rmtree(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    style = test["style"]
    margin = test["margin"]
    mode = test["mode"]
    input_bytes = sum_input_bytes_for_test(root, test)
    cmd = [str(bin_path)]
    stdin_text = None

    if mode == "bench":
        input_path = doc_path(root, test["doc"])
        iterations = bench_iterations(input_bytes)
        cmd += ["--bench", str(input_path), str(out_root), str(iterations), style, margin]
        output_dir = out_root
    elif mode == "export":
        input_path = doc_path(root, test["doc"])
        output_dir = out_root
        output_pdf = out_root / f"{test['doc']}.pdf"
        cmd += ["--export", str(input_path), str(output_pdf), "native", style, margin]
        iterations = 1
    elif mode == "batch":
        input_dir = batch_path(root, test["batch"])
        output_dir = out_root / "pdfs"
        cmd += ["--batch", str(input_dir), str(output_dir), "native", style, margin]
        iterations = 1
    elif mode == "stdin-batch":
        input_dir = batch_path(root, test["batch"])
        output_dir = out_root / "pdfs"
        stdin_text = path_list_for_batch(input_dir)
        cmd += ["--stdin-batch", str(output_dir), "native", style, margin]
        iterations = 1
    elif mode == "serve":
        input_dir = batch_path(root, test["batch"])
        output_dir = out_root / "pdfs"
        stdin_text = path_list_for_batch(input_dir) + "quit\n"
        cmd += ["--serve", str(output_dir), "native", style, margin]
        iterations = 1
    else:
        raise ValueError(mode)

    try:
        rc, wall_s, stdout, stderr, meta = run_command(cmd, Path.cwd(), stdin_text, platform_name)
        status = "ok" if rc == 0 else "failed"
    except subprocess.TimeoutExpired as exc:
        rc, wall_s, stdout, stderr, meta = 124, float("nan"), "", str(exc), {}
        status = "timeout"

    output_bytes = sum_output_bytes(output_dir)
    pdf_count = count_output_pdfs(output_dir)
    bench = parse_bench_results(out_root / "bench-results.txt") if mode == "bench" else {}
    if mode == "export" and output_pdf.exists():
        output_bytes = output_pdf.stat().st_size
        pdf_count = 1

    avg_ms = float(bench["avg_ms"]) if bench.get("avg_ms") else None
    if avg_ms and avg_ms > 0:
        throughput = (input_bytes / MIB) / (avg_ms / 1000.0)
    elif wall_s and wall_s > 0 and math.isfinite(wall_s):
        throughput = (input_bytes / MIB) / wall_s
    else:
        throughput = None

    result = {
        "id": test["id"],
        "platform": platform_name,
        "mode": mode,
        "target": test.get("doc") or test.get("batch"),
        "style": style,
        "margin": margin,
        "status": status,
        "return_code": rc,
        "input_bytes": input_bytes,
        "input_mib": round(input_bytes / MIB, 4),
        "output_bytes": output_bytes,
        "output_mib": round(output_bytes / MIB, 4),
        "pdf_count": pdf_count,
        "wall_s": round(wall_s, 6) if math.isfinite(wall_s) else None,
        "throughput_mib_s": round(throughput, 4) if throughput is not None else None,
        "avg_ms": avg_ms,
        "iterations": int(bench.get("iterations", iterations)),
        "path": bench.get("path"),
        "max_rss_kb": int(meta["max_rss_kb"]) if meta.get("max_rss_kb", "").isdigit() else None,
        "user_s": meta.get("user_s"),
        "sys_s": meta.get("sys_s"),
        "command": cmd,
    }
    if stdout.strip():
        (out_root / "stdout.txt").write_text(stdout, encoding="utf-8", errors="replace")
    if stderr.strip():
        (out_root / "stderr.txt").write_text(stderr, encoding="utf-8", errors="replace")
    (out_root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def summarize_results(results):
    ok = [r for r in results if r["status"] == "ok"]
    by_platform = {}
    for r in ok:
        by_platform.setdefault(r["platform"], []).append(r)

    fastest = sorted([r for r in ok if r.get("throughput_mib_s")], key=lambda r: r["throughput_mib_s"], reverse=True)[:10]
    largest = sorted(ok, key=lambda r: r["input_bytes"], reverse=True)[:10]
    batch = [r for r in ok if r["mode"] in ("batch", "stdin-batch", "serve")]
    bench = [r for r in ok if r["mode"] == "bench"]

    return {
        "total": len(results),
        "ok": len(ok),
        "failed": len(results) - len(ok),
        "by_platform": {k: len(v) for k, v in by_platform.items()},
        "fastest": fastest,
        "largest": largest,
        "batch": batch,
        "bench": bench,
    }


def markdown_table(rows, columns):
    out = ["| " + " | ".join(title for title, _ in columns) + " |"]
    out.append("| " + " | ".join("---" for _ in columns) + " |")
    for row in rows:
        out.append("| " + " | ".join(str(getter(row)) for _, getter in columns) + " |")
    return "\n".join(out)


def write_report(root, label, platform_name, results):
    reports = root / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    summary = summarize_results(results)
    path = reports / f"commercial_benchmark_{platform_name}_{label}.md"

    lines = [
        f"# Commercial Benchmark Report - {platform_name} - {label}",
        "",
        f"Host platform: `{platform_mod.platform()}`",
        f"Tests run: `{summary['total']}`; passed: `{summary['ok']}`; failed/timeouts: `{summary['failed']}`.",
        "",
        "Benchmark scope:",
        "",
        "- Native Markdown-to-PDF path.",
        "- CLI modes: `--bench`, `--export`, `--batch`, `--stdin-batch`, `--serve`.",
        "- Styles: `elegant`, `modern`, `tech`.",
        "- Margins: `compact`, `normal`, `wide`, `margin=0.75in`.",
        "- Generated files range from about 100 KB to 100 MB.",
        "",
        "Important claim caveat:",
        "",
        "- `--bench` measures warm in-process PDF byte generation and is best for conversion-engine speed.",
        "- `--export`, `--batch`, `--stdin-batch`, and `--serve` include file reads and PDF writes.",
        "- Linux results should specify native WSL ext4 when produced by `commercial_linux_ext4.sh`; `/mnt/e` is much slower for batch I/O.",
        "",
        "## Fastest Throughput Cases",
        "",
        markdown_table(summary["fastest"], [
            ("ID", lambda r: r["id"]),
            ("Mode", lambda r: r["mode"]),
            ("Input MiB", lambda r: r["input_mib"]),
            ("Throughput MiB/s", lambda r: r["throughput_mib_s"]),
            ("Avg ms", lambda r: r["avg_ms"] if r["avg_ms"] is not None else ""),
            ("RSS KB", lambda r: r["max_rss_kb"] if r["max_rss_kb"] is not None else ""),
        ]),
        "",
        "## Largest Input Cases",
        "",
        markdown_table(summary["largest"], [
            ("ID", lambda r: r["id"]),
            ("Mode", lambda r: r["mode"]),
            ("Input MiB", lambda r: r["input_mib"]),
            ("Wall s", lambda r: r["wall_s"]),
            ("Throughput MiB/s", lambda r: r["throughput_mib_s"]),
            ("Output MiB", lambda r: r["output_mib"]),
            ("PDFs", lambda r: r["pdf_count"]),
        ]),
        "",
        "## Batch And Warm-Process Cases",
        "",
        markdown_table(summary["batch"], [
            ("ID", lambda r: r["id"]),
            ("Mode", lambda r: r["mode"]),
            ("Input MiB", lambda r: r["input_mib"]),
            ("PDFs", lambda r: r["pdf_count"]),
            ("Wall s", lambda r: r["wall_s"]),
            ("Throughput MiB/s", lambda r: r["throughput_mib_s"]),
            ("RSS KB", lambda r: r["max_rss_kb"] if r["max_rss_kb"] is not None else ""),
        ]),
        "",
        "## All Results",
        "",
        markdown_table(results, [
            ("ID", lambda r: r["id"]),
            ("Status", lambda r: r["status"]),
            ("Mode", lambda r: r["mode"]),
            ("Target", lambda r: r["target"]),
            ("Style", lambda r: r["style"]),
            ("Margin", lambda r: r["margin"]),
            ("Input MiB", lambda r: r["input_mib"]),
            ("Wall s", lambda r: r["wall_s"]),
            ("Avg ms", lambda r: r["avg_ms"] if r["avg_ms"] is not None else ""),
            ("Throughput MiB/s", lambda r: r["throughput_mib_s"] if r["throughput_mib_s"] is not None else ""),
            ("Output MiB", lambda r: r["output_mib"]),
            ("RSS KB", lambda r: r["max_rss_kb"] if r["max_rss_kb"] is not None else ""),
        ]),
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def run_campaign(root, bin_path, platform_name, label):
    plan = linux_plan(root) if platform_name == "linux" else windows_plan(root)
    results = []
    started = time.perf_counter()
    for index, test in enumerate(plan, 1):
        print(f"[{platform_name}] {index}/{len(plan)} {test['id']}", flush=True)
        result = run_test(root, bin_path, test, platform_name, label)
        print(
            f"  -> {result['status']} wall={result['wall_s']}s input={result['input_mib']}MiB throughput={result['throughput_mib_s']}MiB/s",
            flush=True,
        )
        results.append(result)

    result_dir = root / "reports"
    result_dir.mkdir(parents=True, exist_ok=True)
    json_path = result_dir / f"commercial_benchmark_{platform_name}_{label}.json"
    payload = {
        "label": label,
        "platform": platform_name,
        "elapsed_s": round(time.perf_counter() - started, 3),
        "results": results,
        "summary": summarize_results(results),
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    report_path = write_report(root, label, platform_name, results)
    print(f"JSON={json_path}")
    print(f"REPORT={report_path}")
    return payload


def combine_reports(root, label, inputs):
    all_results = []
    source_files = []
    for input_path in inputs:
        data = json.loads(Path(input_path).read_text(encoding="utf-8"))
        source_files.append(str(input_path))
        all_results.extend(data["results"])

    reports = root / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    combined_json = reports / f"commercial_benchmark_combined_{label}.json"
    payload = {
        "label": label,
        "source_files": source_files,
        "results": all_results,
        "summary": summarize_results(all_results),
    }
    combined_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    combined_report = write_report(root, f"combined_{label}", "combined", all_results)
    print(f"COMBINED_JSON={combined_json}")
    print(f"COMBINED_REPORT={combined_report}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--platform", choices=["linux", "windows"], default="linux")
    parser.add_argument("--bin", required=False)
    parser.add_argument("--label", default=time.strftime("%Y%m%d_%H%M%S"))
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--combine", nargs="*")
    args = parser.parse_args()

    root = Path(args.root)
    if args.generate:
        generate_corpus(root)

    if args.combine is not None and len(args.combine) > 0:
        combine_reports(root, args.label, args.combine)
        return

    if args.run:
        if not args.bin:
            default_bin = "build/linux-verify/fast-markdown" if args.platform == "linux" else "build/windows-final2/fast-markdown-imgui.exe"
            args.bin = default_bin
        bin_path = Path(args.bin)
        if not bin_path.exists():
            raise SystemExit(f"binary not found: {bin_path}")
        run_campaign(root, bin_path, args.platform, args.label)


if __name__ == "__main__":
    main()
