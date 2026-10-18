#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Seed inputs of the fuzz targets (tests/fuzz/fuzz_targets.h).

  seeds.py generate [--rayomd BUILD/rayomd] [--cjpeg cjpeg]
      Writes the synthetic seeds committed under tests/fuzz/seeds/: PNGs of every colour type the
      engine reads, JPEGs (baseline, 4:2:0, progressive, restarts) when cjpeg is given, a TrueType
      and an OpenType/CFF font drawn here (no third-party font data), the CFF table alone, SVGs,
      theme files, Markdown and Mermaid diagrams, and, with --rayomd, PDFs of the reversible
      profile.
  seeds.py collect OUT
      Makes OUT/<target>/ corpora for libFuzzer: the committed seeds plus the repository's
      Markdown (tester.md, docs/**/*.md) and the string literals of the core tests, each Markdown
      file under a few option prefixes of the buildpdf target.

Only the standard library."""

import argparse
import base64
import hashlib
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SEEDS = ROOT / "tests" / "fuzz" / "seeds"


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))


# PNG ---------------------------------------------------------------------------------------------

def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png(width, height, color_type, depth, rows, palette=None, trns=None, interlace=0, split_idat=False):
    raw = b"".join(bytes([index % 5]) + row for index, row in enumerate(rows))
    # Filters 0-4 as written; the samples are small, so a wrong unfilter shows in the pixels.
    data = zlib.compress(raw, 9)
    out = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, interlace))
    if palette:
        out += png_chunk(b"PLTE", palette)
    if trns is not None:
        out += png_chunk(b"tRNS", trns)
    if split_idat:
        half = len(data) // 2
        out += png_chunk(b"IDAT", data[:half]) + png_chunk(b"IDAT", data[half:])
    else:
        out += png_chunk(b"IDAT", data)
    return out + png_chunk(b"IEND", b"")


def make_pngs():
    w, h = 12, 8
    images = {
        "gray.png": png(w, h, 0, 8, [bytes((x * 20 + y * 7) & 255 for x in range(w)) for y in range(h)]),
        "gray1.png": png(16, 4, 0, 1, [bytes([0b10110011, 0b01011100]) for _ in range(4)]),
        "rgb.png": png(w, h, 2, 8, [bytes(v for x in range(w) for v in (x * 20, y * 30, 128)) for y in range(h)], split_idat=True),
        "rgb16.png": png(4, 3, 2, 16, [bytes(v for x in range(4) for v in (x, 0, y, 0, 9, 9)) for y in range(3)]),
        "rgba.png": png(w, h, 6, 8, [bytes(v for x in range(w) for v in (x * 20, y * 30, 200, (x * y * 9) & 255)) for y in range(h)]),
        "graya.png": png(w, h, 4, 8, [bytes(v for x in range(w) for v in (x * 20, 255 - x * 20)) for y in range(h)]),
        "palette.png": png(8, 4, 3, 8, [bytes(range(y, y + 8)) for y in range(4)], palette=bytes(range(36))),
        "palette4trns.png": png(8, 4, 3, 4, [bytes([0x01, 0x23, 0x45, 0x67]) for _ in range(4)],
                                palette=bytes(range(24)), trns=bytes([0, 128, 255])),
        "rgbtrns.png": png(4, 4, 2, 8, [bytes(v for x in range(4) for v in (x, x, x)) for _ in range(4)], trns=b"\x00\x01\x00\x01\x00\x01"),
        "interlaced.png": png(4, 4, 6, 8, [bytes(16) for _ in range(4)], interlace=1),
    }
    for name, data in images.items():
        write(SEEDS / "image" / name, data)
    return images


# JPEG --------------------------------------------------------------------------------------------

def make_jpegs(cjpeg):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        w, h = 24, 16
        gray = b"P5 %d %d 255\n" % (w, h) + bytes((x * 10 + y * 5) & 255 for y in range(h) for x in range(w))
        color = b"P6 %d %d 255\n" % (w, h) + bytes(v for y in range(h) for x in range(w) for v in (x * 10, y * 15, 90))
        (tmp / "gray.pgm").write_bytes(gray)
        (tmp / "color.ppm").write_bytes(color)
        jobs = {
            "gray.jpg": ["-quality", "50", "gray.pgm"],
            "color.jpg": ["-quality", "60", "color.ppm"],
            "color444.jpg": ["-sample", "1x1", "-optimize", "color.ppm"],
            "progressive.jpg": ["-progressive", "color.ppm"],
            "restart.jpg": ["-restart", "1B", "color.ppm"],
        }
        for name, args in jobs.items():
            data = subprocess.run([cjpeg] + args, cwd=tmp, check=True, capture_output=True).stdout
            write(SEEDS / "image" / name, data)


# Fonts -------------------------------------------------------------------------------------------

def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def sfnt(version, tables):
    tags = sorted(tables)
    count = len(tags)
    power = 1 << (count.bit_length() - 1)
    header = struct.pack(">IHHHH", version, count, power * 16, power.bit_length() - 1, count * 16 - power * 16)
    offset = 12 + 16 * count
    directory = b""
    body = b""
    for tag in tags:
        data = tables[tag]
        directory += struct.pack(">4sIII", tag.encode("latin-1"), checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    return header + directory + body


def name_table(family):
    records = [(1, family), (2, "Regular"), (4, family + " Regular"), (6, family.replace(" ", "") + "-Regular")]
    strings = b""
    entries = b""
    for name_id, text in records:
        encoded = text.encode("utf-16-be")
        entries += struct.pack(">HHHHHH", 3, 1, 0x409, name_id, len(encoded), len(strings))
        strings += encoded
    return struct.pack(">HHH", 0, len(records), 6 + 12 * len(records)) + entries + strings


def cmap_table(mapping):
    """Format 4 (3,1) and format 12 (3,10) of {code point: glyph}."""
    bmp = sorted(cp for cp in mapping if cp <= 0xFFFF)
    segments = []
    for cp in bmp:
        if segments and segments[-1][1] == cp - 1 and mapping[cp] - cp == segments[-1][2]:
            segments[-1][1] = cp
        else:
            segments.append([cp, cp, mapping[cp] - cp])
    segments.append([0xFFFF, 0xFFFF, 1])
    n = len(segments)
    power = 1 << (n.bit_length() - 1)
    f4 = b"".join(struct.pack(">H", s[1]) for s in segments) + b"\0\0"
    f4 += b"".join(struct.pack(">H", s[0]) for s in segments)
    f4 += b"".join(struct.pack(">h", ((s[2] + 0x8000) & 0xFFFF) - 0x8000) for s in segments)
    f4 += b"\0\0" * n
    f4 = struct.pack(">HHHHHHH", 4, 14 + len(f4), 0, 2 * n, 2 * power, power.bit_length() - 1, 2 * n - 2 * power) + f4
    groups = [(cp, cp, g) for cp, g in sorted(mapping.items())]
    f12 = struct.pack(">HHIII", 12, 0, 16 + 12 * len(groups), 0, len(groups))
    f12 += b"".join(struct.pack(">III", *group) for group in groups)
    header = struct.pack(">HH", 0, 2) + struct.pack(">HHI", 3, 1, 4 + 16) + struct.pack(">HHI", 3, 10, 4 + 16 + len(f4))
    return header + f4 + f12


def head_table(loca_format, bbox):
    return struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 3, 1000, 0, 0,
                       *bbox, 0, 8, 2, loca_format, 0)


def hhea_table(metrics):
    return struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, 800, -200, 0, max(a for a, _ in metrics), 0, 0, 900,
                       1, 0, 0, 0, 0, 0, 0, 0, len(metrics))


def os2_table():
    data = struct.pack(">HhHHH", 4, 500, 400, 5, 0) + b"\0" * 48
    data += b"FUZZ" + struct.pack(">HHH", 0x40, 0x20, 0xFFFF)
    data += struct.pack(">hhhHH", 800, -200, 90, 1000, 250) + b"\0" * 8 + struct.pack(">hhHHH", 500, 700, 0, 32, 0)
    return data


def post_table():
    return struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 0, 0, 0, 0, 0)


def glyph_simple(contours):
    """A glyph of on-curve and off-curve points: [[(x, y, on), ...], ...]."""
    points = [p for contour in contours for p in contour]
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    out = struct.pack(">hhhhh", len(contours), min(xs), min(ys), max(xs), max(ys))
    end = -1
    for contour in contours:
        end += len(contour)
        out += struct.pack(">H", end)
    out += struct.pack(">H", 0)
    out += bytes(1 if p[2] else 0 for p in points)
    x = y = 0
    dx = b""
    dy = b""
    for px, py, _ in points:
        dx += struct.pack(">h", px - x)
        dy += struct.pack(">h", py - y)
        x, y = px, py
    return out + dx + dy


def glyph_composite(components):
    """[(glyph, dx, dy, scale or None)]."""
    out = struct.pack(">hhhhh", -1, 0, -200, 700, 900)
    for index, (glyph, dx, dy, scale) in enumerate(components):
        flags = 0x0001 | 0x0002 | (0x0020 if index + 1 < len(components) else 0) | (0x0008 if scale else 0)
        if index == 0:
            flags |= 0x0200
        out += struct.pack(">HHhh", flags, glyph, dx, dy)
        if scale:
            out += struct.pack(">h", int(scale * 16384))
    return out


def make_truetype():
    box = glyph_simple([[(50, 0, 1), (550, 0, 1), (550, 700, 1), (50, 700, 1)], [(150, 100, 1), (150, 600, 1), (450, 600, 1), (450, 100, 1)]])
    square = glyph_simple([[(60, 0, 1), (500, 0, 1), (500, 650, 1), (60, 650, 1)]])
    curvy = glyph_simple([[(50, 0, 1), (300, 0, 0), (550, 0, 1), (550, 300, 0), (300, 500, 1), (50, 300, 0)]])
    dot = glyph_simple([[(0, 750, 1), (120, 750, 1), (120, 870, 1), (0, 870, 1)]])
    glyphs = [box, square, curvy, b"", dot,
              glyph_composite([(1, 0, 0, None), (4, 100, 0, None), (4, 300, 0, None)]),   # Ä
              glyph_composite([(2, 0, 0, 0.5), (5, 0, 0, None)])]                           # nested composite
    mapping = {0x20: 3, 0xC4: 5, 0xD6: 6, 0x2D9: 4, 0x1F600: 2}
    for cp in range(0x21, 0x7F):
        mapping[cp] = 1 if cp < 0x60 else 2
    for cp in (0x3B1, 0x3B2, 0x41F, 0x4E2D, 0x2705):
        mapping[cp] = 2
    glyf = b""
    loca = []
    for glyph in glyphs:
        loca.append(len(glyf))
        glyf += glyph + b"\0" * (-len(glyph) % 4)
    loca.append(len(glyf))
    metrics = [(600, 50), (560, 60), (600, 50), (300, 0), (150, 0), (600, 50), (600, 50)]
    tables = {
        "head": head_table(1, (0, -200, 700, 900)),
        "hhea": hhea_table(metrics),
        "maxp": struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(glyphs), 64, 4, 64, 4, 2, 0, 0, 0, 0, 0, 0, 3, 2),
        "OS/2": os2_table(),
        "hmtx": b"".join(struct.pack(">Hh", a, l) for a, l in metrics),
        "cmap": cmap_table(mapping),
        "loca": b"".join(struct.pack(">I", o) for o in loca),
        "glyf": glyf,
        "name": name_table("RayoMD Fuzz Sans"),
        "post": post_table(),
    }
    return sfnt(0x00010000, tables)


def cff_int(value):
    if -107 <= value <= 107:
        return bytes([value + 139])
    if 108 <= value <= 1131:
        value -= 108
        return bytes([(value >> 8) + 247, value & 255])
    if -1131 <= value <= -108:
        value = -value - 108
        return bytes([(value >> 8) + 251, value & 255])
    return b"\x1c" + struct.pack(">h", value)


def dict_int(value):
    return b"\x1d" + struct.pack(">i", value)   # 5 bytes, so offsets do not change the size


def cff_index(items):
    if not items:
        return b"\0\0"
    offsets = [1]
    for item in items:
        offsets.append(offsets[-1] + len(item))
    size = 1 if offsets[-1] < 256 else 2 if offsets[-1] < 65536 else 4
    out = struct.pack(">HB", len(items), size)
    for offset in offsets:
        out += offset.to_bytes(size, "big")
    return out + b"".join(items)


def charstring(*ops):
    out = b""
    for op in ops:
        if isinstance(op, int):
            out += cff_int(op)
        else:
            out += op
    return out


def make_cff():
    RMOVETO, RLINETO, HLINETO, VLINETO, RRCURVETO, CALLSUBR, RETURN, ENDCHAR, CALLGSUBR, HSTEM, HINTMASK = (
        b"\x15", b"\x05", b"\x06", b"\x07", b"\x08", b"\x0a", b"\x0b", b"\x0e", b"\x1d", b"\x01", b"\x13")
    local_subrs = [charstring(0, 650, RLINETO, RETURN)]
    global_subrs = [charstring(440, HLINETO, RETURN)]
    glyphs = [
        charstring(600, 50, 0, RMOVETO, 500, HLINETO, 700, VLINETO, -500, HLINETO, ENDCHAR),        # .notdef, with width
        charstring(60, 0, RMOVETO, -107, CALLGSUBR, -107, CALLSUBR, -440, HLINETO, ENDCHAR),        # subroutines
        charstring(0, 100, HSTEM, 50, 0, RMOVETO, HINTMASK, b"\x80", 200, 0, 250, 300, 0, 200, RRCURVETO,
                   -250, 0, -250, 0, 0, -200, RRCURVETO, ENDCHAR),                                   # hints, curves
        charstring(ENDCHAR),                                                                        # space
        charstring(0, 750, RMOVETO, 120, HLINETO, 120, VLINETO, -120, HLINETO, ENDCHAR),
    ]
    names = [b"RayoMDFuzzCFF"]
    strings = []

    def build(private_offset, charstrings_offset, private_size):
        top = (dict_int(0) + dict_int(-200) + dict_int(700) + dict_int(900) + b"\x05"
               + dict_int(charstrings_offset) + b"\x11" + dict_int(private_size) + dict_int(private_offset) + b"\x12")
        return top

    private = cff_int(0) + b"\x14" + cff_int(0) + b"\x15" + dict_int(len(cff_int(0) + b"\x14" + cff_int(0) + b"\x15") + 6) + b"\x13"
    header = b"\x01\x00\x04\x04"
    top_size = len(cff_index([build(0, 0, 0)]))
    prefix = header + cff_index(names)
    after_top = len(prefix) + top_size
    strings_index = cff_index(strings)
    global_index = cff_index(global_subrs)
    charstrings_offset = after_top + len(strings_index) + len(global_index)
    charstrings_index = cff_index(glyphs)
    private_offset = charstrings_offset + len(charstrings_index)
    top = cff_index([build(private_offset, charstrings_offset, len(private))])
    assert len(top) == top_size
    return prefix + top + strings_index + global_index + charstrings_index + private + cff_index(local_subrs), len(glyphs)


def make_opentype(cff, glyph_count):
    metrics = [(600, 50), (560, 60), (600, 50), (300, 0), (150, 0)]
    mapping = {0x20: 3, 0x2D9: 4, 0x4E2D: 2}
    for cp in range(0x21, 0x7F):
        mapping[cp] = 1 if cp < 0x60 else 2
    tables = {
        "CFF ": cff,
        "head": head_table(0, (0, -200, 700, 900)),
        "hhea": hhea_table(metrics),
        "maxp": struct.pack(">IH", 0x00005000, glyph_count),
        "OS/2": os2_table(),
        "hmtx": b"".join(struct.pack(">Hh", a, l) for a, l in metrics),
        "cmap": cmap_table(mapping),
        "name": name_table("RayoMD Fuzz CFF"),
        "post": post_table(),
    }
    return sfnt(0x4F54544F, tables)


# Text seeds --------------------------------------------------------------------------------------

SVG_BASIC = """<?xml version="1.0" encoding="UTF-8"?>
<!-- Every feature RayoMD's converter draws, once. -->
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="120" viewBox="0 0 200 120">
  <style>.accent { fill: #0b3d91; stroke-width: 2 } #t { font-weight: bold } rect.box { opacity: 0.8 }</style>
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="0" gradientTransform="rotate(10)">
      <stop offset="0" stop-color="red"/><stop offset="0.5" stop-color="#0f0" stop-opacity="0.5"/><stop offset="1" stop-color="rgb(0,0,255)"/>
    </linearGradient>
    <clipPath id="c"><circle cx="50" cy="50" r="40"/></clipPath>
    <g id="mark"><path d="M0 0 L10 0 L5 8 Z"/></g>
  </defs>
  <rect class="box" x="5" y="5" width="90" height="50" rx="6" fill="url(#g)" stroke="black" stroke-dasharray="4 2"/>
  <g transform="translate(100 10) scale(0.8) skewX(10)" clip-path="url(#c)">
    <circle cx="40" cy="40" r="30" class="accent" fill-opacity="0.5"/>
    <ellipse cx="60" cy="30" rx="20" ry="10" fill="none" stroke="#333" stroke-linejoin="round" stroke-linecap="square"/>
  </g>
  <path d="M10 80 h20 v10 H10 z m40 0 c10 -10 20 10 30 0 s10 10 20 0 q5 -10 10 0 t10 0 a10 5 30 1 0 15 5 l5 5" fill="none" stroke="purple" fill-rule="evenodd"/>
  <polyline points="120,80 130,90 140,80 150,90" fill="none" stroke="orange" stroke-width="3" stroke-miterlimit="2"/>
  <polygon points="160,80 180,80 170,100"/>
  <line x1="0" y1="118" x2="200" y2="118" stroke="gray"/>
  <use xlink:href="#mark" x="180" y="10"/>
  <text id="t" x="10" y="70" font-size="12" font-family="sans-serif" text-anchor="start">SVG text <tspan dy="2" fill="red">span</tspan> é中</text>
  <text x="190" y="110" text-anchor="end" transform="rotate(-5 190 110)">end</text>
</svg>
"""

THEMES = {
    "full.theme": """# Every key ParseTheme knows.
font = tiny.ttf
font-bold = tiny.otf
font-italic = tiny.ttf
font-bold-italic = tiny.otf
logo = rgba.png
heading-color = #0B3D91
link-color = #c00
accent-color = "#123456"
header-left = {title}
header-center = "{logo}"
header-right = {author} · {date}
footer-left = {subject}
footer-center = {page} / {pages}
footer-right = Confidential — ünïcödé 中文
cover = yes
""",
    "minimal.theme": "; a comment\nheading-color = #000000\ncover = no\n",
    "logo.theme": "logo = basic.svg\nheader-center = {logo}\nfooter-center = Page {page} of {pages}\ncover = true\n",
}

MARKDOWN = {
    "images.md": """# Images

![rgba](rgba.png) ![rgb](rgb.png) ![gray](gray.png)

![palette](palette4trns.png "a title")

![jpeg](color.jpg)

![svg](basic.svg) ![svg with an image](image.svg)

![sub](sub/graya.png) ![outside](../outside.png) ![absolute](/etc/passwd) ![missing](missing.png)

![url](https://example.com/x.png) ![data](data:image/png;base64,AAAA)

<p align="center"><img src="rgb.png" width="40"> <img src="gray.jpg" height="20" alt="g"></p>

[![linked](rgba.png)](https://example.com)

[logo]: rgba.png
![ref][logo]
""",
    "features.md": """---
title: Fuzz features
author: RayoMD
lang: de
---

[TOC]

# Heading *one*

## Heading `two` {#custom}

Paragraph with **bold**, *italic*, ~~strike~~, `code`, <u>under</u>, <mark>mark</mark>, H<sub>2</sub>O, x<sup>2</sup>,
a [link](https://example.com "t"), <https://auto.example>, www.example.com, mail@example.com, &copy; &rarr; &#x1F600;.
A footnote[^1] and a [reference][ref] and [shortcut].

[^1]: The note, with **bold**.
[ref]: https://example.com/ref
[shortcut]: #heading-one

> [!NOTE]
> An alert with a list:
> - a
>   - b

1. one
2. two
   - [ ] task
   - [x] done

| Left | Center | Right |
|:-----|:------:|------:|
| a | `b` | $x^2$ |
| ✅ done | ❌ no | ⚠️ |

```python
def f(x):  # comment
    return "s" + str(x)
```

```js
const a = `t${1}`; // c
```

```
plain
```

Inline math $\\frac{a}{b} + \\sqrt{x}$ and display:

$$
\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2} \\quad \\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}
$$

<details><summary>Sum</summary>

Hidden *text*.

</details>

<div align="right">Right aligned</div>

<!-- comment -->

---

Line<br>break and a hard  
break.

<table><tr><th>H</th></tr><tr><td>cell</td></tr></table>

<a id="anchor"></a>
[to anchor](#anchor)
""",
    "diagrams.md": """# Diagrams

```mermaid
flowchart LR
    A[Start] -->|yes| B{Choice}
    B --> C((End))
    subgraph S [Group]
        D --> E
    end
    C -.-> D
```

> ```mermaid
> sequenceDiagram
>     Alice->>+Bob: Hi
>     Bob-->>-Alice: Hello
> ```

- ```mermaid
  pie title Share
      "A" : 2
      "B" : 3
  ```

```mermaid
xychart-beta
    bar [1, 2]
```

```mermaid
graph TD
    a --
```
""",
    # Ends in "Z": Obsidian's dialect. Its embeds name the notes of NOTES, which the harness copies
    # into the scratch folder that holds the document (fuzz_env.cpp).
    "embeds.md": """# Embeds

![[note-a]]

Text before
![[note-b#Part two]]
![[note-b#^blk]]

- item
  ![[note-a]]

![[note-cycle]]

![[missing]] and [linked](note-a.md) and [[note-b]].
Z""",
    # Ends in "Z" (0x5A): Obsidian's dialect, heading numbers 3 deep, a page before each h2 (fuzz_public.cpp).
    "obsidian.md": """---
title: Vault
---

# Note

## Part ==one==

> [!tip]- A *custom* title
> Text with [[Other#Part|alias]], [[#Part one]], [[#^block]] and ==mark %%hidden%%==.
> > [!bug]
> > Nested.

> [!quote] Title only

%%
A comment over lines with [[links]]
%%

![[rgba.png|40]]

Inline ![[gray.jpg|20x10]] and ![[Other]] and `[[code]]` and $a==b$.

| A | B |
|---|---|
| [[x\\|y]] | ==z== |

```
[[fenced]] ==text==
```

[[_TOC_]]

%%unclosed
Z""",
    # Starts with "#" (0x23: black text and hidden tags) and ends in "Z" (Obsidian's dialect).
    "tags.md": """#start tags #a/b-c_d #1984 #y1984 a#b http://x/#frag
# Heading #h

- [ ] task #todo and `#code` and $#math$ and [[note|alias #in]] %% #hidden %%
    #indented after text

    #indented code

```
#fenced
```

$$
#display
$$

#\u017Cółw #a #b
%% #open
#inside
%% #after ==mark== [[link]] <span style="color:#c39">c</span>
Z""",
}

# Notes that embeds.md embeds (note embeds, embeds.h), copied into the buildpdf target's scratch
# folder beside its document.
NOTES = {
    "note-a.md": "---\ntags: a\n---\n# Note A\n\nText[^1] with ![rgba](rgba.png).\n\n[^1]: A's note.\n",
    "note-b.md": "# B\n\n## Part one\n\nOne.\n\n## Part two\n\nTwo ==marked==.\n\nBlock text ^blk\n\n# After\n",
    "note-cycle.md": "Cycle\n\n![[note-cycle-2]]\n",
    "note-cycle-2.md": "Back\n\n![[note-cycle]]\n\n![[doc]]\n",
}

OPTION_PREFIXES = [b"\x01\x00", b"\x00\x01", b"\x87\x10", b"\x25\x22", b"\x40\x04", b"\x13\x08", b"\xf2\x51", b"\x00\xc3"]


def make_text_seeds(images):
    write(SEEDS / "svg" / "basic.svg", SVG_BASIC)
    image_svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20">'
                 '<image href="data:image/png;base64,%s" width="20" height="20"/>'
                 '<image x="20" href="data:image/png;base64,%s" width="20" height="20" preserveAspectRatio="none"/></svg>\n'
                 % (base64.b64encode(images["rgba.png"]).decode(), base64.b64encode(images["palette.png"]).decode()))
    write(SEEDS / "svg" / "image.svg", image_svg)
    for name, text in THEMES.items():
        write(SEEDS / "theme" / name, text)
    for name, text in NOTES.items():
        write(SEEDS / "notes" / name, text)
    for name, text in MARKDOWN.items():
        for index, prefix in enumerate(OPTION_PREFIXES[:4]):
            write(SEEDS / "buildpdf" / ("%s.%d" % (name, index)), prefix + text.encode("utf-8"))


# Mermaid -----------------------------------------------------------------------------------------
# Written here for the mermaid target, one per form RayoMD draws (docs/development/native_mermaid.md),
# and a few it refuses; none copied from Mermaid's documentation.

MERMAID = {
    "flow-shapes.mmd": """flowchart TD
    intake[Intake form] --> triage(Triage)
    triage --> queue([Queue]) --> worker[[Worker pool]]
    worker --> store[(Results)] --> hub((Hub))
    hub --> gate{Approved?}
    gate -->|yes| pack{{Package}}
    gate -->|no| redo[/Redo step/]
    redo --> back[\\Send back\\]
    pack --> ship[/Ship\\] --> done[\\Done/]
""",
    "flow-links.mmd": """graph LR
    a --- b
    a -.-> c
    a -.- d
    a ==> e
    a === f
    a --x g
    a --o h
    a <--> i
    j x--x k
    l o--o m
    n ~~~ o
    p ----> q
    r -..-> s
    t -- words --> u
    v == heavy ==> w
    x -. light .-> y
    y -->|"a | bar"| z
    z --> z
""",
    "flow-chains.mmd": """flowchart BT
    %% chains and groups
    s1 & s2 & s3 --> mid --> e1 & e2
    mid --> mid2["quoted [with] (brackets)"] --> end1["one<br>two<br/>three"]
    end1 -->|back| s1;
""",
    "flow-subgraphs.mmd": """flowchart RL
    outside --> inner1
    subgraph box1 [First box]
        direction TB
        inner1 --> inner2
        subgraph nest [Nested]
            deep1 --> deep2
        end
    end
    subgraph box2
        direction LR
        p1 --> p2
    end
    box1 --> box2
    deep2 --> p1
    p2 --> outside
""",
    "flow-styles.mmd": """flowchart TD
    a:::warm --> b --> c
    classDef warm fill:#fc9,stroke:#963,stroke-width:3px
    classDef cool,calm fill:rgb(200, 220, 255),color:#123
    classDef default stroke-dasharray: 4 2
    class b cool
    style c fill:#efe,stroke:#393,stroke-width:2px,color:#030
    linkStyle 0 stroke:#f00
    click a callback
    accTitle: Styles
    accDescr { two
    lines }
""",
    "flow-front-matter.mmd": """---
title: Ignored title
---
%%{init: {"theme": "forest"}}%%
flowchart LR
    left --> right
""",
    "seq-basic.mmd": """sequenceDiagram
    title Checkout
    autonumber
    participant B as Browser
    actor U as Shopper
    participant API
    U->>B: Click buy
    B->>+API: POST /orders
    API-->>-B: 201 Created
    B-)U: Thank you
    B->API: plain
    B-->API: dotted
    B-xAPI: cross
    B--xAPI: dotted cross
    API--)B: async
    API->>API: self check
    activate API
    deactivate API
""",
    "seq-frames.mmd": """sequenceDiagram
    loop Each retry
        A->>B: try
        alt Accepted
            B-->>A: ok
        else Rejected
            B-->>A: no
        end
    end
    opt Cache
        A->>C: store
    end
    par First
        A->>B: one
    and Second
        A->>C: two
    end
    critical Lock
        B->>C: hold
    option Timeout
        B-xC: drop
    end
    break Shutdown
        C->>A: bye
    end
    rect rgba(255, 200, 0, 0.2)
        Note over A,C: Shared note
    end
    Note left of A: left
    Note right of C: right<br>side
    Note over B: over
    autonumber 10 5
    A->>B: numbered
    autonumber off
""",
    "pie-basic.mmd": """pie showData title Weekly hours
    "Meetings" : 12.5
    "Coding" : 22
    "Review" : 6
    "Idle" : 0
""",
    "pie-statements.mmd": """pie
    title Single source
    "Everything" : 1
""",
    "refused-class.mmd": """classDiagram
    class Widget
""",
    "refused-open.mmd": """flowchart TD
    subgraph never closed
    a --> b
""",
}


def make_mermaid_seeds():
    for name, text in MERMAID.items():
        write(SEEDS / "mermaid" / name, text)


def make_pdfs(rayomd):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        source = "# Reversible\n\nA source with *emphasis*, ünïcödé and a table:\n\n| a | b |\n|---|---|\n| 1 | 2 |\n"
        (tmp / "doc.md").write_text(source, encoding="utf-8")
        # Named without ".pdf": the repository tracks no PDF files (hygiene.yml).
        variants = {
            "embedded": ["--embed-source"],
            "embedded-compressed": ["--embed-source", "--compress"],
            "embedded-pdfa": ["--embed-source", "--pdfa"],
            "plain": [],
        }
        for name, flags in variants.items():
            output = tmp / (name + ".pdf")
            subprocess.run([rayomd, "--export", str(tmp / "doc.md"), str(output), "--no-image-cache"] + flags, check=True)
            write(SEEDS / "pdfsource" / name, output.read_bytes())


def generate(args):
    images = make_pngs()
    if args.cjpeg:
        make_jpegs(args.cjpeg)
    write(SEEDS / "font" / "tiny.ttf", make_truetype())
    cff, count = make_cff()
    write(SEEDS / "font" / "tiny.otf", make_opentype(cff, count))
    write(SEEDS / "cff" / "tiny.cff", struct.pack(">H", count) + cff)
    make_text_seeds(images)
    make_mermaid_seeds()
    if args.rayomd:
        make_pdfs(args.rayomd)


# Collect -----------------------------------------------------------------------------------------

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPE = re.compile(r"\\(x[0-9A-Fa-f]{1,2}|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8}|[0-7]{1,3}|.)", re.S)
SIMPLE_ESCAPES = {"n": b"\n", "t": b"\t", "r": b"\r", "a": b"\a", "b": b"\b", "f": b"\f", "v": b"\v"}


def unescape(literal):
    """The bytes of a C++ string literal's text: UTF-8 text, \\x bytes, \\u code points."""
    out = b""
    at = 0
    for match in ESCAPE.finditer(literal):
        out += literal[at:match.start()].encode("utf-8")
        at = match.end()
        escape = match.group(1)
        if escape[0] == "x":
            out += bytes([int(escape[1:], 16)])
        elif escape[0] in "uU" and len(escape) > 1:
            out += chr(int(escape[1:], 16)).encode("utf-8", "surrogatepass")
        elif escape[0] in "01234567":
            out += bytes([int(escape, 8) & 255])
        else:
            out += SIMPLE_ESCAPES.get(escape, escape.encode("utf-8"))
    return out + literal[at:].encode("utf-8")


def literals(path):
    """The longer C++ string literals of a test file, adjacent ones joined, escapes decoded."""
    out = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for statement in re.split(r";|\n\s*\n", text):
        found = LITERAL.findall(statement)
        if not found:
            continue
        value = b"".join(unescape(literal) for literal in found)
        if len(value) >= 12:
            out.append(value)
    return out


def collect(args):
    out = pathlib.Path(args.out)
    for folder in SEEDS.iterdir():
        if folder.is_dir():
            (out / folder.name).mkdir(parents=True, exist_ok=True)
            for item in folder.iterdir():
                shutil.copy(item, out / folder.name / item.name)
    # The image and SVG seeds are fonts' and SVG's too: svg gets the SVGs, font the fonts.
    markdown = [ROOT / "tester.md"] + sorted((ROOT / "docs").rglob("*.md"))
    documents = [path.read_bytes() for path in markdown]
    for test in sorted((ROOT / "tests").glob("*.inc")) + [ROOT / "tests" / "core_tests.cpp"]:
        documents += literals(test)
    seen = set()
    for index, document in enumerate(documents):
        digest = hashlib.sha1(document).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        prefix = OPTION_PREFIXES[index % len(OPTION_PREFIXES)]
        write(out / "buildpdf" / ("doc-%s" % digest[:16]), prefix + document)
        if b"<svg" in document:
            write(out / "svg" / ("doc-%s" % digest[:16]), document[document.find(b"<svg"):])
    for document in markdown:
        for prefix in OPTION_PREFIXES:
            write(out / "buildpdf" / ("%s-%s" % (document.stem, prefix.hex())), prefix + document.read_bytes())
    count = sum(1 for _ in out.rglob("*") if _.is_file())
    print("collected %d seeds into %s" % (count, out))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--rayomd")
    gen.add_argument("--cjpeg")
    col = sub.add_parser("collect")
    col.add_argument("out")
    args = parser.parse_args()
    return generate(args) if args.command == "generate" else collect(args)


if __name__ == "__main__":
    sys.exit(main())
