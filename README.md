# RayoMD

<p align="center">
  <a href="https://github.com/Butterski/rayomd/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/rayomd/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/release.yml"><img alt="Release" src="https://github.com/Butterski/rayomd/actions/workflows/release.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml/badge.svg"></a>
  <img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg">
  <img alt="Version: 2.6.0" src="https://img.shields.io/badge/version-2.6.0-informational">
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
- Unicode, clickable links, tables, lists, code, local images, and opt-in URL images.
- Optional exact Markdown recovery through reversible PDFs.
- Optional Windows Pandoc mode when the native subset is not enough.

Use native RayoMD for simple reports and bulk conversion when startup time,
package size, and dependency count matter. Use Pandoc, a browser renderer, or
LaTeX when you need full CommonMark/Pandoc extensions, TeX math, filters,
templates, citations, syntax highlighting, or HTML/CSS fidelity.

## Performance snapshot

In a 2026-07-18 Windows 11 image-free fresh-process comparison, RayoMD 2.5.0
was fastest on all three deterministic workloads supported by its native
subset. The next-fastest comparator took `92.8x` to `123.2x` as long.

| Case | RayoMD | `md-to-pdf` | Pandoc + XeLaTeX |
|---|---:|---:|---:|
| Tiny README | **`17.76 ms`** | `1,647.49 ms` | `3,121.12 ms` |
| Medium feature mix | **`18.51 ms`** | `2,279.92 ms` | `3,406.69 ms` |
| 500-row table | **`19.41 ms`** | `2,307.51 ms` | `3,465.09 ms` |

A focused warm-path check on 2026-07-18 used the repository's 3,896-byte
`tester.md` fixture and an order-balanced frozen-baseline comparison:

| Platform/storage | RayoMD 2.4.1 baseline | RayoMD 2.5.0 | Change |
|---|---:|---:|---:|
| Windows 11 workspace | `1.833 ms` | **`0.865 ms`** | **`-51.0%` paired median** |
| Linux WSL `/mnt/e` | `12.029 ms` | **`7.320 ms`** | **`-31.4%` paired median** |

These are warm `--bench` generation medians; they exclude process startup,
input reading, and timed output writing. Each row uses 10 order-balanced pairs;
the 10 baseline and 10 candidate PDFs on each platform were byte-identical.

This is a scoped benchmark, not a universal ranking or a claim of feature or
visual equivalence. See the
[full report](https://github.com/Butterski/rayomd/wiki/Markdown-to-PDF-Speed-Comparison-2026-07-18),
[optimization audit](https://github.com/Butterski/rayomd/wiki/RayoMD-2.2.0-Optimization-Audit-2026-07-13),
[warm `tester.md` record](https://github.com/Butterski/rayomd/wiki/RayoMD-tester.md-Warm-Benchmark-2026-07-18),
and [benchmark archive](https://github.com/Butterski/rayomd/wiki/Benchmarks)
for methodology, environment, memory, binary-size, and reproduction details.

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

Common CLI workflows:

```sh
rayomd --export input.md output.pdf
rayomd --batch input-folder output-folder native modern normal --workers=4
rayomd --export input.md reversible.pdf native elegant normal --embed-source
rayomd --recover-source reversible.pdf recovered.md
```

The Windows app supports editing, drag-and-drop, engine/style/margin selection,
URL-image opt-in, reversible-PDF inspection, and `Ctrl+E` export.

For dependencies, curl-enabled Linux builds, stdin/serve modes, security flags,
exit codes, and all options, use the [Getting Started](https://github.com/Butterski/rayomd/wiki/Getting-Started)
and [CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference).

## Documentation

| Topic | Guide |
|---|---|
| Installation and builds | [Getting Started](https://github.com/Butterski/rayomd/wiki/Getting-Started) |
| Commands, flags, and exit codes | [CLI Reference](https://github.com/Butterski/rayomd/wiki/CLI-Reference) |
| Supported Markdown subset | [Native Markdown Support](https://github.com/Butterski/rayomd/wiki/Native-Markdown-Support) |
| Exact source recovery and privacy | [Reversible PDFs](https://github.com/Butterski/rayomd/wiki/Reversible-PDFs) |
| Embedding the exporter | [C++ API](https://github.com/Butterski/rayomd/wiki/C++-API) |
| Benchmarks and caveats | [Benchmarks](https://github.com/Butterski/rayomd/wiki/Benchmarks) |
| Regression measurement | [Performance Watcher](https://github.com/Butterski/rayomd/wiki/Performance-Watcher) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

Version-specific engineering contracts remain beside the code:

- [`docs/development/reversible_pdf_profile.md`](docs/development/reversible_pdf_profile.md)
- [`docs/development/performance.md`](docs/development/performance.md)
- [`docs/development/arbitrary_pdf_research_decision.md`](docs/development/arbitrary_pdf_research_decision.md)

## Native renderer scope

Native mode supports ATX and Setext headings, paragraphs, structured nested
lists and block quotes, fenced and indented code, pipe tables, rules, page
breaks, matching-run code spans, classic emphasis and escapes, inline and
reference-style links, URL/email autolinks, standalone inline/reference images,
basic math cleanup/boxes, Unicode fonts, and common status-symbol normalization.
Images embedded in paragraph text use a consistent `image: alt` fallback;
standalone images retain native image layout and missing-image fallback text.

It deliberately does not promise full CommonMark/Pandoc compatibility, TeX math,
syntax highlighting, footnotes, citations, filters, templates, or HTML/CSS
layout fidelity. The complete and current matrix is maintained in
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
