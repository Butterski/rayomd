# Fast Markdown

A lightweight, modern Markdown to PDF converter with a native Windows UI built using ImGui and DirectX 11.

![Fast Markdown](catto.png)

## Features

- 🎨 **Dark Windows 11-style UI** - Clean, modern interface
- 📝 **Live Markdown editing** - Write and preview your content
- 📄 **PDF export** - Convert to beautifully formatted PDFs via Pandoc
- 🎯 **Multiple styles** - Elegant (LaTeX), Modern (Sans), Tech (Mono)
- 📂 **Drag & drop** - Load `.md` files directly
- ⌨️ **Keyboard shortcut** - `Ctrl+E` for quick export

## Requirements

### Build
- MinGW-w64 (g++ with C++17 support)
- Windows SDK (for DirectX 11)

### Runtime
- Windows 10/11
- [Pandoc](https://pandoc.org/installing.html) (for PDF export)
- [MiKTeX](https://miktex.org/) or [TeX Live](https://tug.org/texlive/) with XeLaTeX

## Build

```batch
build.bat
```

This produces `fast-markdown-imgui.exe` (~1MB).

## Usage

1. Run `fast-markdown-imgui.exe`
2. Write or paste Markdown content
3. Select a style from the dropdown
4. Click **Export PDF** (or press `Ctrl+E`)
5. Choose save location

### Drag & Drop

Drop a `.md` file onto the window to load it.

## Project Structure

```
├── main.cpp              # Application source
├── build.bat             # Build script
├── imgui/                # ImGui library
│   ├── backends/         # Platform/renderer backends
│   └── ...
├── catto.ico             # Application icon
└── catto.png             # Icon source
```

## License

MIT License - See [imgui/LICENSE.txt](imgui/LICENSE.txt) for ImGui license.
