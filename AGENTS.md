# RayoMD Agent Guide

Read this file before changing the project. It records the product goal,
architecture, release hygiene, and benchmark guardrails that are easy to lose
between sessions.

## Product Goal

RayoMD is a tiny native Markdown-to-PDF converter. The default value is:

- very small packaged binaries
- fast startup and conversion
- low memory use for normal documents
- no browser, LaTeX, or Pandoc dependency
- a polished Windows Dear ImGui app plus a compact cross-platform CLI

Treat the native renderer as a fast Markdown subset, not a full Pandoc clone.

## Release Readiness Priorities

- Keep the repository root clean: source entry points, `README.md`, `LICENSE`,
  `CMakeLists.txt`, `AGENTS.md`, `CONTRIBUTING.md`, `VERSION`, and the main
  smoke document belong there.
- Keep source-coupled fixtures, specifications, and decision documents under
  docs/; keep longer user guides, dated reports, benchmark results, and research
  archives in the GitHub wiki. Release benchmark records are not kept in the
  repository.
- Keep generated build trees, generated PDFs, benchmark corpora, local binaries,
  caches, and one-off scratch files out of source.
- Prefer reproducible helper scripts under `scripts/` over ad hoc command blobs.
- Keep README claims accurate, conservative, and easy to reproduce.
- Do not advertise native mode as full CommonMark, Pandoc, LaTeX math, or
  syntax-highlight compatible unless those features are actually implemented.

## Current Feature Surface

Native PDF mode supports the core document features listed in `README.md`,
including headings, paragraphs, lists (with GitHub task lists), block quotes and
GitHub alerts, fenced code blocks, pipe tables (whose header row repeats on every
page), rule lines, natively typeset math for a TeX subset, inline emphasis
cleanup, clickable Markdown links (also bare URLs, email addresses and shortcut
references), bookmarks and `#anchor` links from headings, HTML comments, `<br>`
and character references, document metadata from the front matter (`/Info` title,
author, subject and keywords, the catalog's `/Lang`; `FrontMatterMetadata` reads
the YAML subset), opt-in page numbers, page sizes (A4 by default, presets,
landscape and custom sizes; the renderers' `pageW`/`pageH`, never constants),
standalone local images (PNG, JPEG, and SVG drawn as vectors), and HTTP/HTTPS
images on Windows or curl-enabled Linux builds with fallback text.
Footnotes as on GitHub (`[^label]` and `[^label]:` definitions, also in quotes and
list items): numbered by first reference in reading order, drawn as raised link
numbers (one `MathPool` box each, so they take the cold formula paths and the fast
paragraph path stays as it was), and set after the text at 0.85 of the body size,
each note's number linking back. Only `ParseMarkdown(markdown, &footnotes)` reads
them; `footnotes.cpp` holds their numbering, kept out of the parser's translation
unit, where it moved GCC's inlining of the line classifier. A table with a reference
shows its cells as plain text, as one with a formula does. Without definitions the
PDF bytes stay as they are.
Fenced code whose info string names a language GitHub knows (29 lexers: shell and
console, PowerShell, batch, JavaScript and TypeScript, Python, C, C++, C#, Java, Kotlin,
Go, Rust, Swift, PHP, Ruby, SQL, HTML and XML, CSS and SCSS, JSON, YAML, TOML, INI,
Dockerfile, diff) is drawn in the colours of GitHub's light theme; `--no-highlight`
(`PdfOptions::highlightCode`) and blocks in no known language keep the one-colour path
and its bytes. The lexers in `highlight.cpp` read every byte once in one forward pass;
every rule is bounded, so time stays linear for any input. Keep it that way: a new rule
that scans ahead must stop at the end of its line or mark what it scanned, never rescan.
A table of contents comes at the first paragraph `[TOC]` or `[[_TOC_]]` (later ones draw
nothing), or with `--toc` (`PdfOptions::toc`) first or after a title heading; it lists
the top-level headings to `--toc-depth` levels, without a document's only level-1 heading
when that comes first and without headings of footnote references alone. The renderers
lay out its entries in the flow and link them to the headings; the builder adds the page
numbers and the dot leaders afterwards as a content stream of each table page's own,
beside a theme's header and footer, so one layout pass suffices. A TOC entry finds its
heading by the address of the heading's text, which `HeadingMark::text` keeps.
Books (`--book`, `BuildBookPdf`) make one PDF of the files a SUMMARY.md lists (mdBook,
GitBook), of a folder, or of the files given. Each file is parsed on its own (its link
definitions, footnotes and heading anchors are its own) and begins on a new page, so the
page of a link, a heading mark or a note mark tells its file: `BookTargets` resolves
`#fragment` within the file and `b.md#fragment` across files with no file field on the
renderers' structures. Single documents pass `book == nullptr` through the builders and
must keep their bytes; book-only work stays in `RAYOMD_COLD` code and `book.cpp`.
Opt-in compression (`--compress`, `PdfOptions::compress`) writes page content, the
font program and both CMaps as FlateDecode where that makes the file smaller, with
RayoMD's own DEFLATE encoder; never the XMP metadata or an embedded source, which
the reversible profile reads uncompressed. Without it the PDF bytes stay as they
are.
Company themes (`--theme=FILE`, `PdfOptions::theme`): a TrueType or OpenType font for all
text (no fallback font then), with optional bold, italic and bold italic faces (`/F2`, `/F4`,
`/F5`; the Unicode renderer's `faces`, `ChooseFace`): with any of them `multiFace` is set,
paragraphs take the general `WrapStyled` path, and every width and paint of styled text goes
through `SpanFont`/`PaintSpanText`, so measure and paint use the same face; without them every
one of those calls is `font`/`PaintText` as before. Heading, link and accent colours (the renderers'
`headingColor`/`linkColor`/`ruleColor`/`plainQuoteBar`, from `ThemePalette`, never
literals), header and footer fields with placeholders and a logo (one overlay stream
per page, like the page numbers, whose glyphs join the subset before it is cut), and a
cover page before the first page. Without a theme the PDF bytes stay as they are.
PDF/A-3b (`--pdfa`, `PdfOptions::pdfa`) takes the Unicode renderer for all text, as
every font must be embedded: page numbers become a footer band in the document's
font, formulas are typeset in the embedded formula fonts as in any export, and without
a TrueType or OpenType font the export fails
(`PdfaFontUnavailable`) instead of falling back to the standard fonts. It adds an
sRGB output intent, XMP metadata that tells what the Info dictionary does
(`PdfaText` makes both hold the same text), a trailer `/ID` and `/F 4` on links,
and stays PDF 1.7 with an embedded source. Check changes with veraPDF
(`verapdf -f 3b`). Without it the PDF bytes stay as they are.
`PdfOptions::pdfua` (`--pdfua`) writes tagged PDF/UA-1: the fonts as with `pdfa`, formulas
as their TeX source (`MathPool::Disable`: they have no structure tags yet), and a structure
tree (`src/core/tagged.h`) that the Unicode renderer fills as it
paints. Every paint of that renderer goes through it: `PaintText` and `DrawImage` mark
content of the open element, `DrawRect`, `DrawStrokeRect` and rules mark artifacts, and
every block opens its element. A new paint call in the Unicode renderer must do the same,
or veraPDF fails with "content neither marked as Artifact nor tagged" (`verapdf -f ua1`),
and Debug builds assert. Untagged documents keep their bytes: each hook tests `tags` first,
and the tagged-only work is in `RAYOMD_COLD` functions and `tagged.cpp`.
Native exports can opt into the `rayomd-source/1` reversible PDF profile.
Embedding is disabled by default because it exposes the complete source,
including content not visible on rendered pages. Recovery is byte-exact and
format-specific; it must never silently fall back to heuristic PDF conversion.
Keep ordinary non-reversible exports on their unchanged PDF 1.7 fast path.
The profile limits PDFs to 256 MiB and source to 10 MiB. The source cap is
based on the measured maximum exact-recovery case; reject larger inputs before
rendering to avoid excessive output and peak memory.

Important math details (contract: `docs/development/native_math.md`):

- Math is a TeX subset, not LaTeX. Unknown or malformed input degrades to readable
  output; every hard limit shows the formula's complete source. It must never
  crash, hang, or fail the export.
- Math must not change the PDF bytes of a document without math syntax;
  `CheckNoMathGolden` in `tests/core_tests.cpp` pins them. Re-pin it only for a
  deliberate plain-text output change and say why. Formula-bearing text
  takes separate wrap and paint functions (`RAYOMD_MATH_COLD`) so the plain paths
  stay untouched.
- Math is laid out on the Times and Symbol AFM metrics and painted in five embedded
  fonts (`/M1`..`/M5`, RayoMDMath, TeX Gyre Termes outlines at those advances), each
  added only when a formula painted one of its glyphs and holding only those
  (`MathGlyphUse`, `BuildMathFaceFont`). ASCII documents with math stay on the
  standard-font path.
- `src/core/math_symbols.inc`, `src/core/math_font_metrics.inc` and
  `src/core/math_glyphs.inc` are generated by `scripts/generate_math_tables.py`; never
  edit them by hand. Every code a formula can paint must have a glyph there
  (`paintable_slots` in the generator; `CheckMathLayout` checks all five faces).

Important image/link details:

- Local image paths are resolved relative to `TinyPdf::BuildOptions::sourcePath`
  when the caller provides an input file path.
- URL images are controlled by `BuildOptions::enableUrlImages`.
- Windows image support uses WinHTTP/WIC through the Win32 build.
- Linux URL images use libcurl when `RAYOMD_USE_CURL` is defined.
- PNGs with transparency are inflated by RayoMD's own inflater (`inflate.cpp`) and
  re-encoded by its own encoder (`flate.cpp`), on every platform; no zlib or libdeflate.
- Such a PNG of 32,768 pixels or more is kept between processes in
  `PdfOptions::imageCacheDirectory`, a file per SHA-256 of the PNG (SHA-NI where the
  CPU has it). The library default is off; the CLI and the Windows app use the user
  cache directory unless `--no-image-cache`; `--bench` never does. An entry holds the
  RayoMD version and, in `kMagic` of `src/core/image_cache.cpp`, the encoding's
  generation: raise it with any change to the bytes `BuildDecodedPngImage` writes, and
  compare image output between builds with `--no-image-cache`, or one build reads the
  other's streams.
- Failed images should degrade to useful fallback text instead of failing the
  whole conversion.
- An SVG image (a file or URL) is converted by `src/core/svg.cpp`, without a
  dependency, into a form XObject for each document, as its text uses the
  document's fonts; such forms never enter the cross-document image cache. One
  that needs what the converter does not draw (HTML, scripts, style sheets it
  cannot match) or hits a limit shows its alt text. Keep every limit failing
  closed: SVG input is untrusted.
- Links are emitted as PDF annotations; keep visible text and annotation rects
  aligned when changing wrapping or text layout.
- Headings make the PDF outline, and `#anchor` links go to them by GitHub's
  anchor rules (`HeadingTargets`) with explicit destinations; a link to a missing
  anchor is dropped. Renderers note a heading where its first line lands
  (`MarkHeading`, next to that line's `Ensure`): keep them together when
  changing heading layout. A heading keeps room for what follows it on its page
  (`HeadingKeep`), and a table's header row repeats after a page break (the row
  loop goes back to row 0): keep link rectangles on the page they are drawn on.

## Architecture Map

- `include/rayomd/tiny_pdf.h`
  Public native exporter API. Keep this small and stable.

- `src/core/tiny_pdf.cpp`
  Native PDF assembly, font handling, image decoding/cache, link annotations, and the
  standard/Unicode renderers. This remains the performance-critical export facade.

- `src/core/markdown_parser.cpp`
  Internal Markdown block model and parser; keep renderer-independent document cleanup here.

- `src/core/footnotes.h` and `src/core/footnotes.cpp`
  Footnotes in the parser: reading a definition's lines, the numbering by first
  reference once the document is read, and the plain text with reference markers of
  headings and tables that waited for it (`Block::notesPending`). Built at `-Os`.

- `src/core/highlight.h` and `src/core/highlight.cpp`
  Syntax highlighting: the language an info string names (a table made at compile
  time), the lexers that give each byte of a code block a token class, and the colours.
  Built at `-O2`: per-byte code, 1.5 times as fast as at `-Os`. The renderers split a
  line into runs of one colour (`SplitCodeRuns`) and draw them in one text object.

- `src/core/contents.h` and `src/core/contents.cpp`
  The table of contents: turning the marker paragraph into a `BlockType::Contents` block
  (or inserting one for `--toc`) and listing the headings it shows, also a book's. Built
  at `-Os`; the marker scan runs for every document, so it looks at a block's text length
  first.

- `src/core/book.h` and `src/core/book.cpp`
  Books: reading `--book`'s inputs for both command lines (SUMMARY.md as mdBook and
  GitBook read it, a folder, files) and parsing a book's files for `BuildBookPdf`. Built at
  `-Os`, outside `tiny_pdf.cpp` and its inlining budget.

- `src/core/svg.h` and `src/core/svg.cpp`
  SVG to a PDF form XObject: a non-recursive XML reader, the CSS cascade for
  presentation attributes, `style` and simple selectors, paths, shapes, clips,
  linear gradients, `use`, text in the host's font and `data:` images through the
  host (`SvgHost`, implemented by `tiny_pdf.cpp` for each renderer). Built at `-Os`;
  every input-size, depth, work and output limit makes `ConvertSvg` return false.

- `src/core/cff.h` and `src/core/cff.cpp`
  OpenType fonts with PostScript outlines: the 'CFF ' table checked once when the font
  loads (INDEXes, DICTs, FDSelect, private DICTs and subroutines), each glyph's Type 2
  charstring with its subroutines inlined (`Desubroutinize`, bounded in depth, bytes and
  work), and the font programs written of it: a CID-keyed subset whose CIDs are the code
  points (FontFile3 /CIDFontType0C, no CIDToGIDMap) and simple fonts with a built-in
  encoding. Built at `-Os`; every read is bounds-checked, as font files are untrusted.

- `src/core/tagged.h` and `src/core/tagged.cpp`
  PDF/UA's structure tree: elements opened and closed as the renderer paints, marked
  content and artifacts in the page streams, lists, tables and links tracked across
  blocks and pages, and the tree's objects written into one buffer the PDF views
  (StructTreeRoot, elements, ParentTree, IDTree). Built at `-Os`; elements hold no
  heap memory of their own.

- `src/core/rayomd_pdf_source.h` and `src/core/rayomd_pdf_source.cpp`
  Bounded reversible-profile metadata, SHA-256 integrity (SHA-NI where the CPU has it,
  checked against the portable code by the core tests), hostile-input inspection, and
  byte-exact source recovery. Keep this limited to RayoMD's exact classic-xref profile
  rather than growing a general PDF parser.

- `src/core/image_cache.h` and `src/core/image_cache.cpp`
  The persistent cache of re-encoded PNGs: entry format, checks, atomic writes and
  least-recently-used trimming. Cold code, kept out of `tiny_pdf.cpp`, whose inline
  budget it would otherwise take from hot calls.

- `src/core/inline_markdown.cpp`
  Renderer-neutral inline span parsing for emphasis, code, images, links, and math
  delimiters. Both renderers consume this model so visible text and link annotations
  cannot drift. `ParseInlineRuns` is the parser (flat runs for the hot paths);
  `ParseInlineSpans` is the same result as strings, for tests and cold paths.

- `src/core/math_parser.h` and `src/core/math_parser.cpp`
  Tokenizer, parser, error recovery, and limits for the TeX math subset; produces a
  bounded node arena. Uses the generated `src/core/math_symbols.inc`.

- `src/core/math_layout.h` and `src/core/math_layout.cpp`
  Renderer-neutral TeX-like box layout, draw list, PDF operator emission, and the
  embedded formula fonts (`BuildMathFaceFont`). Uses the generated
  `src/core/math_font_metrics.inc` and `src/core/math_glyphs.inc`. Cold code,
  compiled with `-Os`. Also exports `kStandardWordAdvances`, the exact AFM widths the
  standard-font renderer wraps and paints with.

- `scripts/generate_math_tables.py`
  Regenerates the math tables from the Adobe Core 14 AFM files and, with `--glyph-dir`,
  the formula glyphs from TeX Gyre Termes (fontTools; inputs not stored in the
  repository, pinned by SHA-256). TeX Gyre's GUST Font License is in
  `third_party/tex-gyre/`.

- `src/core/export_options.cpp` and `src/common/text_utils.cpp`
  Shared typed style/margin conversion, CLI option parsing primitives (also theme
  files, `ParseTheme`), and non-public text helpers. `export_options.cpp` is built at
  `-Os`: it runs once per run.

- `src/core/png_filter.cpp`
  PNG row filters: undoing them when a PNG is decoded, choosing and applying them when
  its pixels are re-encoded for a PDF. Choosing and applying in AVX2 where the CPU has it
  (GCC/Clang `target("avx2")`, chosen once), else SSE2 on x86-64, with the scalar
  functions kept as the reference and the fallback; core check 99 compares the dispatched,
  the SSE2 and the scalar code with the textbook filters.

- `src/common/file_io.cpp`
  Whole-file reads and writes through C stdio (`RayoMd::File`), wide paths on Windows. On POSIX,
  `WriteAll` overwrites an existing file in place and cuts it to size (no `O_TRUNC`): truncating
  an ext4 file to zero made its close start writeback, 0.1 ms per export. Keep it so; the cost
  is that a crash can leave old and new bytes mixed.

- `src/common/exact_math.h` and `src/common/exact_math.cpp`
  `ceil`, `floor`, `trunc`, `lround`, `llround`, `sqrt` and `fmod` without libm, bit for bit as
  glibc's (the core tests compare them), so the dynamic Linux build needs no `libm.so`. Use them,
  never `<cmath>`'s or other libm calls, in RayoMD's code: one direct call puts libm back into
  every process (`readelf -d build/linux/rayomd` must list libc only). `svg.cpp`'s `atan2`,
  `sincos` and `tan` are loaded from libm.so.6 on first use; a new transcendental function goes
  there too.

- `src/hot.ld` and `scripts/hot_sections.py`
  The order in which GNU ld places the code fresh exports run (Release, Linux, dynamic and
  static). Generated, never edited by hand; regenerate with the script (its docstring has the
  commands) after larger changes to the code every export runs. Stale names are harmless.

- `src/core/inflate.h` and `src/core/inflate.cpp`
  RayoMD's zlib/DEFLATE decoder (`ZlibReader`): resumable output a piece at a time
  through a window, two-level decode tables, a fast loop that looks up the next code
  before its refill. It accepts exactly what zlib's `uncompress()` accepts for an output
  of known size; the core tests hold it to libdeflate on mutated streams.

- `src/core/flate.h` and `src/core/flate.cpp`
  The DEFLATE/zlib encoder behind `--compress` and every re-encoded image: greedy
  matching over two hash tables, per-block stored/fixed/dynamic choice by exact bit
  count, SSE2 or AVX2 Adler-32. Image rows go through `DeflateImageSegment` (the 8-byte table
  only, segments of `ImageSegmentSize` with `kImageSegmentHistory` bytes of history,
  optionally waiting for input another thread is still writing); `ZlibCompressImage` is
  the same stream made on one thread. Output depends only on the input. Its tables live
  per thread and are never cleared between streams. Built at `-O2`: within 1 % of `-O3`
  at half the code.

- `src/core/image_encoder.h` and `src/core/image_encoder.cpp`
  `ImagePlanesEncoder`: the colour and alpha planes of a re-encoded PNG compressed in
  segments while they are decoded, by a process-wide pool of up to three helper threads
  and the decoding thread; the bytes do not depend on how many threads take part.

- `src/core/theme.h` and `src/core/theme.cpp`
  The layout of a theme's header, footer and cover page (placeholders, ellipses,
  wrapping) on UTF-8 text, behind `ThemeTextFont`, which each renderer implements in
  `tiny_pdf.cpp`. Built at `-Os`, and kept out of `tiny_pdf.cpp` on purpose (see the
  inlining guardrail below).

- `src/core/pdfa.h` and `src/core/pdfa.cpp`
  What PDF/A-3b adds: the 480-byte sRGB ICC profile (a v4.2 display profile written
  as data, SHA-256 in the source), the XMP packet with the PDF/A identification and,
  with an embedded source, the reversible profile's properties and their extension
  schema, and the file identifier, a 128-bit hash of the body. Built at `-Os`.

- `src/common/batch_report.cpp`
  What both command lines share for batch export: the JSON Lines report (one
  line per document, written by the worker that finished it), the up-to-date
  check behind `--skip-unchanged` (PDF newer than its Markdown and ending in
  `%%EOF`) and the recursive collection behind `--recursive`.

- `src/cli/main_cli.cpp`
  Portable CLI entry point for Linux and non-GUI workflows. Supports single
  export, stdin Markdown export, folder batch, stdin batch, warm serve mode,
  books, and benchmarks.

- `src/win32/main_win32.cpp`
  Windows Dear ImGui + DirectX 11 app, Windows CLI glue, drag/drop,
  and native export integration. Its batch modes mirror `main_cli.cpp`; change
  both. A MinGW cross-build runs under Wine for local checks of the CLI glue.

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

- `third_party/libdeflate/`
  libdeflate v1.26's decompressor, the files it needs and unchanged (MIT): the core
  tests' independent reference for RayoMD's inflater and encoder. No executable links
  it (`EXCLUDE_FROM_ALL`). Its compressor is not vendored.

- `tester.md`
  Main hand-written smoke/regression document. It covers Unicode text, inline
  styles, code, math, tables, nested lists, block quotes, links, local images,
  remote images, and failed image fallbacks.

- tests/verify_cli.py
  Cross-platform black-box correctness verification for an already-built binary.
  Keep building and timing outside this verifier.

- tests/core_tests.cpp
  Unit and PDF-level tests linked against the core sources. The math checks live in
  `tests/math_markdown_tests.inc`, `tests/math_parser_tests.inc`,
  `tests/math_layout_tests.inc`, and `tests/no_math_golden.inc`, which it includes;
  `tests/inline_lookahead_tests.inc` holds the checks of the inline parser's
  look-ahead tables.

- tools/benchmark.py
  Maintained entry point for run, A/B (warm engine and fresh process),
  competitor, and reversible-profile performance workflows. Focused
  implementations remain under scripts/.
- `.github/workflows/`
  GitHub Actions for Linux/Windows CI, repository hygiene, and CodeQL. Keep
  README badges aligned with workflow filenames when adding or renaming checks.

- docs/assets/branding/ and docs/assets/demo/
  Project branding sources and demo media, kept separate.

- `docs/development/`
  Source-coupled performance workflow, format profiles, the native math contract, and
  architecture decisions.

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
HTTP/HTTPS images and can target a known distro/libcurl baseline. Release builds link
libstdc++ and libgcc statically and do without libm (`exact_math.h`), so the binary needs only libc.
`-DRAYOMD_STATIC=ON` links glibc in as well: the `-static` release asset, about
0.9 MB larger and 0.3 ms faster to start, with no runtime dependency.

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

- `README.md` describes every user-visible feature in its Features section, each with a
  short, working example; keep that section in step when a feature is added or changes.
  Complete flag tables, exit codes, build variants, API details and benchmark methodology
  stay in the GitHub wiki.
- When user-visible behavior changes, update the relevant wiki page and the README's
  Features section.
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
- Linux release builds link the C++ runtime statically with `--gc-sections`: a process
  starts 0.6 ms sooner and warm builds run 5 % faster than with `libstdc++.so`. Keep
  iostreams (`<iostream>`, `<fstream>`, `<sstream>`) out of the CLI and the core: they
  pull about 460 KB of locale and stream code into that build. Use `RayoMd::File` and
  C stdio.
- Fonts are mapped read-only (`FileBytes`); read font data in place instead of copying
  the file.
- `TtfFont` is allocated by its own `operator new` (calloc) or lives in static storage:
  its 128 KB width cache relies on starting zeroed. Never put one on the stack. The cache
  is read by code point (`CodepointWidth`) and by CID (`WidthForCid`); both must keep the
  rule of `CidForCodepoint`.
- A PNG with transparency is decoded a row at a time: `ZlibReader` hands out each row
  from a window of the last 32 KB and a chunk, and the filtered colour and alpha rows go
  straight into `ImagePlanesEncoder`'s segment buffers, which are reused once compressed,
  so no plane is held whole. The encoder, its segment schedule and history, and the filter
  heuristic all decide the image bytes; change them only on purpose. A segment's bytes are
  published with release order after they are written; on a decode error the encoder's
  destructor abandons the planes and waits for its helpers. From `kImageThreadPixels`,
  helpers join in for each usable CPU (the affinity mask) that no other decode of the
  process holds, within the decode's share of the pool's three, so a busy batch asks for
  none. The first encoders of a process carve their buffers from 2 MB pages
  (`MADV_HUGEPAGE` on Linux), which saves most of a fresh process's page faults.
- `WriteFixed2` may write one byte past the end it returns, within the
  `kFixed2MaxChars` every caller must leave.
- Avoid adding large runtimes, browser engines, TeX stacks, or heavy document
  libraries to the native package.
- Reuse output buffers in batch/server paths where practical.
- Be careful with per-line/per-span heap allocation in `tiny_pdf.cpp`.
- Page content is rendered straight into the caller's output buffer and
  `PdfObjects::BuildInto` assembles the file in place around it. Do not go back
  to one string per page: allocating and freeing those buffers cost 7-15 % of a
  warm build on Windows.
- Paragraph text is parsed and wrapped as flat runs (`InlineRuns`, `AsciiRuns`,
  `StyledRuns`): one text buffer per paragraph, reused from paragraph to
  paragraph. Keep a string or vector per span, word, or line off these paths.
- Every look-ahead of the inline parser goes through `InlineScanner`, which
  answers from tables once a paragraph has spent its byte budget. A new scan
  that runs "to the end for every opener" must go there too, or text with
  thousands of unterminated openers becomes quadratic again. Matchers outside it
  (bare URLs, shortcut references) must fail in constant time or never rescan
  text: `CheckInlineLookahead` holds a megabyte of `http://(` and 120,000 `[`.
- Prefer measured changes over plausible micro-optimizations.
- `src/core/tiny_pdf.cpp` sits at GCC's `-O3` inline-unit-growth limit, so any edit
  can move which hot calls get inlined. Profile the hot paths after a change; mark
  hot helpers `RAYOMD_HOT_INLINE` and hot lambdas `RAYOMD_HOT_LAMBDA`, and keep rare
  paths in `RAYOMD_COLD` functions that do not call small hot helpers directly.
  `TailWriter::Lit` and `Bytes` are always inlined: left to GCC, a call for each
  one-byte separator cost 1.6 % of a warm build. A new cold feature goes into a
  translation unit of its own behind a small interface, as `theme.cpp` does: inside
  `tiny_pdf.cpp`, the theme layout moved `std::string::push_back` and appends of the
  link annotations out of line (+2.6 % instructions on `baseline.md`). Write output
  that grows by pieces through `TailWriter`, not per-byte `push_back`.
- Keep image caches bounded. Image support can dominate memory on large or many
  remote images.
- Linux benchmarks are much faster on native/ext4 storage than on `/mnt/*` WSL
  mounts. State the storage location when reporting Linux benchmark numbers.

## Correctness Guardrails

- Native mode should fail gracefully. Missing images, unsupported image formats,
  and unavailable URLs should produce fallback text, not crash.
- Keep ASCII and Latin documents on the standard-font path when possible. A document whose
  characters all have a WinAnsiEncoding code is transcoded once in `BuildPdf`
  (`TranscodeToWinAnsi`); link targets, image paths and formula sources go back to UTF-8
  where they leave the renderer (`WinAnsiToUtf8`).
- Preserve Unicode output by loading/subsetting a system font when needed, TrueType or
  OpenType/CFF (`RAYOMD_FONT`, then known per-OS paths, then a bounded scan of the font
  directories). Without any font, the document degrades to the standard fonts
  and `BuildResult::missingCharacters` counts what they could not show; never
  fail the export for a missing font.
- Keep PDF syntax valid: object ids, xrefs, page resources, image XObjects, and
  link annotations must remain consistent.
- `BuildOptions::sourcePath` matters for relative images; pass it from every
  file-based caller.
- Keep visible link text and annotation rectangles aligned when changing text
  wrapping, painting, or page splitting.
- A word is the text between two white spaces of the source, also across emphasis,
  code, and link boundaries. Never insert a space or break a line inside it; only a
  word wider than the line is cut by character.
- Standard-font text is measured with the exact AFM advances of the font that shows
  it (`kStandardWordAdvances`); keep wrapping, painting, and link rectangles on them.
- A performance change must not change PDF bytes unless that is its stated
  purpose. Line breaks and coordinates come from floating-point sums, so keep
  the order of those additions when restructuring wrapping or measuring code,
  and compare whole PDFs before and after (see `docs/development/performance.md`).

## UI Guidance

The Windows app should feel like a compact utility, not a landing page.

- Keep the main workflow immediate: edit/import Markdown, choose style/margin,
  export.
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
- `packaging/arch/` holds the Arch Linux PKGBUILDs (`rayomd` from the source tag, with
  curl; `rayomd-bin` from the release asset) and their `.SRCINFO`. Do not bump them by hand:
  the release workflow's `arch` job runs `packaging/arch/update.sh <version>` (pkgver,
  pkgrel, sha256sums, `.SRCINFO`), builds, checks and smoke-tests both packages on Arch,
  commits the result to the default branch and, when the `AUR_SSH_PRIVATE_KEY` secret is
  set, pushes both to the AUR.
- RayoMD's own code is licensed under Apache-2.0. Keep `LICENSE`,
  `NOTICE`, README license wording, and SPDX headers aligned when changing
  licensing metadata.
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
