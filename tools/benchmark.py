#!/usr/bin/env python3
"""Maintained entry point for RayoMD performance workflows."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def invoke(script: str, arguments: list[str]) -> int:
    return subprocess.call([sys.executable, str(ROOT / "scripts" / script), *arguments], cwd=ROOT)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("run", "run the standard performance watcher"),
        ("compare", "compare RayoMD with Pandoc"),
        ("release", "archive curated release benchmark records"),
        ("competitors", "compare supported Markdown-to-PDF tools"),
    ):
        command = sub.add_parser(name, help=help_text)
        command.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    routes = {
        "run": "perf_watch.py",
        "compare": "compare_pandoc.py",
        "release": "archive_release_benchmarks.py",
        "competitors": "compare_markdown_pdf_tools.py",
    }
    forwarded = args.arguments
    if forwarded[:1] == ["--"]:
        forwarded = forwarded[1:]
    return invoke(routes[args.command], forwarded)


if __name__ == "__main__":
    raise SystemExit(main())