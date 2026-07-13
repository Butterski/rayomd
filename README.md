# RayoMD

<p align="center">
  <a href="https://github.com/Butterski/rayomd/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/rayomd/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/rayomd/actions/workflows/hygiene.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/release.yml"><img alt="Release" src="https://github.com/Butterski/rayomd/actions/workflows/release.yml/badge.svg"></a>
  <a href="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/Butterski/rayomd/actions/workflows/codeql.yml/badge.svg"></a>
  <img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg">
  <img alt="Version: 2.2.0" src="https://img.shields.io/badge/version-2.2.0-informational">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms: Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-2ea44f">
</p>

RayoMD is a tiny native Markdown-to-PDF converter built for fast startup,
small releases, and predictable deployment.

<p align="center">
  <img src="docs/assets/branding/rayomd.png" alt="RayoMD mascot" width="180">
  <br><sub><sup>* My graphic designer is working on a proper SVG version.</sup></sub>
</p>

<p align="center">
  <img src="docs/assets/demo/ui.png" alt="RayoMD Windows app screenshot" width="780">
</p>

<p align="center">
  <img src="docs/assets/demo/show.gif" alt="RayoMD Windows app demo" width="780">
</p>

<p align="center">
  <a href="docs/assets/demo/show.mp4">Download the MP4 demo</a>
</p>

The default renderer parses Markdown and writes PDF bytes directly from C++17.
It does not start a browser, bundle a runtime, or require Pandoc or LaTeX on
the fast path. Windows builds a compact Dear ImGui + DirectX 11 desktop app
with CLI modes; non-Windows builds use the same native exporter through a small
CLI.

## Highlights

- Native Markdown-to-PDF path with no browser engine.
- Fastest in our July 2026 Windows three-tool comparison: RayoMD finished in
  `15.45` to `18.66 ms`; the next-fastest comparator took `111.2x` to
  `132.0x` as long.
- Single Windows GUI executable for the default release.
- Compact non-Windows CLI binary.
- Fast warm conversion for small and medium documents.
- Bounded parallel folder/stdin batch, stdin Markdown export, warm serve, and benchmark modes.
- Unicode output through subset embedded system fonts.
- Standard PDF font path for ASCII documents.
- Safe local image support with fallback text; HTTP/HTTPS images are explicit opt-in on Windows and curl-enabled Linux builds.
- Clickable Markdown links emitted as PDF annotations.
- Optional Windows Pandoc mode for full-document compatibility.

Pandoc mode resolves `pandoc.exe` to an executable path before launch. During the transition it may still resolve from `PATH`, but it warns when `RAYOMD_PANDOC` is not set to an absolute executable path.

## When To Use It

Use native RayoMD when the document is simple, speed matters, and the
package needs to stay small. Use Pandoc, a browser renderer, or LaTeX when you
need a complete document ecosystem.

| Need | Native RayoMD | Pandoc / browser / LaTeX |
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

## Performance Snapshot

These are dated local CMake Release measurements, not a universal ranking.
Hardware, storage, fonts, compiler, document shape, and supported feature scope
all affect the result. Warm `--bench` rows measure in-process PDF generation;
export and batch rows include process startup and file I/O.

### Fresh-Process Three-Tool Comparison (2026-07-13)

On one Windows 11 system, RayoMD 2.2.0 was the fastest of the three tested
converters in every case. The deterministic synthetic inputs were supported by
RayoMD's native subset, and the next-fastest comparator took `111.2x` to
`132.0x` as long in this run.

| Case | Input | RayoMD 2.2.0 | `md-to-pdf` 5.2.5 | Pandoc 3.9.0.1 + XeLaTeX | Next comparator / RayoMD |
|---|---:|---:|---:|---:|---:|
| Tiny README | `205 B` | **`15.45 ms`** | `1,717.89 ms` | `3,210.69 ms` | **`111.2x`** |
| Medium feature mix | `15,102 B` | **`17.29 ms`** | `2,281.90 ms` | `3,377.94 ms` | **`132.0x`** |
| 500-row table | `33,353 B` | **`18.66 ms`** | `2,340.90 ms` | `3,427.50 ms` | **`125.5x`** |

Each tool ran in a fresh process. One uncounted warm-up preceded seven measured
runs, the table reports medians, and every result was checked for a PDF header,
EOF marker, and non-trivial size. The renderers do not provide equivalent
features or visual output: Pandoc/XeLaTeX and browser-based `md-to-pdf` cover
far more document and styling behavior than RayoMD's deliberately focused
native subset. The results support the scoped claim that RayoMD was fastest in
this benchmark, not an unqualified claim across every converter or workload.

See the dated
[`three-tool benchmark report`](docs/benchmarks/markdown_pdf_speed_comparison_2026-07-13.md)
for timing ranges, environment, versions, caveats, and reproduction commands.

### Native 2.2.0 Release Snapshot

The broader 2026-07-13 suite measured the default one-worker path separately:

| Platform/storage | Binary | Warm aggregate | Cold export | Folder batch | stdin batch | Serve reported |
|---|---:|---:|---:|---:|---:|---:|
| Windows workspace | `2,759,168 B` | `8.110 ms` | `29.752 ms` | `16.053 ms/file` | `7.730 ms/file` | `7.295 ms` |
| Linux WSL ext4 | `367,096 B` | `6.115 ms` | `15.965 ms` | `5.785 ms/file` | `5.608 ms/file` | `5.255 ms` |

A separate five-round, 160-document scaling matrix improved folder throughput
by `2.7x` on Windows and `4.5x` on Linux at six workers, with the expected RSS
increase. Automatic worker caps limit memory multiplication for large inputs.

See the full
[`2.2.0 optimization audit`](docs/benchmarks/optimization_2.2.0_2026-07-13.md)
for p50/p95, peak RSS, correctness checks, and rejected LTO/PGO/streaming
experiments. Use [`tools/benchmark.py`](tools/benchmark.py) for current runs;
raw corpora and reports stay under ignored `benchmark-output/`.

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
| Images | Standalone local images contained to the Markdown file directory by default; HTTP/HTTPS images with explicit opt-in on Windows and curl-enabled Linux builds; alt-text fallback |
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
build/windows/rayomd.exe
```

### Linux / WSL

Requirements:

- `g++` or `clang++` with C++17 support.
- CMake.
- A system TrueType font for Unicode output, such as DejaVu Sans.
- zlib development headers are optional and enable PNG alpha support when found.
- libcurl development headers are optional and enable HTTP/HTTPS image fetching when `RAYOMD_USE_CURL=ON`.

Ubuntu/WSL dependencies:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake fonts-dejavu-core zlib1g-dev
```

Build:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

This default Linux build does not link libcurl, avoiding distro-specific
`libcurl.so.4` symbol-version requirements in portable binaries. HTTP/HTTPS
images degrade to fallback text in that build. To enable Linux URL image
fetching, install libcurl development headers and configure with:

```sh
cmake -S . -B build/linux-curl -DCMAKE_BUILD_TYPE=Release -DRAYOMD_USE_CURL=ON
cmake --build build/linux-curl --config Release
```

Output:

```text
build/linux/rayomd
```

Linux verification:

```sh
python3 tests/verify_cli.py --binary build/linux/rayomd
```

## Continuous Verification

GitHub Actions run the release-critical checks on every push and pull request:

| Workflow | What it verifies |
|---|---|
| CI | Linux and Windows builds followed by the same cross-platform CLI verifier |
| Performance smoke | Performance sampling in a separate workflow with no correctness assertions |
| `Repository Hygiene` | Required release files, Python helper syntax, local README targets, and no generated binaries/PDFs in source |
| `Release` | Tag/manual release packaging for Windows, default Linux, and curl-enabled Linux assets |
| `CodeQL` | Weekly and PR C++ static analysis for the Linux native build path |

## Usage

### Windows GUI

1. Run `rayomd.exe`.
2. Write or paste Markdown, or drag in a `.md` file.
3. Choose `Native Tiny PDF` or `Pandoc (full)`.
4. Choose style and margin.
5. Export with the button or `Ctrl+E`.
6. Enable `URL images` only for documents that should fetch remote images.

### CLI

Only the input and output paths are required for `--export`; use
`--stdin <output.pdf>` when piping Markdown content from another program.
Engine, style, and margin default to `native`, `elegant`, and `normal`. Local
images are contained to the input Markdown file directory by default, and URL
images are disabled unless `--allow-url-images` is passed or the Windows GUI
checkbox is enabled. Stdin exports have no input-file directory, so relative
local images render as fallback text unless `--allow-unsafe-local-images` is
explicitly used for trusted content. Native folder and stdin-batch modes use a
bounded worker pool: automatic mode uses at most six workers, drops to two when
the largest input exceeds 16 MiB, and becomes sequential above 64 MiB. Pass
`--workers=N` (1-64) for an explicit limit. Windows Pandoc batches remain
sequential because they launch an external compatibility pipeline.

Windows:

```batch
rayomd.exe --version
rayomd.exe --doctor
rayomd.exe --export input.md output.pdf native elegant normal
rayomd.exe --export input.md reversible.pdf native elegant normal --embed-source
rayomd.exe --inspect-source reversible.pdf
rayomd.exe --recover-source reversible.pdf recovered.md
type input.md | rayomd.exe --stdin output.pdf native elegant normal
rayomd.exe --batch input-folder output-folder native modern normal --workers=4
type files.txt | rayomd.exe --stdin-batch output-folder native modern normal
rayomd.exe --serve output-folder native modern normal
rayomd.exe --bench input.md bench-output-folder 5000 modern normal
```

Linux / WSL:

```sh
./rayomd --version
./rayomd --doctor
./rayomd --export input.md output.pdf native elegant normal
./rayomd --export input.md reversible.pdf native elegant normal --embed-source
./rayomd --inspect-source reversible.pdf
./rayomd --recover-source reversible.pdf recovered.md
cat input.md | ./rayomd --stdin output.pdf native elegant normal
./rayomd --batch input-folder output-folder native modern normal --workers=4
cat files.txt | ./rayomd --stdin-batch output-folder native modern normal
./rayomd --serve output-folder native modern normal
./rayomd --bench input.md bench-output-folder 5000 modern normal
```

Modes:

| Mode | Use case |
|---|---|
| `--version` | Print the compiled project version |
| `--doctor` | Diagnose capabilities, Unicode fonts, temporary output, and a smoke export |
| `--inspect-source` | Validate and describe an embedded RayoMD source profile |
| `--recover-source` | Recover byte-exact Markdown from a validated reversible PDF |
| `--export` | Convert one Markdown file |
| `--stdin` | Convert Markdown content read from stdin into one PDF |
| `--batch` | Convert every `.md` file in a folder |
| `--stdin-batch` | Feed Markdown file paths from another process |
| `--serve` | Keep one process warm and convert paths sent over stdin |
| `--bench` | Measure native PDF generation time and output size |

Styles:

- `elegant`
- `modern`
- `tech`

Resource flags:

- `--allow-url-images` enables HTTP/HTTPS image fetching and still blocks loopback, private, link-local, multicast, and non-HTTP(S) redirect targets.
- `--allow-unsafe-local-images` restores legacy local-image path behavior for trusted documents only.
- `--workers=N` sets the native folder/stdin-batch worker limit from 1 to 64; omit it for the memory-capped automatic policy.
- `--embed-source` opts a native export into exact Markdown recovery; anyone receiving the PDF can extract source content that is not visible on its pages.

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
| `2` | Missing or invalid CLI arguments |
| `3` | Input file or stdin Markdown could not be read |
| `11` | Native exporter could not load a system font |
| `12` | Native exporter could not write the PDF |
| `14` | Source selected for embedding exceeds 10 MiB |
| `15` | Source selected for embedding is not valid UTF-8 |
| `16` | Reversible PDF would exceed the 256 MiB recovery limit |
| `20` | Pandoc export failed or is unsupported in this build |
| `30` | PDF has no reversible RayoMD profile |
| `31` | Reversible profile version is unsupported |
| `32` | PDF/profile is corrupt, invalid UTF-8, or fails integrity validation |
| `33` | PDF or recovered source exceeds a configured limit |
| `34` | Recovery destination already exists |

## C++ API

The public native API is intentionally small and lives in
[`include/rayomd/tiny_pdf.h`](include/rayomd/tiny_pdf.h).

```cpp
#include "rayomd/tiny_pdf.h"

#include <string>

std::string pdfBytes;

TinyPdf::PdfOptions options;
options.style = TinyPdf::PdfStyle::Modern;
options.margin = TinyPdf::PdfMargin::Normal();
options.sourcePath = "input.md";
options.enableUrlImages = false;  // set true only for trusted URL image fetches
options.embedSource = false;      // opt in only when exact source recovery is wanted

TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdownText, options, pdfBytes);
if (!result) {
    return static_cast<int>(result.error);
}
```

Set `PdfOptions::sourcePath` for file-based conversions so relative local
images resolve next to the input Markdown file. By default, local image targets
must remain inside that source directory after canonicalization. Set
`allowUnsafeLocalImages` only for trusted documents that deliberately need
absolute, UNC/device, or parent-escaping paths.

## Reversible PDFs

Normal exports remain compact PDF 1.7 files and do not contain the Markdown
source. Pass `--embed-source` only when exact recovery is wanted. Reversible
outputs use the `rayomd-source/1` PDF 2.0 profile: a standard Associated File
and `EmbeddedFiles` name-tree entry contain the exact bytes, while bounded XMP
metadata records the source length and SHA-256 digest.

Embedding can disclose comments, front matter, internal notes, reference
definitions, whitespace, and other source content not visible on the pages.
It is disabled by default. On Windows, use **Open...** or Ctrl+O, or drop a
validated reversible `.pdf`, to recover its source into the editor. The editor
keeps the profile version, producer, source size, attachment name, digest
status, and edited state visible. **Save source...** uses the same atomic,
confirm-before-overwrite path as other document writes. A failed inspection or
recovery leaves the current editor contents unchanged.

`--inspect-source` validates without writing. `--recover-source` performs full
validation and atomically creates the caller-selected output; it refuses to
overwrite an existing file and never trusts the attachment name as a path.

Profile version 1 accepts PDFs up to 256 MiB and Markdown up to 10 MiB. It
supports only the single classic-xref structure emitted by RayoMD. Encryption,
incremental updates, object/xref streams, unexpected filters, duplicate source
entries, malformed offsets, invalid UTF-8, and digest mismatches are rejected.
Exact recovery never falls back to heuristic PDF-to-Markdown conversion.
UTF-8 BOMs and embedded NUL bytes are valid source bytes and survive exactly.

The source is uncompressed so default Windows and Linux packages remain
mutually recoverable without a mandatory compression dependency. See
[`docs/development/reversible_pdf_profile.md`](docs/development/reversible_pdf_profile.md).
General PDF-to-Markdown conversion is intentionally not shipped after the
[arbitrary-PDF dependency and quality review](docs/development/arbitrary_pdf_research_decision.md).

## Benchmarking

Correctness verification does not run timing loops. Use the maintained
tools/benchmark.py entry point for performance work; raw reports stay under
ignored benchmark-output/. Detailed commands and storage caveats are in
docs/development/performance.md.

    python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
    python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-wsl --suite watch --label local
    python tools/benchmark.py compare -- --rayomd build/windows/rayomd.exe --root benchmark-output/pandoc --runs 5
    python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --root benchmark-output/competitors
    python3 tools/benchmark.py release -- --from-version 1.1.0 --suite quick

Suites are quick, watch, and full. Use `--workers=N` to keep batch comparisons
comparable. The dedicated scaling harness records p50/p95, failures, and peak
RSS at explicit worker counts:

    python scripts/concurrency_benchmark.py --binary build/windows/rayomd.exe --corpus <folder> --output benchmark-output/concurrency
    python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe --platform windows --suite full

Optimization experiments are opt-in and remain out of normal releases:

    cmake -S . -B build/profile -DRAYOMD_ENABLE_PROFILING=ON
    cmake -S . -B build/lto -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_LTO=ON
    cmake -S . -B build/pgo -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_LTO=ON -DRAYOMD_PGO=GENERATE
    python scripts/train_pgo.py --binary build/pgo/rayomd --corpus <perf-watch-corpus>
    cmake -S . -B build/pgo -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_LTO=ON -DRAYOMD_PGO=USE

Set `RAYOMD_PROFILE=1` when running a profiling build to emit phase timing and
allocation deltas. Generate and consume PGO data in the same build tree, and
refresh it for every source/toolchain release. On Windows, the elevated
`scripts/capture_windows_profile.ps1` helper records the four WPR CPU traces
used by the 2.2.0 audit.

Release records belong under docs/benchmarks/releases/. Remote-image timing is
excluded because network latency is not a stable performance signal. The full
2.2.0 decisions and measurements are in
[docs/benchmarks/optimization_2.2.0_2026-07-13.md](docs/benchmarks/optimization_2.2.0_2026-07-13.md).

## Project Layout

    include/rayomd/             Public native exporter API
    src/core/                   Markdown parser and PDF writer
    src/cli/                    Portable CLI entry point
    src/win32/                  Windows app, CLI glue, and resources
    tests/verify_cli.py         Cross-platform black-box correctness verifier
    tools/benchmark.py          Maintained performance workflow entry point
    scripts/                    Focused benchmark implementation helpers
    docs/assets/branding/       Mascot and icon source assets
    docs/assets/demo/           Windows screenshot and demo media
    docs/benchmarks/releases/   Curated, dated release records
    docs/development/           Performance and optimization notes
    tester.md                   Hand-written smoke document

Generated build trees, benchmark corpora, local binaries, generated PDFs, and
raw timing reports are ignored and must not be committed.

## Packaging

Default native releases are intentionally small:

- Windows GUI release: ship `rayomd-<version>-windows-x64.zip` with `rayomd.exe`.
- Linux/WSL CLI release: ship `rayomd-<version>-linux-x64.tar.gz` with the default no-curl `rayomd`.
- Linux curl edition: ship `rayomd-<version>-linux-x64-curl.tar.gz` with URL image fetching enabled through libcurl.

The release workflow publishes these assets when `VERSION` changes on `master`/`main`,
for `v<VERSION>` tags, or from a manual workflow run. Manual reruns replace the
release files in place.

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

RayoMD is released under the Apache License 2.0. See [`LICENSE`](LICENSE)
and [`NOTICE`](NOTICE). Dear ImGui keeps its own license in
[`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt).
