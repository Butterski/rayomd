#!/usr/bin/env python3
"""Cross-platform black-box CLI verification for an already-built RayoMD binary."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(binary: Path, *args: str, stdin: str | None = None, expect: int = 0) -> subprocess.CompletedProcess[bytes]:
    proc = subprocess.run(
        [str(binary), *args],
        input=None if stdin is None else stdin.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if proc.returncode != expect:
        output = proc.stdout.decode("utf-8", errors="replace")
        raise AssertionError(f"expected exit {expect}, got {proc.returncode}: {binary} {' '.join(args)}\n{output}")
    return proc


def require_pdf(path: Path, *needles: bytes) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"%PDF-") or len(data) < 100:
        raise AssertionError(f"invalid or empty PDF: {path}")
    for needle in needles:
        if needle not in data:
            raise AssertionError(f"{needle!r} missing from {path}")
    return data


def verify(binary: Path, keep: Path | None) -> None:
    binary = binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"binary does not exist: {binary}")
    root = keep.resolve() if keep else Path(tempfile.mkdtemp(prefix="rayomd-verify-"))
    root.mkdir(parents=True, exist_ok=True)
    try:
        run(binary, "--version")
        run(binary, "--doctor")

        ascii_md = root / "ascii.md"
        ascii_md.write_text("# ASCII\n\n[one](https://example.com/one)\n", encoding="utf-8")
        ascii_pdf = root / "ascii.pdf"
        run(binary, "--export", str(ascii_md), str(ascii_pdf), "native", "tech", "margin=54pt")
        require_pdf(ascii_pdf, b"/Subtype /Link", b"https://example.com/one")

        spaced_dir = root / "path with spaces"
        spaced_dir.mkdir()
        spaced_md = spaced_dir / "quoted input.md"
        spaced_pdf = spaced_dir / "quoted output.pdf"
        spaced_md.write_bytes(ascii_md.read_bytes())
        run(binary, "--export", str(spaced_md), str(spaced_pdf), "native", "tech", "margin=54pt")
        require_pdf(spaced_pdf, b"/Subtype /Link", b"https://example.com/one")

        unicode_md = root / "unicode.md"
        reversible_pdf = root / "reversible.pdf"
        run(binary, "--export", str(ascii_md), str(reversible_pdf), "native", "tech", "normal", "--embed-source")
        reversible = require_pdf(
            reversible_pdf, b"/Type /EmbeddedFile", b"/AFRelationship /Source", b"rayomd-source/1"
        )
        if not reversible.startswith(b"%PDF-2.0"):
            raise AssertionError("reversible output did not select PDF 2.0")
        inspected = run(binary, "--inspect-source", str(reversible_pdf))
        if b"status=intact" not in inspected.stdout or b"digest=valid" not in inspected.stdout:
            raise AssertionError("source inspection did not report an intact profile")
        recovered_md = root / "recovered.md"
        run(binary, "--recover-source", str(reversible_pdf), str(recovered_md))
        if recovered_md.read_bytes() != ascii_md.read_bytes():
            raise AssertionError("recovered Markdown is not byte-exact")
        existing = run(binary, "--recover-source", str(reversible_pdf), str(recovered_md), expect=34)
        if b"already exists" not in existing.stdout:
            raise AssertionError("existing recovery destination was not protected")
        not_reversible = run(binary, "--inspect-source", str(ascii_pdf), expect=30)
        if b"not a reversible" not in not_reversible.stdout:
            raise AssertionError("ordinary PDF was not distinguished from a reversible PDF")
        tampered_pdf = root / "tampered.pdf"
        tampered = bytearray(reversible)
        payload = tampered.find(ascii_md.read_bytes())
        if payload < 0:
            raise AssertionError("embedded source payload was not found")
        tampered[payload] ^= 1
        tampered_pdf.write_bytes(tampered)
        run(binary, "--inspect-source", str(tampered_pdf), expect=32)
        failed_recovery = root / "tampered-recovery.md"
        run(binary, "--recover-source", str(tampered_pdf), str(failed_recovery), expect=32)
        if failed_recovery.exists():
            raise AssertionError("failed recovery left a partial output file")

        unsupported_pdf = root / "unsupported-profile.pdf"
        unsupported = reversible.replace(b"rayomd-source/1", b"rayomd-source/2", 1)
        unsupported_pdf.write_bytes(unsupported)
        run(binary, "--inspect-source", str(unsupported_pdf), expect=31)

        unrelated_pdf = root / "unrelated-attachment.pdf"
        unrelated = reversible.replace(b"/Metadata", b"/Metadatu", 1)
        unrelated_pdf.write_bytes(unrelated)
        run(binary, "--inspect-source", str(unrelated_pdf), expect=30)

        unicode_md.write_text("# Unicode\n\nZażółć gęślą jaźń: 日本語.\n", encoding="utf-8")
        unicode_pdf = root / "unicode.pdf"
        run(binary, "--export", str(unicode_md), str(unicode_pdf), "native", "modern", "normal")
        require_pdf(unicode_pdf)

        stdin_pdf = root / "stdin.pdf"
        run(binary, "--stdin", str(stdin_pdf), "native", "modern", "normal", stdin="# Stdin\n\nHello **stdin**.\n")
        require_pdf(stdin_pdf)

        missing = run(binary, "--stdin", expect=2)
        if b"--stdin requires" not in missing.stdout:
            raise AssertionError("missing --stdin argument diagnostic")
        unknown = run(binary, "--export", str(ascii_md), str(root / "bad.pdf"), "--unknown", expect=2)
        if b"unrecognized export option" not in unknown.stdout:
            raise AssertionError("missing unknown-option diagnostic")
        missing_input = run(binary, "--export", str(root / "missing.md"), str(root / "missing.pdf"), expect=3)
        if b"could not read input Markdown file" not in missing_input.stdout:
            raise AssertionError("missing input-file diagnostic")

        doc = root / "security" / "doc"
        out = root / "security" / "out"
        doc.mkdir(parents=True)
        out.mkdir(parents=True)
        image_source = Path(__file__).resolve().parents[1] / "docs" / "assets" / "branding" / "rayomd.png"
        shutil.copyfile(image_source, doc / "allowed.png")
        shutil.copyfile(image_source, doc.parent / "outside.png")
        (doc / "allowed.md").write_text("![allowed-local](allowed.png)\n", encoding="utf-8")
        run(binary, "--export", str(doc / "allowed.md"), str(out / "allowed.pdf"))
        allowed = require_pdf(out / "allowed.pdf")
        if b"allowed-local" in allowed:
            raise AssertionError("contained local image unexpectedly fell back")
        (doc / "escape.md").write_text("![blocked-local](../outside.png)\n", encoding="utf-8")
        run(binary, "--export", str(doc / "escape.md"), str(out / "escape.pdf"))
        require_pdf(out / "escape.pdf", b"blocked-local")
        (doc / "url.md").write_text("![blocked-url](http://127.0.0.1:9/image.png)\n", encoding="utf-8")
        run(binary, "--export", str(doc / "url.md"), str(out / "url-default.pdf"))
        require_pdf(out / "url-default.pdf", b"blocked-url")
        run(binary, "--export", str(doc / "url.md"), str(out / "url-enabled.pdf"), "--allow-url-images")
        require_pdf(out / "url-enabled.pdf", b"blocked-url")
        print(f"RayoMD CLI verification passed: {binary}")
    finally:
        if keep is None:
            shutil.rmtree(root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="already-built rayomd executable")
    parser.add_argument("--keep-output", type=Path, help="retain verifier artifacts in this directory")
    args = parser.parse_args()
    try:
        verify(args.binary, args.keep_output)
    except (AssertionError, FileNotFoundError, OSError) as exc:
        print(f"verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
