# Fast Markdown

<p align="center">
  <a href="https://github.com/Butterski/md2pdf/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/md2pdf/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/md2pdf/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/md2pdf/actions/workflows/hygiene.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/md2pdf/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/Butterski/md2pdf/actions/workflows/codeql.yml/badge.svg"></a>
  <img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg">
  <img alt="Version: 0.1.0" src="https://img.shields.io/badge/version-0.1.0-informational">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms: Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-2ea44f">
</p>

Fast Markdown is a tiny native Markdown-to-PDF converter built for fast startup,
small releases, and predictable deployment.

<p align="center">
  <img src="docs/assets/catto.png" alt="Fast Markdown mascot" width="180">
</p>

The default renderer parses Markdown and writes PDF bytes directly from C++17.
It does not start a browser, bundle a runtime, or require Pandoc or LaTeX on
the fast path. Windows builds a compact Dear ImGui + DirectX 11 desktop app
with CLI modes; non-Windows builds use the same native exporter through a small
CLI.

## Highlights

- Native Markdown-to-PDF path with no browser engine.
- Single Windows GUI executable for the default release.
- Compact non-Windows CLI binary.
- Fast warm conversion for small and medium documents.
- Batch, stdin batch, warm serve, and benchmark modes.
- Unicode output through subset embedded system fonts.
- Standard PDF font path for ASCII documents.
- Local and HTTP/HTTPS image support with fallback text.
- Clickable Markdown links emitted as PDF annotations.
- Optional Windows Pandoc mode for full-document compatibility.

## When To Use It

Use native Fast Markdown when the document is simple, speed matters, and the
package needs to stay small. Use Pandoc, a browser renderer, or LaTeX when you
need a complete document ecosystem.

| Need | Native Fast Markdown | Pandoc / browser / LaTeX |
|---|---|---|
| Small release artifact | Good fit | Usually much larger |
| Cold startup speed | Good fit | Often slower |
| Bulk conversion | Very Good fit | Depends on external tool startup |
| Full CommonMark/Pandoc extensions | Not a goal | Good fit |
| Full TeX math layout | Not supported | Good fit |
| Custom templates and filters | Not supported | Good fit |
| Simple Markdown reports | Good fit | Also works, with more dependencies |

The native renderer is intentionally a fast Markdown subset, not a Pandoc clone.
That boundary keeps the binary small and the default path dependency-light.

## Benchmark Snapshot

These are local CMake Release results kept as reproducible baselines for this
repository. Treat them as engineering numbers, not universal performance claims:
hardware, storage, fonts, compiler, and document shape all matter. The Windows
smoke row was refreshed on 2026-06-17 after embedding the app icon resource.

Warm `--bench` rows measure in-process PDF byte generation after startup. Export
and batch rows include process and file I/O. Linux batch numbers below came from
native WSL ext4 storage; `/mnt/*` Windows-mounted paths are much slower for many
small files.

### Small Document Smoke

The `tester.md` rows use `modern normal`; the ASCII smoke row uses the verifier
default shown in `scripts/verify-linux.sh`.

| Build | Input | Iterations | Avg conversion | Output PDF |
|---|---:|---:|---:|---:|
| Windows GUI/CLI | `tester.md`, Unicode | 100 | `0.53 ms` | `102,595 bytes` |
| Linux CLI | `tester.md`, Unicode | 100 | `0.22 ms` | `168,749 bytes` |
| Linux CLI | ASCII smoke doc | 1,000 | `0.02 ms` | `1,899 bytes` |

Release binary sizes from local release builds:

| Target | Size |
|---|---:|
| `fast-markdown-imgui.exe` Windows app | `2,341,888 bytes` |
| `fast-markdown` Linux CLI | `199,088 bytes` |

### Larger Synthetic Runs

Run date: 2026-06-06. Full caveats and source report pointers are in
[`docs/benchmarks/commercial_benchmark_summary.md`](docs/benchmarks/commercial_benchmark_summary.md).

| Case | Result | Caveat |
|---|---:|---|
| Linux 100 MB native export | `7.03 s` | WSL ext4, synthetic ASCII-heavy document |
| Windows 100 MB native export | `13.48 s` | Synthetic ASCII-heavy document |
| Linux 100 MB warm native benchmark | `6.48 s avg` | Warm `--bench`, no end-to-end I/O |
| Linux 100-file stdin batch | `0.144 s` | 100 synthetic files around 20 KB each |
| Windows 20-file batch | `0.610 s` | 20 synthetic files around 100 KB each |
| Windows Pandoc compatibility smoke | `8.11 s` | External Pandoc/LaTeX path, 100 KB ASCII input |

Use `scripts/perf_watch.py` for current before/after work. It records machine
and git metadata, keeps JSONL history, and compares matching runs.

## Native Markdown Support

| Feature | Native support |
|---|---|
| Headings and paragraphs | Yes |
| Bullet and numbered lists | Yes, including simple indentation-based nesting |
| Block quotes | Yes |
| Fenced code blocks | Yes, plain rendering without syntax highlighting |
| Pipe tables | Yes, with basic left, center, and right alignment |
| Horizontal rules | Yes |
| Explicit page breaks | `\pagebreak`, `\newpage`, and `<!-- pagebreak -->` |
| Inline emphasis | Cleanup/rendering for bold, italic, strikethrough, and code spans |
| Links | Clickable PDF annotations for Markdown links |
| Images | Standalone local and HTTP/HTTPS images with alt-text fallback |
| Math markers | Inline cleanup and `$$` blocks rendered as formula boxes |
| Unicode | Embedded subset system font when needed |
| YAML front matter | Ignored |
| Common status emoji | Normalized fallback text for supported symbols |

Native mode does not currently support:

- Full CommonMark or Pandoc extension compatibility.
- Full TeX math layout.
- Syntax highlighting.
- Footnotes.
- Citations and bibliographies.
- Custom Pandoc filters or templates.
- HTML/CSS layout fidelity.

## Build

### Windows

Requirements:

- MinGW-w64 with C++17 support.
- Windows SDK / DirectX 11 libraries.
- CMake.

Build:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

Output:

```text
build/windows/fast-markdown-imgui.exe
```

### Linux / WSL

Requirements:

- `g++` or `clang++` with C++17 support.
- CMake.
- libcurl development headers for HTTP/HTTPS image URLs.
- A system TrueType font for Unicode output, such as DejaVu Sans.
- zlib development headers are optional and enable PNG alpha support when found.

Ubuntu/WSL dependencies:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake fonts-dejavu-core libcurl4-openssl-dev zlib1g-dev
```

Build:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

Output:

```text
build/linux/fast-markdown
```

Linux verification:

```sh
sh scripts/verify-linux.sh
```

## Continuous Verification

GitHub Actions run the release-critical checks on every push and pull request:

| Workflow | What it verifies |
|---|---|
| `CI` | Linux native CLI build/export/bench, Windows MinGW ImGui build, and a Windows benchmark smoke |
| `Repository Hygiene` | Required release files, Python helper syntax, local README targets, and no generated binaries/PDFs in source |
| `CodeQL` | Weekly and PR C++ static analysis for the Linux native build path |

## Usage

### Windows GUI

1. Run `fast-markdown-imgui.exe`.
2. Write or paste Markdown, or drag in a `.md` file.
3. Choose `Native Tiny PDF` or `Pandoc (full)`.
4. Choose style and margin.
5. Export with the button or `Ctrl+E`.

### CLI

Windows:

```batch
fast-markdown-imgui.exe --version
fast-markdown-imgui.exe --export input.md output.pdf native elegant normal
fast-markdown-imgui.exe --batch input-folder output-folder native modern normal
type files.txt | fast-markdown-imgui.exe --stdin-batch output-folder native modern normal
fast-markdown-imgui.exe --serve output-folder native modern normal
fast-markdown-imgui.exe --bench input.md bench-output-folder 5000 modern normal
```

Linux / WSL:

```sh
./fast-markdown --version
./fast-markdown --export input.md output.pdf native elegant normal
./fast-markdown --batch input-folder output-folder native modern normal
cat files.txt | ./fast-markdown --stdin-batch output-folder native modern normal
./fast-markdown --serve output-folder native modern normal
./fast-markdown --bench input.md bench-output-folder 5000 modern normal
```

Modes:

| Mode | Use case |
|---|---|
| `--version` | Print the compiled project version |
| `--export` | Convert one Markdown file |
| `--batch` | Convert every `.md` file in a folder |
| `--stdin-batch` | Feed file paths from another process |
| `--serve` | Keep one process warm and convert paths sent over stdin |
| `--bench` | Measure native PDF generation time and output size |

Styles:

- `elegant`
- `modern`
- `tech`

Margins:

- `compact`
- `normal`
- `wide`
- `margin=0.75in`
- `margin=54pt`

Exit codes:

| Code | Meaning |
|---:|---|
| `0` | Success |
| `2` | Missing CLI arguments |
| `3` | Input file could not be read |
| `11` | Native exporter could not load a system font |
| `12` | Native exporter could not write the PDF |
| `20` | Pandoc export failed or is unsupported in this build |

## C++ API

The public native API is intentionally small and lives in
[`include/fast_markdown/tiny_pdf.h`](include/fast_markdown/tiny_pdf.h).

```cpp
#include "fast_markdown/tiny_pdf.h"

#include <string>

std::string pdfBytes;

TinyPdf::BuildOptions options;
options.styleIdx = 1;      // modern
options.marginIdx = 1;     // normal
options.sourcePath = "input.md";
options.enableUrlImages = true;

if (!TinyPdf::BuildPdfBytes(markdownText, options, pdfBytes)) {
    return TinyPdf::g_lastError;
}
```

Set `BuildOptions::sourcePath` for file-based conversions so relative local
images resolve next to the input Markdown file.

## Performance Watcher

Use `scripts/perf_watch.py` when you want a repeatable "did this version get
slower?" check. It creates a deterministic random Markdown corpus using the
native feature subset, runs no-UI CLI modes, appends a JSONL history record, and
reports percent deltas against the previous matching platform/suite/seed/options
run.

Windows:

```sh
python scripts/perf_watch.py --binary build/windows/fast-markdown-imgui.exe --platform windows --suite watch --label local
```

WSL/Linux:

```sh
python3 scripts/perf_watch.py --binary build/linux/fast-markdown --platform linux-wsl --suite watch --label local
```

Run both from PowerShell when both binaries are already built:

```powershell
scripts/perf_watch_both.ps1 -Suite watch -Label local
```

Suites are `quick`, `watch`, and `full`. Add `--fail-on-slower-pct 10` when the
watcher should return a nonzero exit code if any time metric is at least 10%
slower than the previous matching run. Remote image timing is intentionally
excluded because network timing is not a stable performance signal.

## Project Layout

```text
include/fast_markdown/       Public C++ header for the native exporter
src/core/                    Portable Markdown parser and PDF writer
src/cli/                     Portable CLI entry point
src/win32/                   Windows Dear ImGui app, CLI glue, and resources
scripts/                     Verification and benchmark helper scripts
docs/assets/                 Mascot and icon source assets
docs/benchmarks/             Archived benchmark summaries and caveats
docs/optimization/           Optimization notes and research follow-up
third_party/imgui/           Vendored Dear ImGui
third_party/simdutf/         Optional simdutf experiment, OFF by default
tester.md                    Hand-written smoke/regression Markdown document
CMakeLists.txt               Cross-platform build definition
```

Generated build trees, generated PDFs, benchmark corpora, and local binaries are
ignored and should not be committed.

## Packaging

Default native releases are intentionally small:

- Windows GUI release: ship `fast-markdown-imgui.exe`.
- Linux/WSL CLI release: ship `fast-markdown`.

Do not bundle Pandoc unless deliberately producing a larger compatibility
package and accounting for Pandoc's GPL license terms. Keep the native package
dependency-light.

The project license allows commercial use, paid binaries, paid support, and a
hosted API service. It does not stop someone else from building a competing
commercial product from the open-source code. Use a separate commercial license,
cloud terms of service, trademark policy, or source-available license only if
that becomes a business requirement.

## Development Notes

- Read [`AGENTS.md`](AGENTS.md) before making changes.
- See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build, verification, and release
  contribution notes.
- Keep benchmark claims conservative and reproducible.
- Keep the native renderer honest about being a subset.
- Prefer standard library and platform APIs already in use before adding
  dependencies.
- Do not edit vendored Dear ImGui files unless the task explicitly requires it.

## License

Fast Markdown is released under the Apache License 2.0. See [`LICENSE`](LICENSE)
and [`NOTICE`](NOTICE). Dear ImGui keeps its own license in
[`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt).
