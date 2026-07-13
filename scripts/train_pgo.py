#!/usr/bin/env python3
"""Train a RayoMD PGO build on the maintained balanced performance corpus."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run(command: list[str], stdin: str | None = None) -> None:
    result = subprocess.run(command, input=stdin, text=True, capture_output=True)
    if result.returncode != 0:
        detail = chr(10).join((result.stdout, result.stderr))
        raise RuntimeError(
            f"training command failed ({result.returncode}): {' '.join(command)}" +
            chr(10) + detail
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("benchmark-output/pgo-training"))
    args = parser.parse_args()

    binary = str(args.binary.resolve())
    corpus = args.corpus.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def sized(kind: str) -> Path:
        matches = sorted((corpus / "sized").glob(f"{kind}_*.md"))
        if not matches:
            raise RuntimeError(f"missing sized {kind} training document")
        return matches[-1]

    documents = [
        corpus / "single" / "single_01.md",
        corpus / "features" / "baseline.md",
        sized("ascii"),
        sized("table"),
        sized("unicode"),
    ]
    for repeat in range(3):
        for index, document in enumerate(documents):
            run([
                binary, "--export", str(document),
                str(output / f"export_{repeat}_{index}.pdf"),
                "native", "modern", "normal",
            ])

    batch_inputs = sorted((corpus / "batch").glob("*.md"))
    run([
        binary, "--batch", str(corpus / "batch"), str(output / "batch"),
        "native", "modern", "normal", "--workers=4",
    ])
    payload = chr(10).join(str(path) for path in batch_inputs) + chr(10)
    run([
        binary, "--stdin-batch", str(output / "stdin-batch"),
        "native", "modern", "normal", "--workers=4",
    ], payload)
    run([
        binary, "--serve", str(output / "serve"), "native", "modern", "normal",
    ], payload + "quit" + chr(10))
    print(f"PGO training completed with {len(documents)} document classes and {len(batch_inputs)} batch files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())