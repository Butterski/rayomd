# Fast Markdown

A lightweight native Windows Markdown to PDF converter built with Dear ImGui and DirectX 11.

The default exporter is built into the EXE. It does not require Pandoc, LaTeX, a browser engine, or a bundled runtime.

![Fast Markdown](catto.png)

## Features

- Native Markdown to PDF export with no external runtime dependency
- Optional Pandoc compatibility engine for full-fidelity documents
- Dark Windows-style ImGui interface
- Drag and drop `.md` files into the editor
- `Ctrl+E` quick export
- Command-line export mode for scripts
- Single small Windows EXE release artifact

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

Native mode writes PDF directly from C++ and embeds a Windows system font into the generated PDF so Unicode text can render correctly. The app does not ship a font file.

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

Requirements:

- MinGW-w64 with C++17 support
- Windows SDK / DirectX 11 libraries

Build:

```batch
build.bat
```

The current build produces `fast-markdown-imgui.exe` at about 1.1 MB.

## Usage

GUI:

1. Run `fast-markdown-imgui.exe`.
2. Write or paste Markdown, or drag in a `.md` file.
3. Choose `Native Tiny PDF` or `Pandoc (full)`.
4. Choose a style.
5. Click `Export PDF` or press `Ctrl+E`.

CLI:

```batch
fast-markdown-imgui.exe --export input.md output.pdf
fast-markdown-imgui.exe --export input.md output.pdf native elegant
fast-markdown-imgui.exe --export input.md output.pdf pandoc modern
fast-markdown-imgui.exe --export input.md output.pdf native tech
```

Styles are `elegant`, `modern`, and `tech`. Numeric style indexes `0`, `1`, and `2` also work.

CLI exit codes:

- `0`: success
- `2`: missing CLI arguments
- `3`: input file could not be read
- `11`: native exporter could not load a Windows font
- `12`: native exporter could not write the PDF
- `20`: Pandoc export failed

## Release Packaging

For the default native exporter, package only:

- `fast-markdown-imgui.exe`

Optional files for source/releases:

- `README.md`
- `LICENSE`
- `catto.ico` / `catto.png`

Do not bundle Pandoc unless you intentionally want a large compatibility package and are prepared to comply with Pandoc's GPL license terms.

## Project Structure

```text
main.cpp              Application and native PDF exporter
build.bat             MinGW build script
imgui/                Dear ImGui sources
catto.ico             Application icon
catto.png             Image used in README
```

## License

Fast Markdown is released under the MIT License. Dear ImGui keeps its own license in `imgui/LICENSE.txt`.
