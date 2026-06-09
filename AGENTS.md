# Fast Markdown Agent Guide

Read this file before changing the project. It captures the project goals,
architecture, and guardrails that are easy to lose between sessions.

## Product Goal

Fast Markdown is a tiny native Markdown-to-PDF converter. The main value is:

- very small packaged binaries
- fast startup and conversion
- low memory use for normal documents
- no browser, LaTeX, or Pandoc dependency on the native fast path
- a polished Windows Dear ImGui app plus a compact cross-platform CLI

Treat the native renderer as a fast Markdown subset, not a full Pandoc clone.
Pandoc compatibility may exist as an optional mode, but it must not become a
required dependency for the default lightweight package.

## Current Feature Surface

Native PDF mode supports the core document features listed in `README.md`,
including headings, paragraphs, lists, block quotes, fenced code blocks, pipe
tables, rule lines, simple math cleanup/boxes, inline emphasis cleanup,
clickable Markdown links, and standalone local or HTTP/HTTPS images with
fallback text.

Important image/link details:

- Local image paths are resolved relative to `TinyPdf::BuildOptions::sourcePath`
  when the caller provides an input file path.
- URL images are controlled by `BuildOptions::enableUrlImages`.
- Windows image support uses WinHTTP/WIC through the Win32 build.
- Linux URL images use libcurl when `FAST_MARKDOWN_USE_CURL` is defined.
- PNG alpha support can use zlib when `FAST_MARKDOWN_USE_ZLIB` is defined.
- Failed images should degrade to useful fallback text instead of failing the
  whole conversion.
- Links are emitted as PDF annotations; keep visible text and annotation rects
  aligned when changing wrapping or text layout.

## Architecture Map

- `include/fast_markdown/tiny_pdf.h`
  Public native exporter API. Keep this small and stable.

- `src/core/tiny_pdf.cpp`
  Portable Markdown parser, native PDF writer, font handling, image decoding,
  image cache, link annotations, and renderers. This is the performance-critical
  engine.

- `src/cli/main_cli.cpp`
  Portable CLI entry point for Linux and non-GUI workflows. Supports single
  export, folder batch, stdin batch, warm serve mode, and benchmarks.

- `src/win32/main_win32.cpp`
  Windows Dear ImGui + DirectX 11 app, Windows CLI glue, drag/drop, Pandoc mode,
  and native export integration.

- `src/win32/fast_markdown.rc`
  Windows resources, including the app icon.

- `CMakeLists.txt`
  Cross-platform build. Windows builds `fast-markdown-imgui`; non-Windows builds
  `fast-markdown`.

- `imgui/`
  Vendored Dear ImGui. Do not edit vendored ImGui files unless the task
  explicitly requires it.

- `third_party/simdutf/`
  Optional vendored simdutf experiment. It is OFF by default because prior
  benchmarks were mixed or slower.

- `tester.md`
  Main hand-written smoke/regression document. It covers Unicode text, inline
  styles, code, math, tables, nested lists, block quotes, links, local images,
  remote images, and failed image fallbacks.

- `scripts/`
  Verification and benchmark helpers. Prefer adding reproducible scripts here
  instead of one-off command blobs.

- `docs/brand_asset_prompt.md`, `catto.png`, `catto.ico`
  Project mascot/branding assets.

## Build And Verify

Windows release build:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

Linux release build:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

Linux verification:

```sh
sh scripts/verify-linux.sh
```

Native benchmark examples:

```sh
build/linux/fast-markdown --bench tester.md benchmark-output/manual 1000 modern normal
build/windows/fast-markdown-imgui.exe --bench tester.md benchmark-output/manual 1000 modern normal
```

Commercial/stress benchmark helpers:

```sh
python3 scripts/stress_benchmark.py --help
python3 scripts/commercial_benchmark.py --help
```

When performance is part of the task, record before/after numbers. Do not keep a
change that improves one tiny case but clearly regresses larger documents,
batching, or memory without calling out the tradeoff.

## Performance Guardrails

- Keep the default native path dependency-light.
- Avoid adding large runtimes, browser engines, TeX stacks, or heavy document
  libraries to the native package.
- Reuse output buffers in batch/server paths where practical.
- Be careful with per-line/per-span heap allocation in `tiny_pdf.cpp`.
- Prefer measured changes over plausible micro-optimizations.
- Keep image caches bounded. Image support can dominate memory on large or many
  remote images.
- Do not turn optional experiments such as simdutf ON by default without fresh,
  broad benchmark evidence.
- Linux benchmarks are much faster on native/ext4 storage than on `/mnt/*` WSL
  mounts. State the storage location when reporting Linux benchmark numbers.

## Correctness Guardrails

- Native mode should fail gracefully. Missing images, unsupported image formats,
  and unavailable URLs should produce fallback text, not crash.
- Keep ASCII documents on the standard-font path when possible.
- Preserve Unicode output by loading/subsetting a system font when needed.
- Keep PDF syntax valid: object ids, xrefs, page resources, image XObjects, and
  link annotations must remain consistent.
- `BuildOptions::sourcePath` matters for relative images; pass it from every
  file-based caller.
- Do not report native mode as full CommonMark, full Pandoc, full LaTeX math, or
  syntax-highlight compatible unless those features are actually implemented.

## UI Guidance

The Windows app should feel like a compact utility, not a landing page.

- Keep the main workflow immediate: edit/import Markdown, choose engine/style/
  margin, export.
- Preserve keyboard export behavior and drag/drop file loading.
- Use Dear ImGui idioms and keep custom drawing localized in `main_win32.cpp`.
- Avoid UI changes that make the binary much larger or require shipping assets
  beyond the icon/mascot unless the value is clear.
- The icon source is `catto.ico`; `catto.png` is the transparent source graphic
  used in docs/branding.

## Packaging And Licensing

- Default Windows release should be a single `fast-markdown-imgui.exe` where
  possible.
- Default Linux release should be a compact `fast-markdown` CLI binary.
- Do not bundle Pandoc unless deliberately producing a larger compatibility
  package and accounting for Pandoc's GPL license terms.
- Keep Dear ImGui license attribution intact.
- Keep generated build trees, benchmark outputs, PDFs, and temporary corpora out
  of source unless the user explicitly asks to track a specific artifact.

## Documentation And Benchmarks

- Update `README.md` when user-visible behavior, commands, build requirements,
  or supported Markdown features change.
- Keep benchmark claims conservative and reproducible.
- `commercial_benchmark_summary.md` contains marketing-safe wording and caveats.
  Do not copy raw headline numbers without the caveats about synthetic corpora,
  warm `--bench` versus end-to-end I/O, and Linux storage location.

## Change Style

- Match the existing C++17 style.
- Keep changes scoped to the touched subsystem.
- Prefer standard library and platform APIs already in use over new dependencies.
- Add small helper functions when they reduce repeated PDF/string/layout logic.
- Avoid broad refactors unless needed for a measured performance or correctness
  issue.
- Protect user changes in the working tree. Check `git status --short` before
  editing and do not revert unrelated work.
