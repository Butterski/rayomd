# RayoMD

<p align="center">
  <a href="https://github.com/Butterski/rayomd/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/rayomd/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/release.yml"><img alt="Release" src="https://github.com/Butterski/rayomd/actions/workflows/release.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml/badge.svg"></a>
  <img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg">
  <img alt="Version: 3.2.0 MathTex" src="https://img.shields.io/badge/version-3.2.0%20MathTex-informational">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms: Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-2ea44f">
</p>

RayoMD is a tiny native Markdown-to-PDF converter built for fast startup,
small releases, and predictable deployment.

<p align="center">
  <strong><a href="https://github.com/Butterski/rayomd/releases">Download</a></strong>
  ·
  <strong><a href="https://github.com/Butterski/rayomd/wiki">Documentation</a></strong>
</p>

<p align="center">
  <img src="docs/assets/branding/rayomd.png" alt="RayoMD mascot" width="180">
</p>

<p align="center">
  <img src="docs/assets/demo/ui.png" alt="RayoMD Windows app screenshot" width="780">
</p>

<p align="center">
  <img src="docs/assets/demo/show.gif" alt="RayoMD Windows app demo" width="780">
  <br><a href="docs/assets/demo/show.mp4">Watch the MP4 demo</a>
</p>

The default renderer parses Markdown and writes PDF bytes directly from C++17.
It does not start a browser, bundle a runtime, or require Pandoc or LaTeX.
Windows ships a compact Dear ImGui app with CLI modes; Linux uses the same
native exporter through a small CLI.

## Why RayoMD

- Native PDF generation with no browser engine on the fast path.
- A single Windows GUI executable and a compact Linux CLI.
- Bounded parallel batch conversion, stdin, warm serve, and benchmark modes.
- Unicode, clickable links (also bare URLs), bookmarks, tables, task lists, GitHub alerts, footnotes (as on GitHub, at the end of the text), code highlighted as on GitHub in 29 languages and formats, local images (also SVG, drawn as vectors), and opt-in URL images.
- Natively typeset math for a TeX subset: no LaTeX, no browser, no embedded math fonts.
- Optional exact Markdown recovery through reversible PDFs.
- Tables of contents (`[TOC]` or `--toc`) with dot leaders, page numbers and links to the headings.
- Books (`--book`): one PDF of the files a `SUMMARY.md` (mdBook, GitBook) or a folder lists, each from a new page, with the links between them, one outline and one table of contents.
- Optional compression (`--compress`) with a built-in DEFLATE encoder: text-heavy PDFs come out two to five times smaller.
- Company themes (`--theme=FILE`): your TrueType font, colours, header and footer text with a logo, and a cover page.
- PDF/A-3b archives (`--pdfa`): every font embedded, sRGB output intent and XMP metadata; formulas show their TeX source.
- Tagged PDF/UA-1 for screen readers (`--pdfua`): headings, lists, tables, figures with their alt text, links and footnotes in reading order, decoration marked as artifacts; validated with veraPDF, also together with `--pdfa`.
- Optional Windows Pandoc mode when the native subset is not enough.

Use native RayoMD for simple reports and bulk conversion when startup time,
package size, and dependency count matter. Use Pandoc, a browser renderer, or
LaTeX when you need full CommonMark/Pandoc extensions, complete LaTeX math, filters,
templates, citations, highlighting for every language, or HTML/CSS fidelity.

## Performance snapshot

On 2026-07-18, order-balanced tests measured RayoMD 2.6 against the repository's
`tester.md` with and without image syntax:

| Platform/storage | Warm: imageless | Warm: image-bearing | Fresh process: imageless | Fresh process: image-bearing |
|---|---:|---:|---:|---:|
| Windows 11 workspace | **`0.447 ms`** | `0.714 ms` | **`21.10 ms`** | `98.39 ms` |
| Linux WSL `/mnt/e` | **`0.225 ms`** | `5.120 ms` | **`25.45 ms`** | `153.63 ms` |

A fresh process includes startup, source reading, PDF generation, output
writing, and exit. The existing Python runner measured 32 A/B pairs after six
preflights per case; filesystem and OS caches may remain warm. Even with the
image-bearing fixture, the median stayed below `100 ms` on Windows and below
`160 ms` on WSL's Windows-mounted workspace.

Warm `--bench` medians exclude startup, input reading, and timed output writing.
They use 10 A/B pairs (`1,000` builds per Windows round and `200` per WSL
round). The image-bearing fixture reuses the local mascot; URL fetching stayed
off, so remote and missing sources used deterministic fallbacks. Every repeated
PDF was byte-identical within its platform and case.

The WSL result includes `/mnt/e` path and file-access costs and is not a
Linux-native/ext4 claim.

See the [2.6 performance snapshot](https://github.com/Butterski/rayomd/wiki/RayoMD-2.6.0-Performance-Snapshot)
for p95 values, ranges, hashes, fixture details, and reproduction commands. The
[benchmark index](https://github.com/Butterski/rayomd/wiki/Benchmarks) keeps
cross-tool, warm-path, reversible-PDF, and older release reports organized.

On 2026-10-12, builds of 3.2.0 against 3.1.0 on Linux (DejaVu Sans, native storage, the
nine watch-suite documents) matched within about 1 %: warm `--bench` +0.5 % with pinned
alignment (+0.3 % instructions), a cold `tester.md` export per process 4.6 % faster, and a
6-worker `--batch` of 360 documents in tmpfs 1.4 % slower. Without the new options the PDFs
are the same except code-block tints, which no longer overlap the line above. The Linux
executable grew from 683 KB to 1,010 KB with the features 3.2 adds.
[`performance.md`](docs/development/performance.md) records what each change cost.

## Quick start

Download a package from [Releases](https://github.com/Butterski/rayomd/releases),
or build from source.

Windows:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
build/windows/rayomd.exe
```

Linux/WSL:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
./build/linux/rayomd --doctor
./build/linux/rayomd --export input.md output.pdf
```

The Windows app supports editing, drag-and-drop, engine/style/margin selection,
URL-image opt-in, reversible-PDF inspection, and `Ctrl+E` export. Every feature below
works from both command lines (`rayomd` on Linux, `rayomd.exe` on Windows) and from the
[C++ API](https://github.com/Butterski/rayomd/wiki/C++-API).

## Features

### Markdown

- Headings (ATX and Setext) become PDF bookmarks and the targets of GitHub-style
  `#anchor` links; a heading never ends a page.
- Paragraphs, emphasis (bold, italic, strikethrough), code spans and backslash escapes.
- Nested bullet and numbered lists, and GitHub task lists (`- [ ]`, `- [x]`) with checkboxes.
- Block quotes, and GitHub alerts (`> [!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`,
  `[!CAUTION]`) with a coloured bar and title.
- Pipe tables with alignment and inline Markdown in cells; the header row repeats on every
  page a table reaches.
- Links: inline, reference-style (full, collapsed and shortcut `[label]`), `<…>` autolinks,
  and bare URLs and email addresses, all clickable.
- Footnotes as on GitHub: `[^label]` references and `[^label]:` definitions (also in quotes
  and list items), numbered by first reference, set after the text with links both ways.
- Rules, page breaks (`\pagebreak`, `\newpage`, `<!-- pagebreak -->`), hidden HTML comments,
  `<br>`, and character references such as `&copy;`.
- Unicode text in an embedded subset of a system font (or the one `RAYOMD_FONT` names), with
  fallback fonts for scripts it lacks; Latin text stays on the PDF standard fonts, which
  keeps those files small.

### Code highlighting

Fenced code whose info string names one of 29 languages is coloured as GitHub's light theme
colours it: shell and console, PowerShell, batch, JavaScript, TypeScript, Python, C, C++, C#,
Java, Kotlin, Go, Rust, Swift, PHP, Ruby, SQL, HTML, XML, CSS, SCSS, JSON, YAML, TOML, INI,
Dockerfile and diff. `--no-highlight` draws every block in one colour.

### Math

A TeX subset typeset natively, with no LaTeX and no embedded math fonts: `$...$`, `$$...$$`
and fenced `math` blocks, in text, lists, quotes, headings and table cells. Unknown or
malformed input degrades to readable output. See
[`native_math.md`](docs/development/native_math.md).

### Images

- Standalone PNG and JPEG images, sized to the page; a missing or unreadable one shows its
  alt text, and an image inside a paragraph reads `image: alt`.
- SVG images drawn as vectors by RayoMD's own converter: paths, shapes, transforms, clipping,
  linear gradients, opacity, text in the document's font, embedded PNG/JPEG, and style
  sheets as Illustrator writes them. Charts from matplotlib, Vega-Lite, Plotly and Graphviz
  draw close to a browser; an SVG that needs scripts or HTML (Mermaid) shows its alt text.
- Local images must lie in the input file's folder unless `--allow-unsafe-local-images`;
  HTTP/HTTPS images load only with `--allow-url-images` (Windows, and the curl Linux build).

### Page layout

- Styles `elegant` (default), `modern` and `tech`; margins `compact`, `normal`, `wide`, or a
  size such as `margin=0.75in` or `margin=54pt`.
- `--page-size=SIZE`: `a4` (default), `a3`, `a5`, `letter` or `legal`, each also with
  `-landscape`, or any `WIDTHxHEIGHT` in `mm`, `cm`, `in` or `pt` (`6x9in`).
- `--page-numbers` writes "N / M" at the foot of every page.

```sh
rayomd --export report.md report.pdf modern wide --page-size=letter-landscape --page-numbers
```

### Document metadata

The YAML front matter fills the PDF's metadata and is not shown on the page:

```yaml
---
title: Quarterly report
author: [Ann Smith, Bob Lee]
subject: Sales in Q3
keywords: [sales, q3]
lang: en-GB
---
```

Without a `title`, the first heading is the title; `description` stands in for `subject`.

### Tables of contents

A paragraph `[TOC]` or `[[_TOC_]]` becomes a table of contents with dot leaders, page
numbers and links to the headings; `--toc` puts one first (after a title heading).
`--toc-depth=N` (1 to 6, default 3) sets how many heading levels it lists.

### Themes

`--theme=FILE` lays a company look over a style: your TrueType font for all text, heading,
link and accent colours, a header and footer with placeholders and a logo, and a cover page.

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

Placeholders are `{title}`, `{author}`, `{subject}`, `{date}`, `{page}`, `{pages}` and
`{logo}`; paths are relative to the theme file. See
[Themes](https://github.com/Butterski/rayomd/wiki/Themes).

### Books

`--book` makes one PDF of several Markdown files: those a `SUMMARY.md` lists (as mdBook and
GitBook read it, with parts and nesting), a folder's files by name with `README.md` first,
or the files given. Each file starts on a new page and keeps its own footnotes and anchors,
links between the files (`other.md#heading`) stay links inside the PDF, the bookmarks nest
by part, and `--toc` gives one table of contents for the whole book.

```sh
rayomd --book docs/src/SUMMARY.md manual.pdf --toc --page-numbers
```

### Batch conversion

```sh
rayomd --batch docs pdf-out --recursive --skip-unchanged --report=report.jsonl --workers=8
```

`--batch` converts a folder on up to 64 threads (6 by default), `--recursive` mirrors its
subfolders, `--skip-unchanged` keeps PDFs newer than their Markdown, and `--report` writes one
JSON line per document (status, exit code, pages, bytes, milliseconds, characters without a
glyph, failed images; `-` for standard output). `--stdin-batch` and `--serve` read paths from
standard input, and `--stdin` converts Markdown piped in.

### Compression

`--compress` writes page content, fonts and CMaps as FlateDecode streams with RayoMD's own
DEFLATE encoder: text-heavy PDFs come out two to five times smaller, at some export time.

### PDF/A-3b and PDF/UA-1

- `--pdfa` writes PDF/A-3b for archiving: every font embedded, an sRGB output intent, XMP
  metadata and a file identifier.
- `--pdfua` writes tagged PDF/UA-1 for screen readers: a structure tree of headings, lists,
  tables (header cells with scope), figures with their alt text, formulas, links and
  footnotes in reading order, with decoration, headers and footers marked as artifacts. The
  language is the front matter's `lang`, else `en`.

Both need a TrueType font to embed and show formulas as their TeX source; together the file
claims both standards. All test documents pass veraPDF. See
[Archival and Accessible PDFs](https://github.com/Butterski/rayomd/wiki/Archival-and-Accessible-PDFs).

### Reversible PDFs

`--embed-source` attaches the exact Markdown to the PDF, and `--recover-source` gets it back
byte for byte (`--inspect-source` describes it). It is off by default, because the file then
carries the whole source, including anything not shown on the pages. See
[Reversible PDFs](https://github.com/Butterski/rayomd/wiki/Reversible-PDFs).

```sh
rayomd --export notes.md notes.pdf --embed-source
rayomd --recover-source notes.pdf recovered.md
```

### Diagnostics

`--doctor` checks fonts, temporary output and a smoke export; `--bench` times repeated
exports of one document; `--version` prints the version. Exit codes are listed in the
[CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference).

## Documentation

| Topic | Guide |
|---|---|
| Installation and builds | [Getting Started](https://github.com/Butterski/rayomd/wiki/Getting-Started) |
| Commands, flags, and exit codes | [CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference) |
| Folders, reports, incremental runs | [Batch Conversion](https://github.com/Butterski/rayomd/wiki/Batch-Conversion) |
| One PDF of many files | [Books](https://github.com/Butterski/rayomd/wiki/Books) |
| Fonts, colours, headers, cover pages | [Themes](https://github.com/Butterski/rayomd/wiki/Themes) |
| PDF/A-3b and PDF/UA-1 | [Archival and Accessible PDFs](https://github.com/Butterski/rayomd/wiki/Archival-and-Accessible-PDFs) |
| Supported Markdown subset | [Native Markdown Support](https://github.com/Butterski/rayomd/wiki/Native-Markdown-Support) |
| Exact source recovery and privacy | [Reversible PDFs](https://github.com/Butterski/rayomd/wiki/Reversible-PDFs) |
| Embedding the exporter | [C++ API](https://github.com/Butterski/rayomd/wiki/C++-API) |
| Benchmarks and caveats | [Benchmarks](https://github.com/Butterski/rayomd/wiki/Benchmarks) |
| Regression measurement | [Performance Watcher](https://github.com/Butterski/rayomd/wiki/Performance-Watcher) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

Version-specific engineering contracts remain beside the code:

- [`docs/development/reversible_pdf_profile.md`](docs/development/reversible_pdf_profile.md)
- [`docs/development/native_math.md`](docs/development/native_math.md)
- [`docs/development/performance.md`](docs/development/performance.md)
- [`docs/development/arbitrary_pdf_research_decision.md`](docs/development/arbitrary_pdf_research_decision.md)

## Boundaries

RayoMD's native renderer is a fast Markdown subset, not a Pandoc clone. It deliberately does
not promise full CommonMark/Pandoc compatibility, complete LaTeX math (packages, macros,
automatic numbering), highlighting for every language, citations, filters, templates,
right-to-left scripts (Arabic and Hebrew are drawn left to right, unshaped), complete SVG
(radial gradients and patterns are approximated; markers, filters and masks are not drawn),
or HTML/CSS layout fidelity. Under `--pdfua`, a theme's cover page is tagged as decoration
and an inline formula is text showing its TeX source. Missing or unsupported images and
malformed math degrade to readable fallbacks instead of failing the export. The complete
matrix is maintained in
[Native Markdown Support](https://github.com/Butterski/rayomd/wiki/Native-Markdown-Support).

## Packaging and development

Default releases remain dependency-light; see [Packaging and Releases](https://github.com/Butterski/rayomd/wiki/Packaging-and-Releases) for the full release policy:

- `rayomd-<version>-windows-x64.zip` — Windows GUI and CLI executable.
- `rayomd-<version>-linux-x64.tar.gz` — portable Linux CLI without libcurl.
- `rayomd-<version>-linux-x64-curl.tar.gz` — Linux CLI with URL-image support.

Do not bundle Pandoc into the lightweight package. It is an optional external
compatibility path with separate licensing and deployment considerations.

Before contributing, read [`AGENTS.md`](AGENTS.md) and
[`CONTRIBUTING.md`](CONTRIBUTING.md). Keep benchmark claims dated and scoped,
and keep generated builds, PDFs, corpora, and raw reports out of source.

## License

RayoMD is released under the Apache License 2.0. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE). Dear ImGui retains its own license in
[`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt).
