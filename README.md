<a id="readme-top"></a>

<div align="center">

<img src="docs/assets/branding/rayomd.png" alt="RayoMD mascot" width="170">

# RayoMD

**Markdown to PDF in milliseconds, from a single small executable.**<br>
No browser. No LaTeX. No Pandoc. No runtime.

<a href="https://github.com/Butterski/rayomd/releases/latest"><img alt="Version: 4.3.1 MathTex" src="https://img.shields.io/badge/version-4.3.1%20MathTex-informational?style=for-the-badge"></a>
<a href="https://github.com/Butterski/rayomd/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/Butterski/rayomd/total?style=for-the-badge&color=2ea44f"></a>
<a href="LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=for-the-badge"></a>

<a href="https://github.com/Butterski/rayomd/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/rayomd/actions/workflows/ci.yml/badge.svg"></a>
<a href="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml/badge.svg"></a>
<a href="https://github.com/Butterski/rayomd/actions/workflows/release.yml"><img alt="Release" src="https://github.com/Butterski/rayomd/actions/workflows/release.yml/badge.svg"></a>
<a href="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml/badge.svg"></a>
<img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
<img alt="Platforms: Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-2ea44f">
<a href="https://github.com/Butterski/rayomd/stargazers"><img alt="GitHub stars" src="https://img.shields.io/github/stars/Butterski/rayomd?style=social"></a>

[**Download**](https://github.com/Butterski/rayomd/releases/latest) ·
[Quick start](#quick-start) ·
[Features](#features) ·
[Benchmarks](#performance) ·
[Documentation](https://github.com/Butterski/rayomd/wiki)

<img src="docs/assets/demo/ui.png" alt="RayoMD Windows app screenshot" width="780">

<sub><a href="docs/assets/demo/show.gif">Watch the demo (GIF)</a> · <a href="docs/assets/demo/show.mp4">MP4</a></sub>

</div>

> [!TIP]
> **New in 4.3:** text in other scripts than Latin (and Latin with characters beyond Windows-1252)
> takes **half the bytes** in the PDF, so such documents come out **up to 26 % smaller** and
> export up to 12 % faster, and documents of plain code blocks export **34 to 42 % faster** in a
> warm process. Since 4.2, RayoMD decodes PNGs with its own inflater, a row at a
> time, on up to four cores, in **70 % less memory** than 4.1.0 for a large one. Text and pixels
> are unchanged. 4.3.1 loads and subsets the TrueType font with less work, so a fresh export of
> a Unicode document runs **up to 8 % faster** than in 4.3.0, with the same bytes. See
> [the numbers](#performance).

RayoMD parses Markdown and writes PDF bytes directly, in its own C++17 code. Windows gets a
compact Dear ImGui app with command-line modes, Linux a small CLI, and both share one exporter
you can also embed through the [C++ API](https://github.com/Butterski/rayomd/wiki/C++-API).

## Highlights

<table>
<tr>
<td width="33%" valign="top">

### Fast
A short note in about **1 ms**, start to exit; a 1024×1024 PNG in 8 to 12 ms, 2 ms once
converted. Batches run on up to 64 threads.

</td>
<td width="33%" valign="top">

### Small
A **643 KB** Linux download and a 1.5 MB Windows app. No browser engine, TeX stack or
runtime to install.

</td>
<td width="33%" valign="top">

### Math
A TeX subset **typeset natively**: fractions, roots, sums, matrices. No LaTeX, no embedded
math fonts.

</td>
</tr>
<tr>
<td valign="top">

### Code and images
Code coloured as on GitHub in **29 languages**; PNG, JPEG, and SVG drawn as vectors.

</td>
<td valign="top">

### Books and themes
One PDF from a `SUMMARY.md`, tables of contents, company fonts, colours, headers and a
cover.

</td>
<td valign="top">

### Standards
Tagged **PDF/UA-1** for screen readers and **PDF/A-3b** for archives, validated with
veraPDF.

</td>
</tr>
</table>

## Quick start

<details open>
<summary><b>Linux</b>: download and run</summary>

```sh
curl -LO https://github.com/Butterski/rayomd/releases/download/v4.3.1/rayomd-4.3.1-linux-x64-static.tar.gz
tar -xzf rayomd-4.3.1-linux-x64-static.tar.gz
./rayomd-4.3.1-linux-x64-static/rayomd --export input.md output.pdf
```

</details>

<details>
<summary><b>Windows</b>: the app and the CLI in one <code>rayomd.exe</code></summary>

Download `rayomd-4.3.1-windows-x64.zip` from
[Releases](https://github.com/Butterski/rayomd/releases/latest) and unpack it. `rayomd.exe`
without arguments opens the app: edit or drop a file, pick a style and margins, and press
`Ctrl+E` to export. With arguments it is the same CLI as on Linux:

```sh
rayomd.exe --export input.md output.pdf
```

</details>

<details>
<summary><b>Build from source</b></summary>

A C and C++17 compiler and CMake; on Linux also a TrueType font such as
`fonts-dejavu-core`. On Windows add `-G "MinGW Makefiles"`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/rayomd --doctor
./build/rayomd --export input.md output.pdf
```

</details>

| Package | For |
|---|---|
| `rayomd-<version>-windows-x64.zip` | Windows: the app and every CLI mode in one `rayomd.exe` |
| `rayomd-<version>-linux-x64.tar.gz` | Linux: needs only glibc |
| `rayomd-<version>-linux-x64-static.tar.gz` | Linux with no dependency at all, the fastest start |
| `rayomd-<version>-linux-x64-curl.tar.gz` | Linux with HTTP/HTTPS images through libcurl |

Every feature below works the same from both command lines and the
[C++ API](https://github.com/Butterski/rayomd/wiki/C++-API).

## Performance

Measured on 2026-10-13: Linux x86-64, g++ 13.3, one pinned CPU core unless said. A one-shot
export is a fresh `rayomd --export` process, start to exit (median of 40 runs).

### Text, 4.3

The Unicode renderer, which draws any text the standard PDF fonts cannot, writes two bytes a
character where it wrote four hex digits; plain code blocks are drawn without a list of strings
per line. One-shot export against 4.2.2 (median of 41):

| Document | 4.2.2 | 4.3.0 | PDF size |
|---|---:|---:|---:|
| [`performance.md`](docs/development/performance.md) (Unicode, 2,000 lines) | 5.07 ms | **4.77 ms** | 960 → **709 KB** |
| This README | 2.41 ms | **2.33 ms** | 252 → **206 KB** |
| 35 KB of C++ in code blocks, Polish comments | 2.34 ms | **2.06 ms** | 287 → **217 KB** |
| The same code, ASCII only | 1.63 ms | **1.48 ms** | 146 KB, as before |

Documents in ASCII or Windows-1252 Latin keep the standard fonts and their exact bytes. Text
extracts and renders the same: 650 PDFs under five option sets pass `qpdf --check` and give
Poppler the same text and pixels as with 4.2.2.

4.3.1 reads the TrueType font's tables directly, keeps its glyph table in memory that starts
zeroed, and writes the font subset once, straight into the PDF. One-shot export against
4.3.0, the same PDF bytes:

| Document | 4.3.0 | 4.3.1 |
|---|---:|---:|
| Short note in Polish | 1.40 ms | **1.28 ms** |
| `tester.md`, no images | 1.89 ms | **1.75 ms** |
| 35 KB of C++ in code blocks, Polish comments | 2.07 ms | **1.91 ms** |
| [`performance.md`](docs/development/performance.md) | 4.86 ms | **4.69 ms** |
| Short note in ASCII (standard fonts) | 0.96 ms | 0.95 ms |

### Images, 4.2

```mermaid
%%{init: {"themeVariables": {"xyChart": {"plotColorPalette": "#2ea44f"}}}}%%
xychart-beta
    title "One 1024×1024 RGBA PNG, one-shot export, in ms"
    x-axis ["3.2.0", "4.0.0", "4.1.0", "4.2.0", "4.2.1", "4.2.1, 2 cores", "4.2.1, seen before"]
    y-axis "Milliseconds" 0 --> 90
    bar [87.6, 27.3, 12.2, 11.4, 11.0, 7.2, 2.2]
```

| Document | 4.1.0 | 4.2.0 | 4.2.1 | 4.2.1, 2 cores | 4.2.1, image seen before |
|---|---:|---:|---:|---:|---:|
| One 1024×1024 RGBA PNG | 12.2 ms | 11.4 ms | **11.0 ms** | **7.2 ms** | **2.2 ms** |
| `tester.md` with that PNG | 13.1 ms | 12.2 ms | **11.9 ms** | **8.0 ms** | **3.0 ms** |
| One 2048×2048 RGBA illustration | 27.7 ms | 24.6 ms | **20.4 ms** | **14.3 ms** | **1.7 ms** |
| One noisy 1024×1024 RGBA photo | 32.7 ms | 33.1 ms | **31.9 ms** | **24.5 ms** | **14.1 ms** |

A PNG with transparency is inflated by RayoMD's own decoder a row at a time, each row is
unfiltered, split into colour and alpha, filtered again (in AVX2 where the CPU has it) straight
into compression segments, and on more than one core helper threads compress the segments
while decoding goes on. The decoder accepts exactly what zlib does; it is faster than
libdeflate's on three of the seven test images, within 1.3 % on a fourth and up to 21 % slower
on the others. 4.2.1 unfilters two Paeth-filtered rows at once and scores filters on bytes, so
images with such rows gain most; the screenshot, which has none, is 2 to 3 % slower than in
4.2.0. Every pixel is unchanged and the streams are within 0.3 % of 4.1.0's. From 32,768 pixels
a converted image is kept in the user cache directory, so its next export, in any process,
skips the work: the last column. `--no-image-cache` turns that off. Documents without such a
PNG give the same PDF bytes as 4.0.0, as fast.

**Memory.** A new image is no longer decoded whole: the decoder keeps the last 32 KB and a
chunk, and at most six 1 MiB compression segments are filled at once. Peak memory of a one-shot
export, one core (RSS):

| Export | 4.1.0, new image | 4.2, new image | 4.2, image seen before |
|---|---:|---:|---:|
| Short note, no image | 2.7 MiB | 2.8 MiB | 2.8 MiB |
| One 1024×1024 RGBA PNG | 11.2 MiB | **8.8 MiB** | 3.8 MiB |
| One 2048×2048 RGBA illustration | 27.8 MiB | **8.4 MiB** | 3.3 MiB |
| One noisy 1024×1024 RGBA photo | 22.3 MiB | **19.9 MiB** | 14.8 MiB |
| 48 documents with a 1024×1024 PNG each, `--batch`, six workers | 58.5 MiB | 60.3 MiB | |

On four cores the helper threads and their buffers add up to 3.8 MiB (the 1024×1024 PNG
12.6 MiB, 4.1.0 11.9 MiB there). A batch keeps every document's streams, which dominate its
memory. The noisy photo's memory is mostly its own PNG and its 2.8 MB stream.

### Text, 4.0

4.0.0 against 3.2.0 (2026-10-12); 4.1.0 and 4.2 render text as fast and with the same bytes.

| Document | 3.2.0 | 4.0.0 | Faster by |
|---|---:|---:|---:|
| Short note, ASCII | 1.69 ms | **1.04 ms** | 38 % |
| Short note, Polish text | 2.53 ms | **1.31 ms** | 48 % |
| Mixed document with formulas (`single_01.md`) | 6.27 ms | **4.14 ms** | 34 % |
| Short Chinese note, drawn in a fallback font | 7.59 ms | **2.67 ms** | 65 % |

Warm exports, repeated in one process with `--bench`, run 16 % faster than 3.2.0 (geometric
mean of the nine watch-suite documents, pinned; documents with formulas up to 25 %), with PDFs
byte for byte those of 3.2.0 on a corpus of 3,353 PDFs. The fully static Linux edition starts
another 0.3 ms sooner: a short ASCII note in 0.68 ms.

| Release asset | 4.0.0 | 4.1.0 | 4.2.0 | 4.2.1 | 4.3.0 | 4.3.1 |
|---|---:|---:|---:|---:|---:|---:|
| `rayomd-<version>-linux-x64.tar.gz` | 604 KB | 635 KB | 643 KB | 645 KB | 646 KB | 645 KB |
| `rayomd-<version>-linux-x64-static.tar.gz` | 1,039 KB | 1,027 KB | 1,039 KB | 1,042 KB | 1,042 KB | 1,042 KB |
| `rayomd-<version>-windows-x64.zip` (GUI and CLI) | 1,444 KB | 1,474 KB | 1,480 KB | 1,482 KB | 1,484 KB | 1,482 KB |

4.1.0 adds libdeflate's decompressor (MIT) and drops zlib, which the static edition linked in;
4.2.0 replaces libdeflate with RayoMD's own decoder.

### Against other tools

The same machine (AMD Ryzen 5 PRO 4650GE, Debian 12 container, no network) converting the
repository's `tester.md` (headings, Unicode text, code, tables, lists, quotes, footnotes and
formulas), each tool as a fresh process, start to exit, median of 20 runs on 2026-10-12.

**Without images**

```mermaid
%%{init: {"themeVariables": {"xyChart": {"plotColorPalette": "#2ea44f"}}}}%%
xychart-beta
    title "tester.md without images, in ms (lower is better)"
    x-axis ["RayoMD", "glu", "Pandoc + Typst", "md-to-pdf", "Pandoc + XeLaTeX"]
    y-axis "Milliseconds" 0 --> 1800
    bar [1.6, 56, 112, 1670, 1708]
```

| Tool | Version | Time | Formulas |
|---|---|---:|---|
| **RayoMD** | 4.0.0 | **1.6 ms** | typeset |
| glu | 0.0.43 | 55.8 ms (35×) | left as TeX source |
| Pandoc + Typst | 3.12.1, Typst 0.15.1 | 112.2 ms (71×) | typeset |
| md-to-pdf | 5.2.5, Chromium 154 | 1,669.8 ms (1,057×) | left as TeX source |
| Pandoc + XeLaTeX | 3.12.1, TeX Live 2022 | 1,708.2 ms (1,081×) | typeset |

In a running process (`--serve`, `--batch`, `--bench`) each further export takes **0.21 ms**.

**With a 1024×1024 PNG**

```mermaid
%%{init: {"themeVariables": {"xyChart": {"plotColorPalette": "#2ea44f"}}}}%%
xychart-beta
    title "tester.md with its PNG, in ms (lower is better)"
    x-axis ["RayoMD", "glu", "Pandoc + Typst", "md-to-pdf", "Pandoc + XeLaTeX"]
    y-axis "Milliseconds" 0 --> 2800
    bar [7.5, 86, 148, 1745, 2652]
```

| Tool | Time |
|---|---:|
| **RayoMD** 4.2.0 | **7.5 ms** |
| glu | 86.5 ms (12×) |
| Pandoc + Typst | 148.5 ms (20×) |
| md-to-pdf | 1,744.8 ms (233×) |
| Pandoc + XeLaTeX | 2,651.8 ms (354×) |

RayoMD 4.2.0 was measured on 2026-10-13 in the same container with `--no-image-cache`, so it
converts the PNG every run, as the other tools do; beside it 4.1.0 took 9.5 ms (4.0.0 took
30.9 ms that day, 27.9 ms on 2026-10-12), and 4.2.0 takes 2.8 ms once the image is in its
cache. Without images 4.2.0 is as fast as 4.1.0 (1.62 against 1.61 ms).

> [!NOTE]
> Both documents are `tester.md` without the five lines that test fallbacks: the remote and
> missing images and a formula with an unknown TeX command. RayoMD shows those as fallback
> text; with them XeLaTeX and glu stop with an error, and Pandoc waits on the network. The
> imageless one also drops the mascot PNG. Pandoc reads
> `markdown+tex_math_single_backslash-raw_tex`, so `\[ … \]` is display math as in RayoMD.
> The tools render different Markdown subsets, so this compares time to a finished PDF, not
> identical output. Details and the command:
> [comparison of 2026-10-12](https://github.com/Butterski/rayomd/wiki/Markdown-to-PDF-Speed-Comparison-2026-10-12).

[`performance.md`](docs/development/performance.md) records how every number was taken and what
each change gained; the warm comparisons come from `python3 tools/benchmark.py ab`. The
[benchmark index](https://github.com/Butterski/rayomd/wiki/Benchmarks) keeps older reports,
comparisons with other tools, and their caveats.

## The 4.x goal: hot code in faster forms

RayoMD 4.0 moved the code where the time goes into lower-level, faster forms, one measured
change at a time, and 4.1 and 4.2 continue it for images: plain C-style loops over raw buffers
where library containers allocated and copied, SSE2 and AVX2 instructions where one operation
can work on 16 or 32 bytes, more cores where work can overlap, data streamed a row at a time
instead of held whole, and a statically linked runtime where process start dominated.

```mermaid
flowchart LR
    A["Profile a real export"] --> B["Rewrite the hot spot:<br/>C-style loop, SIMD, a thread"]
    B --> C{"Same PDF bytes,<br/>or for a re-encoded<br/>image the same pixels?"}
    C -- no --> X["Left out,<br/>recorded with its numbers"]
    C -- yes --> D{"Faster in a<br/>pinned A/B?"}
    D -- no --> X
    D -- yes --> E["Committed and recorded<br/>in performance.md"]
```

- **Images (4.2).** RayoMD's own inflater, a row at a time, instead of libdeflate's whole-image
  one; rows filtered straight into compression segments that up to four cores compress at once,
  in an order that does not change the bytes; AVX2 Adler-32. Encoders with one hash probe or
  fewer insertions were faster but up to 19 % larger on some image, and lost.
- **Images (4.1).** libdeflate's decompressor instead of zlib, RayoMD's own encoder with an image
  mode, colour split from alpha in SSE2, filters chosen in AVX2, the colour compressed on a
  second core while decoding goes on, SHA-NI hashing, and a persistent cache of converted
  images. zlib, libdeflate's and ISA-L's encoders, zlib-ng, Wuffs and fixed filters were
  measured and lost.
- **Text.** Numbers and glyph codes are written from digit-pair tables, a character's width is
  one table lookup, and text with formulas no longer copies every word.
- **Start-up.** No iostreams, the C++ runtime linked in, fonts memory-mapped instead of read,
  and a fully static Linux edition.

What is left in the profiles, and what was tried and rejected, is listed under *Measured
opportunities* in [`performance.md`](docs/development/performance.md).

<p align="right"><a href="#readme-top">back to top ↑</a></p>

## Features

### Markdown

GitHub-flavoured Markdown as people write it. Headings become PDF bookmarks and `#anchor`
targets, and a table's header row repeats on every page it reaches:

```markdown
# Report

Text with **bold**, *italic*, ~~strike~~, `code` and a [link](https://example.com).
A bare URL like https://example.com and an address like team@example.com are clickable.

- [x] Task lists
- Nested bullet and numbered lists
  1. like this one

> [!WARNING]
> GitHub alerts: NOTE, TIP, IMPORTANT, WARNING and CAUTION.

| Item | Price |
|:-----|------:|
| Tea  |  2.50 |

A sentence with a footnote.[^1]

[^1]: Set after the text, linked both ways.
```

Also reference links (full, collapsed and shortcut), `<…>` autolinks, rules, page breaks
(`\pagebreak`, `\newpage`, `<!-- pagebreak -->`), `<br>`, character references such as
`&copy;`, and hidden HTML comments. Unicode text uses an embedded subset of a system font (or
the one `RAYOMD_FONT` names) with fallback fonts for other scripts; Latin text stays on the PDF
standard fonts, which keeps those files small.

### Code highlighting

Fenced code in one of 29 languages is coloured as GitHub's light theme colours it: shell,
console, PowerShell, batch, JavaScript, TypeScript, Python, C, C++, C#, Java, Kotlin, Go, Rust,
Swift, PHP, Ruby, SQL, HTML, XML, CSS, SCSS, JSON, YAML, TOML, INI, Dockerfile and diff.
`--no-highlight` draws every block in one colour.

````markdown
```python
def greet(name):
    return f"Hello, {name}"
```
````

### Math

A TeX subset typeset natively, with no LaTeX and no embedded math fonts, in text, lists, quotes,
headings and table cells. Unknown or malformed input degrades to readable output; see
[`native_math.md`](docs/development/native_math.md).

```markdown
The roots are $x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}$.

$$
\sum_{i=1}^{n} i = \frac{n(n+1)}{2}
$$
```

### Images

```markdown
![Revenue by quarter](charts/revenue.svg)
![Team photo](photos/team.jpg)
```

- PNG and JPEG, sized to the page; a missing or unreadable image shows its alt text, and an
  image inside a paragraph reads `image: alt`.
- A PNG with transparency is re-encoded once: from 32,768 pixels on it is kept in the user
  cache directory (`~/.cache/rayomd/images`, on Windows `%LOCALAPPDATA%\RayoMD\cache\images`;
  at most about 256 MB), so later exports of the same image skip the work. The PDF is the same
  either way; `--no-image-cache` keeps nothing.
- SVG drawn as vectors by RayoMD's own converter: paths, shapes, transforms, clipping, linear
  gradients, opacity, text in the document's font, embedded PNG/JPEG, and Illustrator style
  sheets. Charts from matplotlib, Vega-Lite, Plotly and Graphviz draw close to a browser; an SVG
  that needs scripts or HTML (Mermaid) shows its alt text.
- Local images must lie in the input file's folder unless `--allow-unsafe-local-images`;
  HTTP/HTTPS images load only with `--allow-url-images` (Windows, and the curl Linux build).

### Page layout

Styles `elegant` (default), `modern` and `tech`; margins `compact`, `normal`, `wide`,
`margin=0.75in` or `margin=54pt`; page sizes `a4` (default), `a3`, `a5`, `letter`, `legal`,
each also `-landscape`, or any `WIDTHxHEIGHT` in `mm`, `cm`, `in` or `pt`; "N / M" page
numbers.

```sh
rayomd --export report.md report.pdf modern wide --page-size=letter-landscape --page-numbers
```

### Document metadata

The YAML front matter fills the PDF's metadata and is not shown on the page. Without a `title`
the first heading is the title; `description` stands in for `subject`.

```yaml
---
title: Quarterly report
author: [Ann Smith, Bob Lee]
subject: Sales in Q3
keywords: [sales, q3]
lang: en-GB
---
```

### Tables of contents

A paragraph `[TOC]` or `[[_TOC_]]` becomes a table of contents with dot leaders, page numbers
and links to the headings; `--toc` puts one first, and `--toc-depth=N` (1 to 6, default 3) sets
how many heading levels it lists.

```sh
rayomd --export handbook.md handbook.pdf --toc --toc-depth=2
```

### Themes

`--theme=FILE` lays a company look over a style: your TrueType font, heading, link and accent
colours, a header and footer with placeholders (`{title}`, `{author}`, `{subject}`, `{date}`,
`{page}`, `{pages}`, `{logo}`) and a logo, and a cover page. Paths are relative to the theme
file; see [Themes](https://github.com/Butterski/rayomd/wiki/Themes).

<details>
<summary>Example theme file</summary>

```ini
font = fonts/AcmeSans.ttf
heading-color = #0B3D91
link-color = #0B6E4F
accent-color = #F2A900
header-left = {logo}
header-right = {title}
footer-left = {author}
footer-right = {date}
logo = acme-logo.svg
cover = yes
```

</details>

### Books

`--book` makes one PDF of several files: those a `SUMMARY.md` lists (mdBook and GitBook, with
parts and nesting), a folder's files by name with `README.md` first, or the files given. Each
starts on a new page, links between them (`other.md#heading`) stay links inside the PDF, the
bookmarks nest by part, and `--toc` gives one table of contents for the whole book.

```sh
rayomd --book docs/src/SUMMARY.md manual.pdf --toc --page-numbers
```

### Batch conversion

`--batch` converts a folder on up to 64 threads (6 by default); `--recursive` mirrors its
subfolders, `--skip-unchanged` keeps PDFs newer than their Markdown, and `--report` writes one
JSON line per document (`-` for standard output). `--stdin` converts Markdown piped in,
`--stdin-batch` converts the paths piped in, and `--serve` stays running and converts each path
it reads, answering one line per document.

```sh
rayomd --batch docs pdf-out --recursive --skip-unchanged --report=report.jsonl --workers=8
cat notes.md | rayomd --stdin notes.pdf
```

### Compression, archiving and accessibility

- `--compress` writes page content, fonts and CMaps as FlateDecode streams with RayoMD's own
  DEFLATE encoder: text-heavy PDFs come out two to five times smaller.
- `--pdfa` writes PDF/A-3b for archiving: every font embedded, an sRGB output intent, XMP
  metadata and a file identifier.
- `--pdfua` writes tagged PDF/UA-1 for screen readers: headings, lists, tables with header
  scope, figures with their alt text, formulas, links and footnotes in reading order, decoration
  marked as artifacts; the language is the front matter's `lang`, else `en`.

PDF/A and PDF/UA need a TrueType font to embed and show formulas as their TeX source; together
the file claims both, and every test document passes veraPDF. See
[Archival and Accessible PDFs](https://github.com/Butterski/rayomd/wiki/Archival-and-Accessible-PDFs).

```sh
rayomd --export policy.md policy.pdf --pdfa --pdfua --compress
```

### Reversible PDFs

`--embed-source` attaches the exact Markdown to the PDF and `--recover-source` gets it back byte
for byte; `--inspect-source` describes it.

> [!IMPORTANT]
> Embedding is off by default: the file then carries the whole source, including anything not
> shown on the pages. See [Reversible PDFs](https://github.com/Butterski/rayomd/wiki/Reversible-PDFs).

```sh
rayomd --export notes.md notes.pdf --embed-source
rayomd --recover-source notes.pdf recovered.md
```

### Diagnostics and Pandoc mode

`--doctor` checks fonts, temporary output and a smoke export; `--bench` times repeated exports of
one document; `--version` prints the version. Exit codes are in the
[CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference). On Windows an optional
Pandoc mode hands documents to an installed Pandoc when the native subset is not enough.

```sh
rayomd --doctor
rayomd --bench report.md bench-out 200 modern normal
```

<p align="right"><a href="#readme-top">back to top ↑</a></p>

## Boundaries

> [!NOTE]
> RayoMD's renderer is a fast Markdown subset, not a Pandoc clone. When you need the things
> below, use Pandoc, LaTeX or a browser renderer.

It does not promise full CommonMark/Pandoc compatibility, complete LaTeX math (packages,
macros, automatic numbering), highlighting for every language, citations, filters, templates,
right-to-left scripts (Arabic and Hebrew are drawn left to right, unshaped), complete SVG
(radial gradients and patterns are approximated; markers, filters and masks are not drawn), or
HTML/CSS layout. Under `--pdfua` a theme's cover page is tagged as decoration and an inline
formula is text showing its TeX source. Missing or unsupported images and malformed math degrade
to readable fallbacks instead of failing the export. The complete matrix is in
[Native Markdown Support](https://github.com/Butterski/rayomd/wiki/Native-Markdown-Support).

## Documentation

| Topic | Guide |
|---|---|
| Installation and builds | [Getting Started](https://github.com/Butterski/rayomd/wiki/Getting-Started) |
| Commands, flags and exit codes | [CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference) |
| Folders, reports, incremental runs | [Batch Conversion](https://github.com/Butterski/rayomd/wiki/Batch-Conversion) |
| One PDF of many files | [Books](https://github.com/Butterski/rayomd/wiki/Books) |
| Fonts, colours, headers, cover pages | [Themes](https://github.com/Butterski/rayomd/wiki/Themes) |
| PDF/A-3b and PDF/UA-1 | [Archival and Accessible PDFs](https://github.com/Butterski/rayomd/wiki/Archival-and-Accessible-PDFs) |
| Supported Markdown subset | [Native Markdown Support](https://github.com/Butterski/rayomd/wiki/Native-Markdown-Support) |
| Exact source recovery and privacy | [Reversible PDFs](https://github.com/Butterski/rayomd/wiki/Reversible-PDFs) |
| Embedding the exporter | [C++ API](https://github.com/Butterski/rayomd/wiki/C++-API) |
| Release assets and dependencies | [Packaging and Releases](https://github.com/Butterski/rayomd/wiki/Packaging-and-Releases) |
| Benchmarks and caveats | [Benchmarks](https://github.com/Butterski/rayomd/wiki/Benchmarks) |
| Regression measurement | [Performance Watcher](https://github.com/Butterski/rayomd/wiki/Performance-Watcher) |

Engineering contracts stay beside the code:
[`performance.md`](docs/development/performance.md),
[`native_math.md`](docs/development/native_math.md),
[`reversible_pdf_profile.md`](docs/development/reversible_pdf_profile.md) and
[`arbitrary_pdf_research_decision.md`](docs/development/arbitrary_pdf_research_decision.md).

## Contributing

Read [`AGENTS.md`](AGENTS.md) and [`CONTRIBUTING.md`](CONTRIBUTING.md) first. A change to hot
code comes with before/after measurements and byte-identical output unless changing the output
is its purpose; benchmark claims stay dated and scoped; builds, PDFs and corpora stay out of
the repository. Pandoc is never bundled into the lightweight packages.

<a href="https://github.com/Butterski/rayomd/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=Butterski/rayomd" alt="Contributors">
</a>

## Star history

<a href="https://star-history.com/#Butterski/rayomd&Date">
  <img src="https://api.star-history.com/svg?repos=butterski%2Frayomd&type=Date" alt="Star history chart" width="600">
</a>

## License

RayoMD is released under the Apache License 2.0; see [`LICENSE`](LICENSE). [`NOTICE`](NOTICE)
lists the third-party code and the runtime libraries linked into the release binaries. Dear
ImGui keeps its own license in [`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt).

<p align="right"><a href="#readme-top">back to top ↑</a></p>
