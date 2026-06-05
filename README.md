# Fast Markdown

A lightweight native Markdown to PDF converter. The Windows build includes a Dear ImGui + DirectX 11 desktop app; Linux builds the same native exporter as a portable CLI.

The default exporter is built into the app binary. It does not require Pandoc, LaTeX, a browser engine, or a bundled runtime.

![Fast Markdown](catto.png)

## Features

- Native Markdown to PDF export with no external runtime dependency
- Optional Pandoc compatibility engine for full-fidelity documents
- Dark Windows-style ImGui interface on Windows
- Drag and drop `.md` files into the editor
- `Ctrl+E` quick export
- Command-line single-file and batch export modes for Windows and Linux scripts
- Independent margin control: compact, normal, or wide
- Single small Windows EXE release artifact
- Unicode font subsetting for much smaller embedded-font PDFs

## Export Engines

### Native Tiny PDF

This is the default engine. It is designed for speed, low memory use, and simple packaging.

Supported Markdown subset:

- Headings
- Paragraphs
- Bullet and numbered lists, including simple indentation-based nesting
- Block quotes
- Fenced code blocks
- Pipe tables with basic left/center/right alignment
- Inline math marker cleanup and `$$` math blocks rendered as formula boxes
- Horizontal rules
- Faux bold, italic, and strikethrough for inline emphasis
- Basic inline cleanup for code spans, links, and images
- Common status emoji fallback text for small embedded-font PDFs
- YAML front matter is ignored

Native mode writes PDF directly from C++ and embeds a subset of a system font into the generated PDF so Unicode text can render correctly without shipping a font file. On Windows it searches Segoe UI, Arial, and Tahoma. On Linux it searches common DejaVu, Liberation, FreeFont, and Noto paths. Set `FAST_MARKDOWN_FONT=/path/to/font.ttf` to force a specific font.

ASCII-only documents use an even faster standard PDF font path. That path does not load or embed a system font, so output files are usually only a few KB and batch exports can run in a few milliseconds per document. Documents with Polish characters, emoji, or other Unicode automatically use the Unicode-safe embedded-font path.

Not supported in native mode:

- Full TeX math layout
- Syntax highlighting
- Footnotes
- Citations
- Image embedding
- Custom Pandoc filters/templates
- Full CommonMark/Pandoc extension compatibility

### Pandoc

Pandoc mode is still available for documents that need the full Pandoc pipeline. It shells out to `pandoc` and uses the selected style arguments.

Pandoc mode requires:

- Pandoc
- A PDF engine such as XeLaTeX, LaTeX, Typst, wkhtmltopdf, or another Pandoc-supported engine

## Build

Windows GUI requirements:

- MinGW-w64 with C++17 support
- Windows SDK / DirectX 11 libraries

Windows GUI build:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

The current Windows CMake release build produces `fast-markdown-imgui.exe` at about 1.16 MB.

Linux portable CLI requirements:

- `g++` or `clang++` with C++17 support
- A system TrueType font for Unicode output, such as DejaVu Sans

On Ubuntu/WSL, install build dependencies with your package manager:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake fonts-dejavu-core
```

Linux CLI build:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

The Linux build does not compile ImGui or DirectX. It builds the native Markdown-to-PDF CLI from the same PDF exporter core.

Linux verification:

```sh
sh scripts/verify-linux.sh
```

## Usage

Windows GUI:

1. Run `fast-markdown-imgui.exe`.
2. Write or paste Markdown, or drag in a `.md` file.
3. Choose `Native Tiny PDF` or `Pandoc (full)`.
4. Choose a style.
5. Choose a margin.
6. Click `Export PDF` or press `Ctrl+E`.

CLI:

```batch
fast-markdown-imgui.exe --export input.md output.pdf
fast-markdown-imgui.exe --export input.md output.pdf native elegant
fast-markdown-imgui.exe --export input.md output.pdf pandoc modern
fast-markdown-imgui.exe --export input.md output.pdf native tech
fast-markdown-imgui.exe --export input.md output.pdf native modern compact
fast-markdown-imgui.exe --batch input-folder output-folder native modern normal
type files.txt | fast-markdown-imgui.exe --stdin-batch output-folder native modern normal
fast-markdown-imgui.exe --serve output-folder native modern normal
fast-markdown-imgui.exe --bench input.md bench-output-folder 5000 modern normal
```

Linux CLI uses the same native arguments after building `fast-markdown`:

```sh
./fast-markdown --export input.md output.pdf native elegant normal
./fast-markdown --batch input-folder output-folder native modern normal
cat files.txt | ./fast-markdown --stdin-batch output-folder native modern normal
./fast-markdown --serve output-folder native modern normal
./fast-markdown --bench input.md bench-output-folder 5000 modern normal
```

Styles are `elegant`, `modern`, and `tech`. Numeric style indexes `0`, `1`, and `2` also work.
Margins are `compact`, `normal`, and `wide`.

Batch mode converts every `.md` file in the input folder to a same-name `.pdf` in the output folder. Native batch mode keeps the parsed font metrics cached for the whole batch, so it is much faster than launching the EXE once per document.

Stdin batch mode reads one Markdown file path per input line and writes same-name PDFs to the output folder. It is meant for tools that want to keep one Fast Markdown process warm and feed it work without relaunching the EXE per document.

Serve mode is the lowest-latency integration path. Start one process, write one Markdown file path per stdin line, and read `OK/ERR`, elapsed milliseconds, and the output PDF path from stdout. Send `quit` or close stdin to stop it.

Benchmark mode warms the native renderer once, then measures in-process PDF byte generation. It writes `bench-results.txt` and a validated `sample.pdf` to the output folder.

Current local benchmark examples after the optimization roadmap pass:

- ASCII standard-font path: 5,000 iterations, 0.07 ms average conversion, 2.5 KB PDF.
- Unicode embedded-font path using `tester.md`: 500 iterations, 0.52 ms average conversion, 105 KB PDF.
- Serve path: 20 ASCII files in 21.6-23.5 ms wall-clock total; reported per-file export plus write averaged 0.37-0.43 ms.
- Long ASCII document, 156 KB input: 100 iterations, 9.59 ms average conversion, 352 KB PDF.
- Long Unicode document, 163 KB input: 50 iterations, 49.79 ms average conversion, 1.85 MB PDF.

CLI exit codes:

- `0`: success
- `2`: missing CLI arguments
- `3`: input file could not be read
- `11`: native exporter could not load a system font
- `12`: native exporter could not write the PDF
- `20`: Pandoc export failed

## Release Packaging

For the default native exporter, package only:

- `fast-markdown-imgui.exe`
- `fast-markdown` on Linux CLI releases

Optional files for source/releases:

- `README.md`
- `LICENSE`
- `catto.ico` / `catto.png`

Do not bundle Pandoc unless you intentionally want a large compatibility package and are prepared to comply with Pandoc's GPL license terms.

## Project Structure

```text
include/              Public C++ headers
src/core/             Portable Markdown parser and PDF writer
src/cli/              Portable native CLI entrypoint
src/win32/            Windows ImGui + DirectX app and Windows CLI glue
CMakeLists.txt        Cross-platform build definition
scripts/              Non-root verification helpers
imgui/                Dear ImGui sources
catto.ico             Application icon
catto.png             Image used in README
```

## License

Fast Markdown is released under the MIT License. Dear ImGui keeps its own license in `imgui/LICENSE.txt`.
