#!/usr/bin/env python3
import argparse
import json
import random
import shutil
import subprocess
import time
from pathlib import Path


WORDS = [
    "markdown", "renderer", "glyph", "unicode", "latency", "pipeline", "cache",
    "allocation", "paragraph", "table", "conversion", "baseline", "vector",
    "document", "profile", "batch", "layout", "export", "polski", "zażółć",
    "gęślą", "jaźń", "résumé", "naïve", "東京", "Москва", "Δοκιμή",
]

INLINE = [
    "**bold text**", "*italic span*", "~~strike marker~~", "`inline_code()`",
    "[link label](https://example.com/very/long/path?with=query)",
    "$x^2 + y^2 = z^2$", "✅", "⚠️", "❌",
]


def sentence(rng, min_words=10, max_words=28):
    n = rng.randint(min_words, max_words)
    parts = []
    for _ in range(n):
        if rng.random() < 0.16:
            parts.append(rng.choice(INLINE))
        else:
            parts.append(rng.choice(WORDS))
    return " ".join(parts).capitalize() + "."


def table(rng, rows=8, cols=4):
    headers = [f"Column {i + 1}" for i in range(cols)]
    aligns = ["---", ":---", "---:", ":---:"]
    out = ["| " + " | ".join(headers) + " |"]
    out.append("| " + " | ".join(aligns[i % len(aligns)] for i in range(cols)) + " |")
    for _ in range(rows):
        cells = [sentence(rng, 3, 8).replace("|", "/") for _ in range(cols)]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def code_block(rng, lines=12):
    out = ["```cpp"]
    for i in range(lines):
        name = rng.choice(["width", "cid", "glyph", "line", "offset"])
        out.append(f"auto {name}_{i} = compute_{name}({i}, \"{rng.choice(WORDS)}\");")
    out.append("```")
    return "\n".join(out)


def make_doc(seed, sections, paragraphs_per_section):
    rng = random.Random(seed)
    out = [
        "---",
        f"title: Stress {seed}",
        "---",
        "",
        f"# Stress Document {seed}",
        "",
    ]
    for s in range(sections):
        level = 2 + (s % 3)
        out.append("#" * level + f" Section {s + 1}")
        out.append("")
        for _ in range(paragraphs_per_section):
            out.append(" ".join(sentence(rng) for _ in range(rng.randint(2, 5))))
            out.append("")
        out.append("> " + " ".join(sentence(rng, 8, 18) for _ in range(2)))
        out.append("")
        out.extend([f"- {sentence(rng, 5, 14)}" for _ in range(rng.randint(4, 8))])
        out.append("")
        out.extend([f"{i + 1}. {sentence(rng, 5, 14)}" for i in range(rng.randint(4, 8))])
        out.append("")
        if s % 2 == 0:
            out.append(table(rng, rows=rng.randint(4, 10), cols=rng.randint(3, 5)))
            out.append("")
        if s % 3 == 0:
            out.append(code_block(rng, lines=rng.randint(8, 20)))
            out.append("")
        if s % 4 == 0:
            out.append("$$")
            out.append(r"\int_0^\infty e^{-x^2} dx = \frac{\sqrt{\pi}}{2}")
            out.append("$$")
            out.append("")
    return "\n".join(out)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def generate(root, seed):
    corpus = root / "corpus"
    if corpus.exists():
        shutil.rmtree(corpus)
    write(corpus / "single" / "style_mix.md", make_doc(seed, 8, 3))
    write(corpus / "single" / "large_mix.md", make_doc(seed + 1, 36, 5))

    for i in range(10):
        write(corpus / "batch10" / f"doc_{i:02d}.md", make_doc(seed + 100 + i, 6 + i % 4, 2 + i % 3))

    for i in range(1000):
        sections = 2 + (i % 5)
        paragraphs = 1 + (i % 3)
        write(corpus / "batch1000" / f"doc_{i:04d}.md", make_doc(seed + 1000 + i, sections, paragraphs))

    for i in range(100):
        write(corpus / "random100" / f"random_{i:03d}.md", make_doc(seed + 3000 + i, random.Random(seed + i).randint(1, 14), random.Random(seed + i * 7).randint(1, 5)))


def parse_bench(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            values[k] = v
    return values


def parse_time(stderr):
    values = {}
    for line in stderr.splitlines():
        line = line.strip()
        if line.startswith("Elapsed (wall clock) time"):
            values["elapsed"] = line.split(":", 1)[1].strip()
        elif line.startswith("Maximum resident set size"):
            values["max_rss_kb"] = line.split(":", 1)[1].strip()
        elif line.startswith("User time"):
            values["user_s"] = line.split(":", 1)[1].strip()
        elif line.startswith("System time"):
            values["sys_s"] = line.split(":", 1)[1].strip()
    return values


def run_cmd(cmd, cwd):
    start = time.perf_counter()
    proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elapsed = time.perf_counter() - start
    return proc.returncode, elapsed, proc.stdout, proc.stderr


def run_benchmarks(root, bin_path, label, cwd):
    corpus = root / "corpus"
    out = root / "results" / label
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)

    results = {"label": label, "bench": {}, "batch": {}}
    bench_specs = [
        ("style_mix", corpus / "single" / "style_mix.md", 1000),
        ("large_mix", corpus / "single" / "large_mix.md", 200),
    ]
    for name, input_path, iterations in bench_specs:
        output_dir = out / name
        cmd = [str(bin_path), "--bench", str(input_path), str(output_dir), str(iterations), "elegant", "normal"]
        rc, elapsed, stdout, stderr = run_cmd(cmd, cwd)
        if rc != 0:
            raise SystemExit(f"bench {name} failed rc={rc}\nstdout={stdout}\nstderr={stderr}")
        values = parse_bench(output_dir / "bench-results.txt")
        values["wall_s"] = round(elapsed, 6)
        results["bench"][name] = values

    time_bin = shutil.which("time")
    if time_bin == "time":
        time_bin = "/usr/bin/time" if Path("/usr/bin/time").exists() else None
    if not time_bin and Path("/usr/bin/time").exists():
        time_bin = "/usr/bin/time"

    for name in ["batch10", "random100", "batch1000"]:
        input_dir = corpus / name
        output_dir = out / f"{name}_pdf"
        cmd = [str(bin_path), "--batch", str(input_dir), str(output_dir), "native", "elegant", "normal"]
        run = cmd
        if time_bin:
            run = [time_bin, "-v"] + cmd
        rc, elapsed, stdout, stderr = run_cmd(run, cwd)
        if rc != 0:
            raise SystemExit(f"batch {name} failed rc={rc}\nstdout={stdout}\nstderr={stderr}")
        pdf_count = len(list(output_dir.glob("*.pdf")))
        results["batch"][name] = {
            "wall_s": round(elapsed, 6),
            "pdf_count": pdf_count,
            **parse_time(stderr),
        }

    (out / "results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(results, indent=2, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="benchmark-output/stress")
    parser.add_argument("--bin", default="build/linux-verify/fast-markdown")
    parser.add_argument("--label", default="run")
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()

    root = Path(args.root)
    if args.generate:
        generate(root, args.seed)
    if args.run:
        run_benchmarks(root, Path(args.bin), args.label, Path.cwd())


if __name__ == "__main__":
    main()
