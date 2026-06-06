#!/usr/bin/env bash
set -euo pipefail

source_dir="${1:-/mnt/e/projekty/fast-markdown/imgui-version}"
label="${2:-$(date +%Y%m%d_%H%M%S)}"
work_dir="$(mktemp -d "${HOME}/fast-md-simdutf.XXXXXX")"
dest_dir="${source_dir}/benchmark-output/simdutf-large-unicode/${label}"

printf 'EXT4_WORKDIR=%s\n' "$work_dir"

rsync -a --no-owner --no-group \
  --exclude=.git \
  --exclude=build \
  --exclude=benchmark-output \
  "$source_dir/" "$work_dir/"

cd "$work_dir"
python3 scripts/commercial_benchmark.py --root benchmark-output/commercial --generate
python3 - <<'PY'
from pathlib import Path
from scripts.commercial_benchmark import write_sized_doc, MIB
write_sized_doc(Path("benchmark-output/commercial/corpus/docs/unicode_10mb.md"), 10 * MIB, "unicode")
PY

cmake -S . -B build/off -DCMAKE_BUILD_TYPE=Release -DFAST_MARKDOWN_USE_SIMDUTF=OFF >/dev/null
cmake --build build/off -j 4 >/dev/null
cmake -S . -B build/on -DCMAKE_BUILD_TYPE=Release -DFAST_MARKDOWN_USE_SIMDUTF=ON >/dev/null
cmake --build build/on -j 4 >/dev/null

mkdir -p "benchmark-output/simdutf-large-unicode/${label}"
SIMDUTF_LABEL="$label" python3 - <<'PY'
import json
import shutil
import subprocess
import time
from pathlib import Path

root = Path("benchmark-output/commercial/corpus/docs")
out_root = Path("benchmark-output/simdutf-large-unicode")
label = Path(__import__("os").environ.get("SIMDUTF_LABEL", "run")).name
out_root = out_root / label
docs = [
    ("unicode_1mb", 40),
    ("unicode_2_5mb", 12),
    ("unicode_5mb", 8),
    ("unicode_10mb", 4),
]
variants = [
    ("off", Path("build/off/fast-markdown")),
    ("on", Path("build/on/fast-markdown")),
]

def parse_bench(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            values[k] = v
    return values

def parse_time(stderr):
    values = {}
    for raw in stderr.splitlines():
        line = raw.strip()
        if line.startswith("Maximum resident set size"):
            values["max_rss_kb"] = line.split(":", 1)[1].strip()
        elif line.startswith("User time"):
            values["user_s"] = line.split(":", 1)[1].strip()
        elif line.startswith("System time"):
            values["sys_s"] = line.split(":", 1)[1].strip()
    return values

results = []
for doc, iterations in docs:
    input_path = root / f"{doc}.md"
    for variant, binary in variants:
        output_dir = out_root / f"{doc}_{variant}"
        if output_dir.exists():
            shutil.rmtree(output_dir)
        output_dir.mkdir(parents=True)
        cmd = [
            "/usr/bin/time", "-v", str(binary), "--bench", str(input_path), str(output_dir),
            str(iterations), "elegant", "normal"
        ]
        start = time.perf_counter()
        proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wall_s = time.perf_counter() - start
        if proc.returncode != 0:
            raise SystemExit(f"{doc} {variant} failed\nstdout={proc.stdout}\nstderr={proc.stderr}")
        bench = parse_bench(output_dir / "bench-results.txt")
        time_meta = parse_time(proc.stderr)
        result = {
            "doc": doc,
            "variant": variant,
            "input_bytes": int(bench["input_bytes"]),
            "input_mib": round(int(bench["input_bytes"]) / (1024 * 1024), 4),
            "iterations": int(bench["iterations"]),
            "avg_ms": float(bench["avg_ms"]),
            "total_ms": float(bench["total_ms"]),
            "wall_s": round(wall_s, 6),
            "avg_pdf_bytes": int(bench["avg_pdf_bytes"]),
            "path": bench["path"],
            "max_rss_kb": int(time_meta.get("max_rss_kb", "0")),
            "user_s": time_meta.get("user_s"),
            "sys_s": time_meta.get("sys_s"),
        }
        results.append(result)
        print(json.dumps(result), flush=True)

comparisons = []
for doc, _ in docs:
    off = next(r for r in results if r["doc"] == doc and r["variant"] == "off")
    on = next(r for r in results if r["doc"] == doc and r["variant"] == "on")
    comparisons.append({
        "doc": doc,
        "input_mib": off["input_mib"],
        "off_avg_ms": off["avg_ms"],
        "on_avg_ms": on["avg_ms"],
        "delta_ms": round(on["avg_ms"] - off["avg_ms"], 4),
        "delta_pct": round(((on["avg_ms"] - off["avg_ms"]) / off["avg_ms"]) * 100.0, 2),
        "off_rss_kb": off["max_rss_kb"],
        "on_rss_kb": on["max_rss_kb"],
    })

payload = {"results": results, "comparisons": comparisons}
(out_root / "simdutf_large_unicode.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")

lines = [
    "# simdutf Large Unicode Benchmark",
    "",
    "| Doc | Input MiB | OFF avg ms | ON avg ms | ON delta | OFF RSS KB | ON RSS KB |",
    "|---|---:|---:|---:|---:|---:|---:|",
]
for c in comparisons:
    lines.append(
        f"| {c['doc']} | {c['input_mib']} | {c['off_avg_ms']} | {c['on_avg_ms']} | {c['delta_pct']}% | {c['off_rss_kb']} | {c['on_rss_kb']} |"
    )
(out_root / "simdutf_large_unicode.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

mkdir -p "$dest_dir"
cp -a "benchmark-output/simdutf-large-unicode/${label}/." "$dest_dir/"
printf 'COPIED_RESULTS=%s\n' "$dest_dir"
