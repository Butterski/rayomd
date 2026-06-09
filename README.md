# Fast Markdown

Fast Markdown is a tiny native Markdown-to-PDF converter built for speed, small releases, and predictable deployment.

![Fast Markdown mascot](catto.png)

The native exporter writes PDF bytes directly from C++. It does not start a browser, bundle a runtime, or require Pandoc/LaTeX for the fast path. Windows also ships a Dear ImGui + DirectX 11 desktop app; Linux builds the same exporter as a compact CLI.

## Why It Exists

Most Markdown-to-PDF tools route through a browser, LaTeX, or a large document stack. That is useful for full fidelity, but expensive when the job is simple: turn Markdown into a clean PDF quickly, in bulk, without shipping hundreds of megabytes.

Fast Markdown targets that lightweight case:

- Native C++ Markdown parser and PDF writer
- No browser engine in the default path
- No LaTeX dependency in the default path
- Standard PDF font path for ASCII documents
- Subset embedded system fonts for Unicode documents
- Windows GUI plus cross-platform native CLI
- Optional Pandoc mode on Windows for full-document compatibility

## Benchmarks

These are local verified CMake release results from this repo after the Linux migration and code split.

| Build | Input | Iterations | Avg Conversion | Output PDF |
|---|---:|---:|---:|---:|
| Windows GUI/CLI | `tester.md`, Unicode | 100 | `0.50 ms` | `105,249 bytes` |
| Linux CLI | `tester.md`, Unicode | 100 | `0.22 ms` | `168,749 bytes` |
| Linux CLI | ASCII smoke doc | 1,000 | `0.02 ms` | `1,899 bytes` |

Release binary sizes from the same verification run:

| Target | Size |
|---|---:|
| `fast-markdown-imgui.exe` Windows app | `1,162,240 bytes` |
| `fast-markdown` Linux CLI | `199,088 bytes` |

Notes:

- Benchmarks measure warm in-process PDF byte generation through the built-in benchmark command.
- Windows used Segoe UI for Unicode output; Linux used DejaVu Sans. Different fonts produce different subset sizes.
- The native renderer is intentionally a Markdown subset, not a full Pandoc replacement.

## Supported Native Markdown

- Headings
- Paragraphs
- Bullet and numbered lists, including simple indentation-based nesting
- Block quotes
- Fenced code blocks
- Pipe tables with basic left, center, and right alignment
- Inline math marker cleanup and `$$` math blocks rendered as formula boxes
- Horizontal rules
- Explicit page breaks with `\pagebreak`, `\newpage`, or `<!-- pagebreak -->`
- Standalone local and HTTP/HTTPS images with alt-text fallback
- Clickable Markdown links in native PDFs
- Faux bold, italic, and strikethrough for inline emphasis
- Basic inline cleanup for code spans and inline images
- Common status emoji fallback text
- YAML front matter ignored

Not supported in native mode:

- Full TeX math layout
- Syntax highlighting
- Footnotes
- Citations
- Custom Pandoc filters/templates
- Full CommonMark/Pandoc extension compatibility

Use Pandoc mode when you need the full document ecosystem. Use native mode when latency, package size, and simple deployment matter.

## Build

### Windows

Requirements:

- MinGW-w64 with C++17 support
- Windows SDK / DirectX 11 libraries
- CMake

Build:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

Output:

```text
build/windows/fast-markdown-imgui.exe
```

### Linux

Requirements:

- `g++` or `clang++` with C++17 support
- CMake
- libcurl development headers for HTTP/HTTPS image URLs
- A system TrueType font for Unicode output, such as DejaVu Sans

Ubuntu/WSL dependencies:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake fonts-dejavu-core libcurl4-openssl-dev
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

## Usage

Windows GUI:

1. Run `fast-markdown-imgui.exe`.
2. Write or paste Markdown, or drag in a `.md` file.
3. Choose `Native Tiny PDF` or `Pandoc (full)`.
4. Choose style and margin.
5. Export with the button or `Ctrl+E`.

Windows CLI:

```batch
fast-markdown-imgui.exe --export input.md output.pdf native elegant normal
fast-markdown-imgui.exe --batch input-folder output-folder native modern normal
type files.txt | fast-markdown-imgui.exe --stdin-batch output-folder native modern normal
fast-markdown-imgui.exe --serve output-folder native modern normal
fast-markdown-imgui.exe --bench input.md bench-output-folder 5000 modern normal
```

Linux CLI:

```sh
./fast-markdown --export input.md output.pdf native elegant normal
./fast-markdown --batch input-folder output-folder native modern normal
cat files.txt | ./fast-markdown --stdin-batch output-folder native modern normal
./fast-markdown --serve output-folder native modern normal
./fast-markdown --bench input.md bench-output-folder 5000 modern normal
```

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

## CLI Modes

| Mode | Use case |
|---|---|
| `--export` | Convert one Markdown file |
| `--batch` | Convert every `.md` file in a folder |
| `--stdin-batch` | Feed file paths from another process |
| `--serve` | Keep one process warm and convert paths sent over stdin |
| `--bench` | Measure native PDF generation time and output size |

Exit codes:

| Code | Meaning |
|---:|---|
| `0` | Success |
| `2` | Missing CLI arguments |
| `3` | Input file could not be read |
| `11` | Native exporter could not load a system font |
| `12` | Native exporter could not write the PDF |
| `20` | Pandoc export failed or unsupported in this build |

## Project Layout

```text
include/              Public C++ headers
src/core/             Portable Markdown parser and PDF writer
src/cli/              Portable native CLI entrypoint
src/win32/            Windows ImGui + DirectX app and Windows CLI glue
scripts/              Non-root verification helpers
docs/                 Brand prompt and project notes
imgui/                Dear ImGui sources
catto.png             Transparent flat-vector mascot graphic
catto.ico             Windows app icon
CMakeLists.txt        Cross-platform build definition
```

## Packaging

Native releases can be small:

- Windows GUI release: ship `fast-markdown-imgui.exe`
- Linux CLI release: ship `fast-markdown`

Do not bundle Pandoc unless you intentionally want a larger compatibility package and are prepared to comply with Pandoc's GPL license terms.

## License

Fast Markdown is released under the MIT License. Dear ImGui keeps its own license in `imgui/LICENSE.txt`.
