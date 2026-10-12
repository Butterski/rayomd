#!/usr/bin/env python3
"""Cross-platform black-box CLI verification for an already-built RayoMD binary."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


def run(
    binary: Path, *args: str, stdin: str | None = None, expect: int = 0, timeout: float | None = None
) -> subprocess.CompletedProcess[bytes]:
    proc = subprocess.run(
        [str(binary), *args],
        input=None if stdin is None else stdin.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=timeout,
    )
    if proc.returncode != expect:
        output = proc.stdout.decode("utf-8", errors="replace")
        raise AssertionError(f"expected exit {expect}, got {proc.returncode}: {binary} {' '.join(args)}\n{output}")
    return proc


def run_raw_windows(binary: Path, command_line: str, expect: int = 0) -> subprocess.CompletedProcess[bytes]:
    proc = subprocess.run(
        command_line,
        executable=str(binary),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=10,
    )
    if proc.returncode != expect:
        output = proc.stdout.decode("utf-8", errors="replace")
        raise AssertionError(f"expected exit {expect}, got {proc.returncode}: {command_line}\n{output}")
    return proc


def require_pdf(path: Path, *needles: bytes) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"%PDF-") or len(data) < 100:
        raise AssertionError(f"invalid or empty PDF: {path}")
    for needle in needles:
        if needle not in data:
            raise AssertionError(f"{needle!r} missing from {path}")
    return data


def report_records(text: str) -> dict[str, dict]:
    """The JSON lines of a batch report by input file name (either path separator)."""
    records = {}
    for line in text.splitlines():
        if line.startswith("{"):
            record = json.loads(line)
            records[record["input"].replace("\\", "/").rsplit("/", 1)[-1]] = record
    return records


def verify_batch(binary: Path, root: Path) -> None:
    batch_in = root / "batch in"
    (batch_in / "sub" / "deeper").mkdir(parents=True)
    (batch_in / ".hidden").mkdir()
    (batch_in / "a.md").write_text("# A\n\n![x](missing.png)\n", encoding="utf-8")
    (batch_in / "sub" / "b.md").write_text("# B\n", encoding="utf-8")
    (batch_in / "sub" / "deeper" / "c.md").write_text("# C\n", encoding="utf-8")
    (batch_in / ".hidden" / "h.md").write_text("# H\n", encoding="utf-8")
    batch_out = root / "batch out"
    report_path = root / "report.jsonl"
    run(binary, "--batch", str(batch_in), str(batch_out), "native", "modern", "normal", "--recursive",
        f"--report={report_path}")
    made = {path.relative_to(batch_out).as_posix() for path in batch_out.rglob("*.pdf")}
    if made != {"a.pdf", "sub/b.pdf", "sub/deeper/c.pdf"}:
        raise AssertionError(f"--recursive wrote {sorted(made)}")
    records = report_records(report_path.read_text(encoding="utf-8"))
    if sorted(records) != ["a.md", "b.md", "c.md"] or any(
        record["status"] != "ok" or record["pages"] != 1 or record["bytes"] <= 0 for record in records.values()
    ) or records["a.md"]["failed_images"] != 1:
        raise AssertionError(f"unexpected batch report: {records}")
    # Nothing changed, so the next run skips every document, except a PDF that was cut short.
    cut = batch_out / "sub" / "b.pdf"
    cut.write_bytes(cut.read_bytes()[:100])
    rerun = run(binary, "--batch", str(batch_in), str(batch_out), "--recursive", "--skip-unchanged", "--report=-")
    statuses = {name: record["status"] for name, record in report_records(rerun.stdout.decode("utf-8")).items()}
    if statuses != {"a.md": "skipped", "b.md": "ok", "c.md": "skipped"}:
        raise AssertionError(f"--skip-unchanged gave {statuses}")
    missing = batch_in / "missing.md"
    listed = run(binary, "--stdin-batch", str(root / "stdin batch out"), "--report=-",
                 stdin=f"{batch_in / 'a.md'}\n{missing}\n", expect=3)
    records = report_records(listed.stdout.decode("utf-8"))
    if records.get("a.md", {}).get("status") != "ok" or records.get("missing.md", {}).get("code") != 3:
        raise AssertionError(f"unexpected stdin batch report: {records}")


def verify_theme(binary: Path, root: Path) -> None:
    """--theme reads a theme file whose relative paths are its folder's: a logo in the header and
    on a cover page, page numbers in the footer; an unknown key fails with exit 2."""
    theme_dir = root / "theme dir"
    theme_dir.mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[1] / "docs" / "assets" / "branding" / "rayomd.png",
                    theme_dir / "logo.png")
    theme = theme_dir / "acme.theme"
    theme.write_text("# ACME\nlogo = logo.png\nheader-left = {logo}\nheader-right = {title}\n"
                     "footer-center = {page} / {pages}\nheading-color = #0B3D91\ncover = yes\n", encoding="utf-8")
    source = root / "themed.md"
    source.write_text("---\ntitle: Report\nauthor: Ann\n---\n\n# Results\n\nText.\n", encoding="utf-8")
    themed_pdf = root / "themed.pdf"
    run(binary, "--export", str(source), str(themed_pdf), f"--theme={theme}")
    require_pdf(themed_pdf, b"/PageLabels", b"/Im1 Do Q", b"(Report) Tj", b"(1 / 1) Tj", b"0.04 0.24 0.57 rg")
    bad = theme_dir / "bad.theme"
    bad.write_text("colour = red\n", encoding="utf-8")
    rejected = run(binary, "--export", str(source), str(root / "bad-theme.pdf"), f"--theme={bad}", expect=2)
    if b"unknown key 'colour'" not in rejected.stdout:
        raise AssertionError("an unknown theme key was not reported")


def verify_pdfa(binary: Path, root: Path) -> None:
    """--pdfa: PDF 1.7 with every font embedded (formulas as their source), printable links, an
    sRGB output intent, PDF/A-3b XMP and a file identifier; with --embed-source too, still PDF 1.7,
    and the source recovers byte for byte."""
    source = root / "archive.md"
    source.write_text("---\ntitle: Archive & <Co>\n---\n\n# Archive\n\nSee [site](https://example.com) and $x^2$.\n",
                      encoding="utf-8")
    archive_pdf = root / "archive.pdf"
    run(binary, "--export", str(source), str(archive_pdf), "--pdfa", "--page-numbers")
    archive = require_pdf(archive_pdf, b"/S /GTS_PDFA1", b'pdfaid:part="3"', b"/FontFile2", b"/Subtype /Link /F 4 ",
                          b"Archive &amp; &lt;Co&gt;")
    if (not archive.startswith(b"%PDF-1.7\n") or b"/Subtype /Type1" in archive or
            not re.search(rb"/ID \[<([0-9A-F]{32})> <\1>\]", archive)):
        raise AssertionError("--pdfa did not write a PDF/A-3b file")
    reversible_pdf = root / "archive-source.pdf"
    run(binary, "--export", str(source), str(reversible_pdf), "--pdfa", "--embed-source")
    if not require_pdf(reversible_pdf, b"rayomd-source/1", b"<pdfaExtension:schemas>").startswith(b"%PDF-1.7\n"):
        raise AssertionError("--pdfa --embed-source did not stay PDF 1.7")
    recovered = root / "archive-recovered.md"
    run(binary, "--recover-source", str(reversible_pdf), str(recovered))
    if recovered.read_bytes() != source.read_bytes():
        raise AssertionError("--pdfa --embed-source did not recover the source")


def verify_footnotes(binary: Path, root: Path) -> None:
    """Footnotes: a reference links to its note after the text and the note's number back to it;
    a reference to no note and the definition's own line are not links."""
    source = root / "notes.md"
    source.write_text("Claim[^a] and [^none].\n\n[^a]: The note.\n", encoding="utf-8")
    notes_pdf = root / "notes.pdf"
    run(binary, "--export", str(source), str(notes_pdf))
    if require_pdf(notes_pdf, b"/Dest [").count(b"/Dest [") != 2:
        raise AssertionError("footnotes: expected a link to the note and one back")


def verify_highlight(binary: Path, root: Path) -> None:
    """Fenced code in a language GitHub knows takes its token colours, each run after its colour in
    one text object; --no-highlight shows it in one colour, as is a block in no known language."""
    source = root / "code.md"
    source.write_text("```python\ndef f(x): return 'a'  # note\n```\n\n```text\ndef plain(): pass\n```\n",
                      encoding="utf-8")
    colored_pdf = root / "code.pdf"
    run(binary, "--export", str(source), str(colored_pdf))
    colored = require_pdf(colored_pdf, b"Tm (def ) Tj .4 .224 .729 rg (f) Tj ", b".349 .388 .431 rg (# note) Tj",
                          b"0.12 0.12 0.12 rg BT /F3 9.5 Tf 1 0 0 1 ")
    if colored.count(b".812 .133 .18 rg") != 2:   # def and return; the text block has none
        raise AssertionError("highlighting: expected two keyword runs")
    plain_pdf = root / "code-plain.pdf"
    run(binary, "--export", str(source), str(plain_pdf), "--no-highlight")
    plain = require_pdf(plain_pdf, b"(def f\\(x\\): return 'a'  # note) Tj")
    if b".812 .133 .18 rg" in plain:
        raise AssertionError("--no-highlight still coloured the code")


def verify_contents(binary: Path, root: Path) -> None:
    """--toc: a table of contents after the title heading, its entries linked to the headings to
    --toc-depth levels; a depth outside 1-6 fails with exit 2."""
    source = root / "manual.md"
    source.write_text("# Manual\n\n## Install\n\nText.\n\n### Linux\n\nText.\n", encoding="utf-8")
    shallow_pdf = root / "manual-shallow.pdf"
    run(binary, "--export", str(source), str(shallow_pdf), "--toc", "--toc-depth=1")
    deep_pdf = root / "manual-deep.pdf"
    run(binary, "--export", str(source), str(deep_pdf), "--toc")
    # Three bookmarks, and one or two entries' links.
    if (require_pdf(shallow_pdf, b"(Contents) Tj").count(b"/Dest [") != 4 or
            require_pdf(deep_pdf, b"(Contents) Tj").count(b"/Dest [") != 5):
        raise AssertionError("--toc: unexpected table of contents links")
    run(binary, "--export", str(source), str(root / "manual-bad.pdf"), "--toc-depth=7", expect=2)


def verify_book(binary: Path, root: Path) -> None:
    """--book: the files a SUMMARY.md lists as one PDF, each from a new page, a link between them
    internal; a file it lists that is missing, and --embed-source, fail."""
    book = root / "book"
    (book / "part").mkdir(parents=True)
    (book / "SUMMARY.md").write_text(
        "# Summary\n\n- [One](one.md)\n- [Two](part/two%20b.md)\n", encoding="utf-8")
    (book / "one.md").write_text("# One\n\nSee [two](part/two%20b.md#later).\n", encoding="utf-8")
    (book / "part" / "two b.md").write_text("Text.\n\n## Later\n\nBack to [one](../one.md).\n", encoding="utf-8")
    pdf = root / "book.pdf"
    run(binary, "--book", str(book), str(pdf), "--toc")
    data = require_pdf(pdf, b"(Contents) Tj")
    # The table, one and two (titled from SUMMARY.md) on pages of their own; no link left as a URI.
    if data.count(b"/Type /Page ") != 3 or b"(Two) Tj" not in data or b"/URI" in data:
        raise AssertionError("--book: unexpected pages or links")
    (book / "SUMMARY.md").write_text("- [Gone](gone.md)\n", encoding="utf-8")
    run(binary, "--book", str(book), str(root / "book-missing.pdf"), expect=3)
    run(binary, "--book", str(book / "one.md"), str(root / "book-embed.pdf"), "--embed-source", expect=2)


def verify_svg(binary: Path, root: Path) -> None:
    """An SVG image is drawn as a form XObject, its text in the document's font."""
    (root / "chart.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="120" height="60">'
        '<rect width="120" height="60" fill="#36c"/><text x="10" y="40">Chart</text></svg>', encoding="utf-8")
    source = root / "figure.md"
    source.write_text("# Figure\n\n![chart](chart.svg)\n", encoding="utf-8")
    pdf = root / "figure.pdf"
    run(binary, "--export", str(source), str(pdf))
    data = require_pdf(pdf, b"/Subtype /Form", b"(Chart) Tj")
    if b"/Subtype /Image" in data:
        raise AssertionError("SVG image: drawn as a raster image")


def verify_pdfua(binary: Path, root: Path) -> None:
    """--pdfua: a tagged PDF 1.7, its link annotation in a Link element of its structure tree."""
    source = root / "tagged.md"
    source.write_text("# Tagged\n\nA [link](https://example.com).\n\n- item\n", encoding="utf-8")
    pdf = root / "tagged.pdf"
    run(binary, "--export", str(source), str(pdf), "--pdfua")
    data = require_pdf(pdf, b"/StructTreeRoot", b"/MarkInfo << /Marked true >>", b"pdfuaid:part=\"1\"",
                       b"/S /Link", b"/Type /OBJR", b"/StructParent ", b"/Tabs /S")
    if not data.startswith(b"%PDF-1.7") or data.count(b"BDC\n") + data.count(b"BMC\n") != data.count(b"EMC\n"):
        raise AssertionError("--pdfua: unbalanced marked content or wrong version")


def pdf_streams(data: bytes) -> list[tuple[bytes, bytes]]:
    """The dictionary and payload of every stream of a PDF, in file order."""
    streams = []
    at = 0
    while (at := data.find(b" 0 obj\n", at)) != -1:
        at += 7
        start = data.find(b"\nstream\n", at)
        if start == -1 or data.find(b"\nendobj\n", at) < start:
            continue
        dictionary = data[at:start]
        length = int(re.search(rb"/Length (\d+)", dictionary).group(1))
        streams.append((dictionary, data[start + 8:start + 8 + length]))
        at = start + 8 + length
    return streams


def verify_compression(binary: Path, root: Path) -> None:
    """--compress: what changes is FlateDecode and inflates (strict zlib: header and Adler-32)
    to exactly the stream of the uncompressed export, and the file is smaller."""
    source = root / "compress.md"
    lines = "".join(f"Line {n} of the same words, Za\u017c\u00f3\u0142\u0107.\n\n" for n in range(150))
    source.write_text("# Compressed\n\n" + lines, encoding="utf-8")
    plain_pdf = root / "plain.pdf"
    packed_pdf = root / "packed.pdf"
    run(binary, "--export", str(source), str(plain_pdf), "--page-numbers")
    run(binary, "--export", str(source), str(packed_pdf), "--page-numbers", "--compress")
    plain = require_pdf(plain_pdf)
    packed = require_pdf(packed_pdf, b"/Filter /FlateDecode")
    plain_streams = pdf_streams(plain)
    packed_streams = pdf_streams(packed)
    if len(plain_streams) != len(packed_streams):
        raise AssertionError("--compress changed the number of streams")
    compressed = 0
    for (plain_dict, plain_data), (packed_dict, packed_data) in zip(plain_streams, packed_streams):
        if packed_dict == plain_dict:
            if packed_data != plain_data:
                raise AssertionError("--compress changed an uncompressed stream")
            continue
        if b"/Filter /FlateDecode" not in packed_dict or zlib.decompress(packed_data) != plain_data:
            raise AssertionError(f"a compressed stream does not inflate to the original: {packed_dict!r}")
        compressed += 1
    if compressed < 2 or len(packed) * 2 > len(plain):
        raise AssertionError(f"--compress: {compressed} streams compressed, {len(plain)} -> {len(packed)} bytes")


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def png_predict(row: bytes, prev: bytes, bpp: int, kind: int, sign: int) -> bytes:
    """A PNG row filter (sign -1) or its inverse (sign 1), as the specification writes them."""
    out = bytearray(len(row))
    source = out if sign > 0 else row
    for x in range(len(row)):
        a = source[x - bpp] if x >= bpp else 0
        b = prev[x]
        c = prev[x - bpp] if x >= bpp else 0
        predictor = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        out[x] = (row[x] + sign * predictor) & 255
    return bytes(out)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png_file(width: int, height: int, color_type: int, rows: list[bytes], extra: bytes) -> bytes:
    """An 8-bit PNG of `rows`, each row under filter y % 5, with the chunks `extra` before its data."""
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    filtered = bytearray()
    prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        filtered.append(y % 5)
        filtered += png_predict(row, prev, bpp, y % 5, -1)
        prev = row
    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + extra
            + png_chunk(b"IDAT", zlib.compress(bytes(filtered), 9)) + png_chunk(b"IEND", b""))


def pdf_image_planes(data: bytes) -> dict[int, tuple[bytes, bytes | None]]:
    """By width: every image of a PDF but soft masks, as its colour bytes and those of its soft
    mask, each inflated (zlib) and its /Predictor 15 rows unfiltered."""
    objects = {}
    at = 0
    while match := re.compile(rb"(\d+) 0 obj\n").search(data, at):
        start = match.end()
        stream = data.find(b"\nstream\n", start)
        end = data.find(b"\nendobj\n", start)
        if stream == -1 or end < stream:
            at = end
            continue
        dictionary = data[start:stream]
        length = int(re.search(rb"/Length (\d+)", dictionary).group(1))
        objects[int(match.group(1))] = (dictionary, data[stream + 8:stream + 8 + length])
        at = stream + 8 + length

    def plane(dictionary: bytes, payload: bytes) -> bytes:
        width = int(re.search(rb"/Width (\d+)", dictionary).group(1))
        height = int(re.search(rb"/Height (\d+)", dictionary).group(1))
        colors = 3 if b"/DeviceRGB" in dictionary else 1
        if b"/FlateDecode" not in dictionary or b"/Predictor 15" not in dictionary:
            raise AssertionError(f"image not a /Predictor 15 Flate stream: {dictionary!r}")
        rows = zlib.decompress(payload)
        row_bytes = width * colors
        if len(rows) != (row_bytes + 1) * height:
            raise AssertionError(f"image rows of the wrong size: {dictionary!r}")
        out = bytearray()
        prev = bytes(row_bytes)
        for y in range(height):
            line = rows[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
            prev = png_predict(line[1:], prev, colors, line[0], 1)
            out += prev
        return bytes(out)

    masks = {int(n) for dictionary, _ in objects.values() for n in re.findall(rb"/SMask (\d+) 0 R", dictionary)}
    images = {}
    for number, (dictionary, payload) in objects.items():
        if b"/Subtype /Image" not in dictionary or number in masks:
            continue
        mask = re.search(rb"/SMask (\d+) 0 R", dictionary)
        width = int(re.search(rb"/Width (\d+)", dictionary).group(1))
        images[width] = (plane(dictionary, payload), plane(*objects[int(mask.group(1))]) if mask else None)
    return images


def verify_png_lossless(binary: Path, root: Path) -> None:
    """PNGs with transparency are re-encoded without loss: inflated with Python's zlib and
    unfiltered, a PDF image and its /SMask give back every source pixel's colour and alpha. Every
    kind of PNG with alpha, every row filter in the source, and widths around the steps of the
    RGBA split (16 pixels, then 2); an opaque RGBA image gets no soft mask."""
    rng = random.Random(20261013)
    widths = (1, 2, 3, 15, 16, 17, 18, 33, 47, 130)
    palette = bytes(rng.randrange(256) for _ in range(3 * 40))
    trns = bytes(rng.choice((0, 255, rng.randrange(256))) for _ in range(25))   # fewer entries than the palette
    kinds = {"rgba": 6, "rgba-opaque": 6, "gray-alpha": 4, "palette": 3, "rgb-key": 2, "gray-key": 0}
    for name, color_type in kinds.items():
        folder = root / f"png-{name}"
        folder.mkdir()
        expected = {}
        for width in widths:
            height = 7
            colors, alphas, rows = bytearray(), bytearray(), []
            for y in range(height):
                row = bytearray()
                for x in range(width):
                    value = (x * 7 + y * 13) & 255 if (x + y) % 3 else rng.randrange(256)
                    alpha = 255 if name == "rgba-opaque" else rng.choice((0, 255, rng.randrange(256), (x * 9) & 255))
                    if color_type == 6:
                        pixel = bytes((value, rng.randrange(256), 255 - value))
                        row += pixel + bytes((alpha,))
                    elif color_type == 4:
                        pixel = bytes((value,))
                        row += pixel + bytes((alpha,))
                    elif color_type == 3:
                        index = value % 40
                        row.append(index)
                        pixel = palette[3 * index:3 * index + 3]
                        alpha = trns[index] if index < len(trns) else 255
                    elif color_type == 2:
                        pixel = bytes((10, 20, 30)) if x % 4 == 1 else bytes((value, value ^ 85, 30))
                        row += pixel
                        alpha = 0 if pixel == bytes((10, 20, 30)) else 255
                    else:
                        pixel = bytes((77,)) if x % 4 == 1 else bytes((value if value != 77 else 78,))
                        row += pixel
                        alpha = 0 if pixel == bytes((77,)) else 255
                    colors += pixel
                    alphas.append(alpha)
                rows.append(bytes(row))
            extra = b""
            if color_type == 3:
                extra = png_chunk(b"PLTE", palette) + png_chunk(b"tRNS", trns)
            elif color_type in (0, 2):
                extra = png_chunk(b"tRNS", bytes((0, 10, 0, 20, 0, 30)) if color_type == 2 else bytes((0, 77)))
            (folder / f"w{width}.png").write_bytes(png_file(width, height, color_type, rows, extra))
            expected[width] = (bytes(colors), None if all(a == 255 for a in alphas) else bytes(alphas))
        source = folder / "images.md"
        source.write_text("".join(f"![w{width}](w{width}.png)\n\n" for width in widths), encoding="utf-8")
        pdf = folder / "images.pdf"
        run(binary, "--export", str(source), str(pdf))
        images = pdf_image_planes(require_pdf(pdf))
        for width, (colors, alphas) in expected.items():
            if width not in images:
                raise AssertionError(f"PNG {name} {width} px wide: no image in the PDF")
            if images[width] != (colors, alphas):
                raise AssertionError(f"PNG {name} {width} px wide: pixels changed in the PDF")


def verify_image_cache(binary: Path, root: Path) -> None:
    """A transparent PNG of 256x256 pixels is kept in the user cache directory under its SHA-256:
    a later export reads it without writing it again, a damaged copy is found out and rewritten,
    and --no-image-cache keeps nothing. The PDF is the same every time."""
    cache = root / "user-cache" / ("RayoMD/cache/images" if os.name == "nt" else "rayomd/images")
    folder = root / "image-cache"
    folder.mkdir()
    rows = [bytes(v for x in range(256) for v in ((x * 3 + y) & 255, (x ^ y) & 255, y & 255, (x + y * 7) & 255))
            for y in range(256)]
    png = png_file(256, 256, 6, rows, b"")
    (folder / "picture.png").write_bytes(png)
    source = folder / "picture.md"
    source.write_text("![picture](picture.png)\n", encoding="utf-8")
    entry = cache / (hashlib.sha256(png).hexdigest() + ".rimg")

    def export(name: str, *flags: str) -> bytes:
        run(binary, "--export", str(source), str(folder / name), *flags)
        return require_pdf(folder / name)

    plain = export("uncached.pdf", "--no-image-cache")
    if cache.exists() and any(cache.iterdir()):
        raise AssertionError("--no-image-cache: an image was kept")
    if export("miss.pdf") != plain or [p.name for p in cache.iterdir()] != [entry.name]:
        raise AssertionError(f"image cache: no single entry {entry.name} after the first export")
    kept = entry.read_bytes()
    inode = entry.stat().st_ino
    if export("hit.pdf") != plain or entry.stat().st_ino != inode:
        raise AssertionError("image cache: a kept image was not read, or was written again")
    middle = len(kept) // 2
    for damage in (kept[:middle] + bytes((kept[middle] ^ 1,)) + kept[middle + 1:], kept[:-9]):
        entry.write_bytes(damage)
        inode = entry.stat().st_ino
        if export("damaged.pdf") != plain or entry.read_bytes() != kept or entry.stat().st_ino == inode:
            raise AssertionError("image cache: a damaged entry was used or not written again")


def verify(binary: Path, keep: Path | None) -> None:
    binary = binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"binary does not exist: {binary}")
    root = keep.resolve() if keep else Path(tempfile.mkdtemp(prefix="rayomd-verify-"))
    root.mkdir(parents=True, exist_ok=True)
    # Images are kept in a cache of this run's own: an earlier build's could hide a broken encoder.
    os.environ["XDG_CACHE_HOME"] = str(root / "user-cache")
    os.environ["LOCALAPPDATA"] = str(root / "user-cache")
    try:
        run(binary, "--version")
        run(binary, "--doctor")
        if os.name == "nt":
            # Exercise CommandLineToArgvW behavior outside the common fast path.
            run_raw_windows(binary, "   --doctor")

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
        numbered_pdf = root / "numbered.pdf"
        run(binary, "--export", str(ascii_md), str(numbered_pdf), "native", "modern", "normal", "--page-numbers")
        require_pdf(numbered_pdf, b"/Contents [", b"(1 / 1) Tj")
        letter_pdf = root / "letter.pdf"
        run(binary, "--export", str(ascii_md), str(letter_pdf), "native", "modern", "normal", "--page-size=letter")
        require_pdf(letter_pdf, b"/MediaBox [0 0 612 792]")
        bad_size_pdf = root / "bad-size.pdf"
        bad_size = run(binary, "--export", str(ascii_md), str(bad_size_pdf), "--page-size=b5", expect=2)
        if b"--page-size must be" not in bad_size.stdout or bad_size_pdf.exists():
            raise AssertionError("an invalid --page-size was not rejected")
        verify_compression(binary, root)
        verify_png_lossless(binary, root)
        verify_image_cache(binary, root)
        verify_theme(binary, root)
        verify_pdfa(binary, root)
        verify_footnotes(binary, root)
        verify_highlight(binary, root)
        verify_contents(binary, root)
        verify_book(binary, root)
        verify_svg(binary, root)
        verify_pdfua(binary, root)
        verify_batch(binary, root)

        if os.name == "nt":
            adjacent_quote_pdf = root / "adjacent-quote.pdf"
            adjacent_quote_pdf.unlink(missing_ok=True)
            prefix = subprocess.list2cmdline(
                [str(binary), "--export", str(ascii_md), str(adjacent_quote_pdf)]
            )
            adjacent = run_raw_windows(binary, prefix + ' "nat""ive"', expect=2)
            if b"unrecognized export option" not in adjacent.stdout or adjacent_quote_pdf.exists():
                raise AssertionError("adjacent quotes did not retain CommandLineToArgvW semantics")

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

        # Native math: formulas are typeset with the PDF standard fonts, currency stays literal,
        # and a document without math gains no math fonts.
        math_md = root / "math.md"
        math_md.write_text(
            "# Math $E=mc^2$\n\nInline $\\frac{a}{b}$, literal $5 and $10.\n\n"
            "$$\n\\int_0^1 x\\,dx = \\frac12\n$$\n\n| f |\n|---|\n| $\\alpha$ |\n",
            encoding="utf-8",
            newline="\n",
        )
        math_pdf = root / "math.pdf"
        run(binary, "--export", str(math_md), str(math_pdf), "native", "modern", "normal")
        math_data = require_pdf(
            math_pdf,
            b"RayoMD Native Standard PDF",
            b"/BaseFont /Symbol",
            b"/BaseFont /Times-Italic",
            b" /M1 ",
            b"$5 and $10",
        )
        if b"\\frac" in math_data or b"\\int" in math_data or b"\\alpha" in math_data:
            raise AssertionError("TeX source leaked into the page content")
        if b"/BaseFont /Symbol" in ascii_pdf.read_bytes() or b"/BaseFont /Times" in ascii_pdf.read_bytes():
            raise AssertionError("math fonts were added to a document without math")

        unicode_math = root / "unicode-math.md"
        unicode_math.write_text("Zażółć $\\text{gęślą} + x^2$\n", encoding="utf-8", newline="\n")
        run(binary, "--export", str(unicode_math), str(root / "unicode-math.pdf"), "native", "modern", "normal")
        require_pdf(root / "unicode-math.pdf", b"RayoMD Native Tiny PDF", b"/BaseFont /Times-Italic")

        run(binary, "--stdin", str(root / "stdin-math.pdf"), "native", "modern", "normal", stdin="$$\nx^2\n$$\n")
        require_pdf(root / "stdin-math.pdf", b"/BaseFont /Times-Italic")

        # Hostile math must terminate with a valid PDF; the timeout is a hang guard, not a benchmark.
        hostile = root / "hostile-math.md"
        hostile.write_text(
            "$$\n" + "{" * 5000 + "\n$$\n\n$$\n" + "\\frac{" * 2000 + "\n$$\n\n" + "$$x\n" * 20000,
            encoding="utf-8",
            newline="\n",
        )
        run(binary, "--export", str(hostile), str(root / "hostile-math.pdf"), timeout=60)
        require_pdf(root / "hostile-math.pdf")

        math_reversible = root / "math-reversible.pdf"
        run(binary, "--export", str(math_md), str(math_reversible), "native", "modern", "normal", "--embed-source")
        run(binary, "--recover-source", str(math_reversible), str(root / "math-recovered.md"))
        if (root / "math-recovered.md").read_bytes() != math_md.read_bytes():
            raise AssertionError("recovered math Markdown is not byte-exact")

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
