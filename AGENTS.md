# RayoMD Agent Guide

Read this file before changing the project. It records the product goal,
architecture, release hygiene, and benchmark guardrails that are easy to lose
between sessions.

## Product Goal

RayoMD is a tiny native Markdown-to-PDF converter. The default value is:

- very small packaged binaries
- fast startup and conversion
- low memory use for normal documents
- no browser, LaTeX, or Pandoc dependency on the native fast path
- a polished Windows Dear ImGui app plus a compact cross-platform CLI

Treat the native renderer as a fast Markdown subset, not a full Pandoc clone.
Pandoc compatibility may exist as an optional Windows mode, but it must not
become a required dependency for the default lightweight package.

## Release Readiness Priorities

- Keep the repository root clean: source entry points, `README.md`, `LICENSE`,
  `CMakeLists.txt`, `AGENTS.md`, `CONTRIBUTING.md`, `VERSION`, and the main
  smoke document belong there.
- Keep source-coupled fixtures, release records, specifications, and decision
  documents under docs/; keep longer user guides, dated reports, and research
  archives in the GitHub wiki.
- Keep generated build trees, generated PDFs, benchmark corpora, local binaries,
  caches, and one-off scratch files out of source.
- Prefer reproducible helper scripts under `scripts/` over ad hoc command blobs.
- Keep README claims accurate, conservative, and easy to reproduce.
- Do not advertise native mode as full CommonMark, Pandoc, LaTeX math, or
  syntax-highlight compatible unless those features are actually implemented.

## Current Feature Surface

Native PDF mode supports the core document features listed in `README.md`,
including headings, paragraphs, lists, block quotes, fenced code blocks, pipe
tables, rule lines, simple math cleanup/boxes, inline emphasis cleanup,
clickable Markdown links, standalone local images, and HTTP/HTTPS images on
Windows or curl-enabled Linux builds with fallback text.
Native exports can opt into the `rayomd-source/1` reversible PDF profile.
Embedding is disabled by default because it exposes the complete source,
including content not visible on rendered pages. Recovery is byte-exact and
format-specific; it must never silently fall back to heuristic PDF conversion.
Keep ordinary non-reversible exports on their unchanged PDF 1.7 fast path.
The profile limits PDFs to 256 MiB and source to 10 MiB. The source cap is
based on the measured maximum exact-recovery case; reject larger inputs before
rendering to avoid excessive output and peak memory.

Important image/link details:

- Local image paths are resolved relative to `TinyPdf::BuildOptions::sourcePath`
  when the caller provides an input file path.
- URL images are controlled by `BuildOptions::enableUrlImages`.
- Windows image support uses WinHTTP/WIC through the Win32 build.
- Linux URL images use libcurl when `RAYOMD_USE_CURL` is defined.
- PNG alpha support can use zlib when `RAYOMD_USE_ZLIB` is defined.
- Failed images should degrade to useful fallback text instead of failing the
  whole conversion.
- Links are emitted as PDF annotations; keep visible text and annotation rects
  aligned when changing wrapping or text layout.

## Architecture Map

- `include/rayomd/tiny_pdf.h`
  Public native exporter API. Keep this small and stable.

- `src/core/tiny_pdf.cpp`
  Native PDF assembly, font handling, image decoding/cache, link annotations, and the
  standard/Unicode renderers. This remains the performance-critical export facade.

- `src/core/markdown_parser.cpp`
  Internal Markdown block model and parser; keep renderer-independent document cleanup here.

- `src/core/rayomd_pdf_source.h` and `src/core/rayomd_pdf_source.cpp`
  Bounded reversible-profile metadata, SHA-256 integrity, hostile-input
  inspection, and byte-exact source recovery. Keep this limited to RayoMD's
  exact classic-xref profile rather than growing a general PDF parser.

- `src/core/inline_markdown.cpp`
  Renderer-neutral inline span parsing for emphasis, code, images, and links. Both renderers
  consume this model so visible text and link annotations cannot drift.

- `src/core/export_options.cpp` and `src/common/text_utils.cpp`
  Shared typed style/margin conversion, CLI option parsing primitives, and non-public text helpers.

- `src/cli/main_cli.cpp`
  Portable CLI entry point for Linux and non-GUI workflows. Supports single
  export, stdin Markdown export, folder batch, stdin batch, warm serve mode,
  and benchmarks.

- `src/win32/main_win32.cpp`
  Windows Dear ImGui + DirectX 11 app, Windows CLI glue, drag/drop, Pandoc mode,
  and native export integration.

- `src/win32/rayomd.rc`
  Windows manifest and app icon resources. Keep resource changes localized here.

- `CMakeLists.txt`
  Cross-platform build. Windows and non-Windows builds produce `rayomd`
  (`rayomd.exe` on Windows).

- `VERSION`
  Single source for the release version. CMake compiles it into both command-line
  entry points; update it only for release/versioning work.

- `CONTRIBUTING.md`
  Short contributor guide with project priorities, build/verify commands,
  performance expectations, and versioning rules.

- `third_party/imgui/`
  Vendored Dear ImGui, currently v1.92.8. Do not edit vendored ImGui files
  unless the task explicitly requires it.

- `third_party/simdutf/`
  Optional vendored simdutf experiment. It is OFF by default because measured
  results were mixed or slower.

- `tester.md`
  Main hand-written smoke/regression document. It covers Unicode text, inline
  styles, code, math, tables, nested lists, block quotes, links, local images,
  remote images, and failed image fallbacks.

- tests/verify_cli.py
  Cross-platform black-box correctness verification for an already-built binary.
  Keep building and timing outside this verifier.

- tools/benchmark.py
  Maintained entry point for run, compare, release, and competitor performance
  workflows. Focused implementations remain under scripts/.
- `.github/workflows/`
  GitHub Actions for Linux/Windows CI, repository hygiene, and CodeQL. Keep
  README badges aligned with workflow filenames when adding or renaming checks.

- docs/assets/branding/ and docs/assets/demo/
  Project branding sources and demo media, kept separate.

- `docs/benchmarks/releases/`
  Script-consumed release benchmark records and their generated index.

- `docs/development/`
  Source-coupled performance workflow, format profiles, and architecture decisions.

- [GitHub wiki](https://github.com/Butterski/rayomd/wiki)
  User guides, dated benchmark reports, and optimization research. Wiki history
  is context, not a mandate to keep old experiments alive.

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

The default Linux build intentionally avoids a libcurl runtime dependency. Use
`-DRAYOMD_USE_CURL=ON` only when producing a Linux build that should fetch
HTTP/HTTPS images and can target a known distro/libcurl baseline.

Cross-platform CLI verification (after building):

```sh
python3 tests/verify_cli.py --binary build/linux/rayomd
python tests/verify_cli.py --binary build/windows/rayomd.exe
```

Native benchmark examples:

```sh
build/linux/rayomd --bench tester.md benchmark-output/manual 1000 modern normal
build/windows/rayomd.exe --bench tester.md benchmark-output/manual 1000 modern normal
```

Performance watcher examples:

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-wsl --suite watch --label local
```

When performance is part of the task, record before/after numbers. Do not keep a
change that improves one tiny case but clearly regresses larger documents,
batching, or memory without calling out the tradeoff.

GitHub CI entry points:

- `.github/workflows/ci.yml`
  Builds and smokes Linux native CLI plus Windows MinGW ImGui app.

- `.github/workflows/hygiene.yml`
  Checks release files, script syntax, local README targets, and generated
  output exclusions.

- `.github/workflows/codeql.yml`
  Runs C++ CodeQL analysis on the Linux native build path.

- `.github/workflows/release.yml`
  Packages default Linux, curl-enabled Linux, and Windows artifacts for matching version
  bumps, `v<VERSION>` tags, and manual dispatch; keep `VERSION` and package contents aligned.

## Benchmark And Documentation Rules

- Update `README.md` when user-visible behavior, commands, build requirements,
  supported Markdown features, or benchmark workflows change.
- Update `VERSION` and the README version badge together when preparing a
  release.
- Keep benchmark claims dated and scoped.
- Distinguish warm `--bench` timings from end-to-end export and batch timings.
- Mention Linux storage location when reporting WSL/Linux batch numbers.
- Keep raw generated reports under ignored `benchmark-output/`, not source.
- The wiki commercial benchmark campaign is archival
  marketing-safe wording and caveats. Do not copy raw headline numbers without
  the caveats about synthetic corpora, warm `--bench` versus end-to-end I/O,
  and Linux storage location.

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
- Keep visible link text and annotation rectangles aligned when changing text
  wrapping, painting, or page splitting.

## UI Guidance

The Windows app should feel like a compact utility, not a landing page.

- Keep the main workflow immediate: edit/import Markdown, choose engine/style/
  margin, export.
- Preserve keyboard export behavior and drag/drop file loading.
- Use Dear ImGui idioms and keep custom drawing localized in `main_win32.cpp`.
- Avoid UI changes that make the binary much larger or require shipping assets
  beyond the icon/mascot unless the value is clear.
- The icon source is `docs/assets/branding/rayomd.ico`; `docs/assets/branding/rayomd.png` is the
  transparent source graphic used in docs/branding.

## Packaging And Licensing

- Default Windows release should be a single `rayomd.exe` where
  possible.
- Default Linux release should be a compact `rayomd` CLI binary.
- RayoMD's own code is licensed under Apache-2.0. Keep `LICENSE`,
  `NOTICE`, README license wording, and SPDX headers aligned when changing
  licensing metadata.
- Do not bundle Pandoc unless deliberately producing a larger compatibility
  package and accounting for Pandoc's GPL license terms.
- Keep Dear ImGui license attribution intact.
- Keep generated build trees, benchmark outputs, PDFs, and temporary corpora out
  of source unless the user explicitly asks to track a specific artifact.

## Change Style

- Match the existing C++17 style.
- Keep changes scoped to the touched subsystem.
- Prefer standard library and platform APIs already in use over new
  dependencies.
- Add small helper functions when they reduce repeated PDF/string/layout logic.
- Avoid broad refactors unless needed for a measured performance or correctness
  issue.
- Use structured parsers/APIs when available instead of ad hoc string surgery.
- Protect user changes in the working tree. Check `git status --short` before
  editing and do not revert unrelated work.
