# Performance development

Correctness and performance are intentionally separate. Build RayoMD first,
run `tests/verify_cli.py` for behavior, and use `tools/benchmark.py` only when
timing or release evidence is needed. Raw reports belong under ignored
`benchmark-output/`.

This file holds what a change needs: how to measure, how to prove the PDF bytes
did not move, the rules of the hot paths, where performance stands, and what was
measured and left. The dated notes on what every change since 2.6.0 gained, with
their tables, are in the wiki's
[Performance History](https://github.com/Butterski/rayomd/wiki/Performance-History).

## Where it stands (4.3.1)

Measured on 2026-10-13: Linux x86-64, g++ 13.3, Docker, one pinned CPU unless
said; a fresh export is a new `rayomd --export` process, start to exit.

- **Fresh text exports** are mostly the kernel's: starting the process, faulting
  pages in, the dynamic loader (the static edition avoids most of it) and, when
  the output file exists, the writeback ext4 forces when a truncated file is
  closed. A short ASCII note costs 0.64 ms of CPU; the same note in Polish 0.88 ms
  (1.00 before 4.3.1), the difference being the TrueType font found, read,
  subset and written. 4.3.1 reads `hmtx` and `loca` with one bounds test a table,
  keeps the glyph table in memory that starts zeroed, plans the subset and writes
  it once into its stream: 1,143,000 → 662,000 user instructions and 187 → 156
  page faults for the Polish note; `tester.md` without images 1.46 → 1.33 ms.
- **Unicode text** (4.3.0) is written as literal strings of two bytes a CID
  instead of four hex digits: text bytes about halve (this file's 2,000-line
  4.3.0 version 711,291 → 526,833 bytes), and plain code blocks are drawn
  without a vector of strings per line (35 KB of C++ in code blocks 350 → 202 µs
  warm).
- **Images** (4.1 and 4.2): RayoMD's own row-at-a-time inflater, its own encoder
  in an image mode, SSE2/AVX2 filters, Paeth unfiltered in pairs, and segments
  compressed by up to three helper threads. The mascot (1024x1024 RGBA) exports
  fresh in about 11 ms on one CPU and 7 ms on two or four (4.0.0: 27 ms), 2.2 ms
  from the persistent image cache. A new image needs more memory than in 4.0.0,
  which kept only the alpha plane whole: peak RSS 8.8 MiB for the mascot on one
  CPU (4.0.0: 5.3, 4.1.0: 11.2), 3.7 MiB once the image is cached.
- **Warm builds** (`--bench`) are 16 % faster than 3.2.0 over the watch fixtures
  (4.0: digit-pair numbers, one-lookup widths, formulas without a string per
  word, the static runtime).

## After 4.6.1: formula fonts, tables, fallback fonts, images, code, static start

Measured together on 2026-10-14 against 4.6.1, both built with g++ 13.3 (Release), CPU 3,
Docker. The 2,875 PDFs of the byte check are identical for the dynamic and the static build, and
so are those of a CJK matrix (24 documents under five option sets with Noto Sans CJK and with
WenQuanYi Micro Hei as the fallback font, exported singly, as a batch and as a book).

Warm `--bench` (`tools/benchmark.py ab`, 11 rounds), instructions and time against 4.6.1:

| Fixture | Instructions | Time |
| --- | ---: | ---: |
| `tester.md` without images | -15.0 % | -10.8 % |
| Tables, most with formulas or notes (table-heavy) | -34.2 % | -28.2 % |
| 500 plain table rows | -3.5 % | -1.4 % |
| Highlighted code (code-heavy) | -12.2 % | -8.4 % |
| The same in the Unicode renderer | -12.1 % | -6.8 % |
| README, essay, notes, polish, native math | -0.7 to +0.3 % | -0.1 to +3.2 % |

The time of the last row is mostly code placement: against the same code with the old
`src/hot.ld`, warm builds take 0.5 % longer (geometric mean; notes +1.6 %, README +1.4 %) while
fresh exports gain up to 3 %. `cjkmany.md` (CJK text the system font lacks), warm with Noto Sans
CJK: 0.48 → 0.35 ms (-28 %).

Fresh `--export` (`perfstat.sh`, 100 runs), task-clock and page faults:

| Document | Dynamic 4.6.1 | Dynamic now | Static 4.6.1 | Static now |
| --- | ---: | ---: | ---: | ---: |
| Short ASCII note | 0.52 ms, 89 | 0.53 ms, 91 | 0.35 ms, 57 | **0.32 ms**, 56 |
| Short Polish note | 0.61 ms, 116 | **0.57 ms**, 112 | 0.44 ms, 84 | **0.38 ms**, 76 |
| Essay, 10,000 words | 1.04 ms, 175 | **0.99 ms**, 173 | 0.82 ms, 143 | **0.77 ms**, 137 |
| `tester.md`, no images | 1.03 ms, 157 | **0.99 ms**, 154 | 0.83 ms, 124 | **0.78 ms**, 119 |
| One 1024x1024 PNG, no cache | 10.35 ms, 722 | **10.02 ms**, 704 | 9.93 ms, 687 | **9.78 ms**, 667 |
| The same PNG from the image cache | 1.55 ms, 445 | **1.22 ms**, 305 | 1.37 ms, 411 | **0.98 ms**, 268 |
| `cjkmany.md`, Noto Sans CJK | 17.36 ms | **14.23 ms** | 16.23 ms | **13.87 ms** |

What changed:

- **Formula fonts** (the 4.5/4.6 regression of `tester.md`): `BuildMathFaceFont` appends with
  `to_chars` into reserved strings, and each embedded face's objects, which depend only on the
  face, its codes and `--compress`, are kept between exports (`CachedMathFace`, 32 entries) and
  added as stream views.
- **Tables**: plain cells are wrapped into one buffer per column (`AsciiLines`, kept between
  exports like `tableCells`), the stripped text a view (`StripInlineMarkdownView`); a table with
  formulas keeps its cells per column (`MathTableCell`) instead of a vector of lines of spans per
  row, a plain line measured and painted with the same arithmetic.
- **Fallback fonts**: CFF glyphs are flattened into one raw buffer (fresh `cjkmany.md` -24 %
  instructions); and a layout in the system font that comes to a character of the BMP the font
  lacks probes for a fallback font from the source (`ProbeFallback`, `GuessFallbackFont`): when
  one is picked, the layout stops and the document is laid out in it. That build checks its guess
  (`NeedsRebuild`): only when the system font's choice for the characters painted is that font and
  could not differ for the characters the system font's layout paints (`kLayoutChars`,
  `FallbackChoice::stable`), no formula was laid out and no SVG converted, does it stand;
  otherwise the document is built as before. Not with a theme's bands or cover, PDF/A or PDF/UA
  page numbers, or URL images.
- **PNG from the cache**: IDAT is joined only for the stream that uses it, and a cache entry is
  read straight into the colour and alpha streams.
- **Highlighted code**: `CodeColor` returns a `string_view`; the Unicode renderer widens a block
  into a kept buffer (an ASCII block in one loop, lexed as it is), and draws a line whose integer
  width fits whole, without the floating-point wrap loop (exact: each of that loop's sums is at
  most the exact one times 1 + 1e-9 for lines under a million characters). Both changes to the
  Unicode renderer also made the standard renderer's code-heavy build 10 % cheaper through GCC's
  inlining of `tiny_pdf.cpp`.
- **`--fast-images`** (`PdfOptions::fastImages`): the image encoder's fast mode, one candidate a
  position and incompressible data skipped after half as many literals. On one CPU without the
  cache, the mascot 10.33 → 9.31 ms, a 3570x1444 RGBA screenshot 34.1 → 30.4 ms; the image
  streams of seven RGBA screenshots 0.6 to 2.6 % larger, the pixels the same. Its streams are
  cached under keys and `.fast.rimg` files of their own.
- **Static start**: GCC 13's libgcc sorts every FDE at startup when crtbeginT.o registers
  `.eh_frame`. `RAYOMD_STATIC` builds with GCC 13 or newer link with `--eh-frame-hdr` and wrap the
  registration into no-ops (`src/cli/static_frames.cpp`): 0.42 M fewer user instructions a
  process, 40-50 µs. The release's static asset, built by GCC 11, registers lazily. `rayomd-bin`
  on Arch now installs the static asset.
- **Static edition position-independent** (2026-10-17): `RAYOMD_STATIC` links `-static-pie`
  instead of `-static`, so the static asset gets the address randomisation the dynamic build has
  (its plain `-static` was an ET_EXEC at 0x400000). Fresh exports, 64 runs each against the same
  code linked `-static`: 4 to 6 more page faults (glibc relocates 2,164 pointers at start), +0.02
  to +0.04 ms on a 205-byte document and within noise on the 15 and 33 KB ones; 57 KB larger. It
  stays 0.25 to 0.35 ms (13 %) faster to start than the dynamic build. Its crtbeginS.o registers
  no frames, so the wrap above does not run there. Checked with GCC 11 (the release's) and 13.

## Phases 1 and 2 against 4.3.1

All of the work below, measured together on 2026-10-13: the 4.3.1 code and the final code
built side by side with g++ 13.3 (Release, as CI), CPU 3, every binary re-read into the page
cache first. The 2,875 PDFs of the byte check (359 documents under eight option sets, a book
and two batches) are identical, for the dynamic and the static build.

Fresh `--export`, `perf stat` of 300 runs, output on tmpfs:

| Document | Dynamic 4.3.1 | Dynamic now | Static 4.3.1 | Static now | Faults, dynamic |
| --- | ---: | ---: | ---: | ---: | ---: |
| Short ASCII note | 0.60 ms | **0.49 ms** (-18 %) | 0.33 ms | **0.30 ms** (-9 %) | 98 → 83 |
| Short Polish note | 0.77 ms | **0.56 ms** (-27 %) | 0.52 ms | **0.38 ms** (-27 %) | 154 → 113 |
| Essay, 10,000 words | 1.24 ms | **0.98 ms** (-21 %) | 0.95 ms | **0.78 ms** (-18 %) | 187 → 172 |
| `tester.md`, no images | 1.17 ms | **0.92 ms** (-21 %) | 0.88 ms | **0.71 ms** (-19 %) | 189 → 146 |
| README (Unicode, 28 KB) | 1.71 ms | **1.32 ms** (-23 %) | 1.41 ms | **1.09 ms** (-23 %) | 258 → 216 |
| One 1024x1024 PNG | 10.53 ms | 10.27 ms (-2 %) | 10.19 ms | 9.86 ms (-3 %) | 729 → 715 |

Overwriting an existing PDF on ext4 (interleaved `perf stat`, 300 runs), CPU and wall time:
the short ASCII note 700 → 497 µs and 1,095 → 852 µs, the Polish note 863 → 567 µs and
1,285 → 942 µs, the README 1,763 → 1,296 µs and 2,240 → 1,729 µs.

Warm builds (`tools/benchmark.py ab`, 11 rounds, A/A within 0.8 %):

| Fixture | Time | Instructions |
| --- | ---: | ---: |
| README (Unicode) | -19.6 % | -23.2 % |
| Polish note | -22.5 % | -20.1 % |
| Essay | -17.5 % | -15.5 % |
| Short ASCII note | -11.3 % | -11.2 % |
| `native_math.md` | -10.1 % | -8.7 % |
| `tester.md`, no images | -9.7 % | -9.8 % |
| Code-heavy (31 languages) | -8.1 % | -15.6 % |
| Table-heavy | -4.1 % | -8.9 % |
| Geometric mean | **-13.1 %** | **-14.3 %** |

The dynamic binary grew from 1,293,160 to 1,321,904 bytes (exact rounding, the SSE2 scans,
the kept buffers) and no longer needs `libm.so`; the static one from 2,174,600 to 2,199,216.

## Fresh exports, Phase 1

Measured on 2026-10-13 (g++ 13.3, glibc 2.39, CPU 3, `perf stat` of 200 `--export` runs of each
binary after its file was re-read into the page cache; output on tmpfs unless said). Four
changes, each keeping every PDF byte:

- **The PDF is written in place** (`RayoMd::File::WriteAll`, POSIX): `open` without `O_TRUNC`,
  one write loop, `ftruncate` only for a regular file that was longer. Truncating an existing
  ext4 file to zero freed its blocks and made `close` start writeback (`auto_da_alloc`); without
  it an export onto an existing PDF is 0.12 to 0.19 ms faster. Tradeoff: no such flush, so a
  crash can leave old and new bytes mixed, as any in-place overwrite can.
- **TrueType one-shot work**: `post` copies its 32-byte header only, `hmtx` and `loca` are read
  in place, `TtfFont`'s scalars lie before its 128 KB arrays, and a subset not cached yet is
  written by `BuildInto` straight into the file (`PdfObjects::SetStreamWriter`): polish -28
  faults, -60 µs.
- **No `libm.so`** in the dynamic build (`exact_math.h`; SVG loads glibc's trigonometry with
  `dlopen` on first use): -10 faults, -0.07 ms for every export; a document with SVG transforms
  or arcs pays 0.04 ms for the `dlopen`.
- **Function ordering** (`src/hot.ld`, `scripts/hot_sections.py`): 3 to 4 fewer faults, 7 to
  19 µs.

| document | 4.3.1 dynamic | now dynamic | 4.3.1 static | now static |
| --- | ---: | ---: | ---: | ---: |
| notes.md | 0.60 ms, 98 faults | 0.49 ms, 85 | 0.33 ms, 52 | 0.31 ms, 52 |
| polish.md | 0.76 ms, 154 | 0.57 ms, 109 | 0.51 ms, 110 | 0.39 ms, 76 |
| essay.md | 1.24 ms, 187 | 1.06 ms, 171 | 0.95 ms, 140 | 0.85 ms, 137 |
| tester-noimg.md | 1.18 ms, 189 | 0.95 ms, 143 | 0.88 ms, 143 | 0.74 ms, 111 |
| README.md | 1.69 ms, 257 | 1.43 ms, 213 | 1.40 ms, 216 | 1.20 ms, 185 |
| image.md | 10.47 ms, 729 | 10.36 ms, 716 | 10.14 ms, 682 | 9.98 ms, 680 |
| notes.md, existing ext4 output | 0.69 ms | 0.50 ms | 0.45 ms | 0.34 ms |
| README.md, existing ext4 output | 1.75 ms | 1.42 ms | 1.49 ms | 1.24 ms |

Warm builds (`ab --instructions`) stay within their spread: instructions -0.5 % (the libm
change; `WriteFixed2` converts numbers from 2^52 directly instead of calling `llround`).
Not done: the lazy cmap (see *Tried and rejected*: `HasGlyph` runs per painted character and
would need a test and atomics for shared fonts), and `-no-pie` (see *Tried and rejected*).
Open decisions measured in the Phase 1 breakdown: the static binary as the default asset, and
skipping libgcc's frame registration in GCC 13 static builds (30 µs).

Later trims of the one-shot CLI (dynamic, `perf stat` in 40 to 60 interleaved rounds of 50
exports onto an existing tmpfs or ext4 PDF, against an identical copy as control):

- **The input is read with `read()`** into the string instead of mapped, copied and unmapped:
  one fault, two syscalls and 15 K kernel instructions less; notes, polish and README 4 to 6 µs
  less CPU (control ±2), 3 to 9 µs less wall time.
- **`--export` and `--stdin` end with `_Exit`** after flushing stdout (`ExitOneShot`), so the
  1 MB output buffer, the font mapping, the document and every static are left to the kernel;
  batch, serve and the library free theirs as before. notes, polish and README 7 to 45 µs less
  CPU and 8 to 19 µs less wall time (control within ±5), about one fault and 15 to 130 K kernel
  instructions less.

## Warm text, Phase 2

Measured on 2026-10-13 (g++ 13.3, CPU 3; `ab --instructions` over the warm fixtures, 11
rounds, and alignment-pinned builds of both sides; whole-PDF identity over the harness corpus
and a differential fuzzer of 5,000 to 6,000 random inline paragraphs per change, three option
sets each plus batches). Four changes to the standard-font paragraph path, each keeping every
PDF byte:

- **Run text copied a line at a time** (`AppendAsciiRunText`): a word that follows the pending
  bytes in the parsed text joins them, so a line is one `memcpy` instead of one per word.
- **`TailWriter` without zero-fill** (see *Buffers and the content stream*).
- **One escape test per paragraph** (`HasLiteralSpecial`): runs of a paragraph without ( ) \ or
  control bytes skip `WriteEscapedLiteral`'s per-byte class test (essay: 4.0 M of 73 M
  instructions over 21 builds → 0.16 M).
- **The run buffer only grows**: no zero-fill of twice each paragraph.

| fixture | time | time, aligned | instructions |
| --- | ---: | ---: | ---: |
| essay.md | -9.6 % | -7.8 % | -6.7 % |
| native_math.md | -1.7 % | -1.2 % | -2.1 % |
| notes.md | -2.9 % | -0.4 % | -1.9 % |
| README.md (Unicode) | -0.6 % | -0.5 % | -1.4 % |
| table-heavy.md | +0.3 % | -0.7 % | -1.4 % |
| code-heavy.md | +0.6 % | -0.1 % | -1.1 % |
| polish.md, tester-noimg.md | +0.7 %, +0.1 % | -1.8 %, -0.1 % | -0.5 %, -0.6 % |
| geometric mean | -1.7 % | -1.6 % | -2.0 % |

A/A spread up to 1.5 % unaligned, 1.2 % aligned. Fresh exports (perfstat, 300 runs): essay
1.06 → 1.03 ms, 3.717 → 3.480 M instructions; notes 0.49 ms, 0.439 → 0.435 M. Measured and
not kept: SIMD word ends and branch-free word sums, an operand cache (*Tried and rejected*);
the word loop's mispredictions remain (*Measured opportunities*).

Then four changes to the Unicode renderer's text path (measured the same way, plus a 100 KB
generated Unicode document and a differential fuzzer of 6,000 Unicode documents per change:
Polish, Greek, Cyrillic, CJK, emoji, combining marks, characters DejaVu Sans lacks, NBSP and
other spaces, surrogate-range code points, invalid UTF-8, CIDs whose bytes need escaping; each
exported plain, `--pdfa`, `--pdfua`, with a theme and a random page width, plus batches):

- **Plain CIDs copied as they are** (`UsedCidSet::IsPlain`): a code unit the font has a glyph
  for, already recorded, no high surrogate and no byte to escape is one bit test and a store in
  `WriteCidText`; everything else goes to `WriteNewCid` (README: 29 → 10 instructions a
  character). 8 KB more per renderer (2 to 3 more page faults on a fresh export).
- **Unicode run text copied a line at a time** (`PendingWide`, `QueueWide`), as the ASCII path.
- **A word measured while its end is found** (`ScanWideWord`): one pass, the same sum in the
  same order as `TextWidth`, no call per word.
- **A paragraph decoded in one pass** (`WriteUtf8Wide`): the wide buffer is sized once and keeps
  its size; `basic_string::append(first, last)` had made a temporary string, a malloc and a free,
  for most spans.

| fixture (against Phase 2a) | time | time, aligned | instructions |
| --- | ---: | ---: | ---: |
| README.md (Unicode) | -13.2 % | -11.5 % | -17.6 % |
| polish.md | -7.6 % | -7.0 % | -7.9 % |
| tester-noimg.md | -4.5 % | -3.8 % | -5.5 % |
| 100 KB generated Unicode | -4.5 % | -3.6 % | -7.0 % |
| code-heavy.md, table-heavy.md | -7.9 %, -5.8 % | -6.1 %, -2.3 % | -13.3 %, -6.6 % |
| essay, native_math, notes (ASCII) | +0.1, -0.6, -0.9 % | +1.7, +1.5, +1.3 % | 0.0, -0.1, 0.0 % |
| geometric mean | -5.1 % | -3.4 % | -6.6 % |

A/A spread up to 1.5 %; the ASCII fixtures keep their instructions, so their aligned change is
placement. Fresh exports (perfstat, 300 runs): README 1.44 → 1.37 ms, 5.48 → 4.59 M
instructions; polish 0.58 ms both, 0.688 → 0.671 M, 110 → 113 faults; notes and essay unchanged.

Then the allocations every export made (callgrind call counts of `operator new` by call chain;
polish.md made 101 a warm build, notes.md 174), in two changes:

- **Buffers kept by each thread from one export to the next** (`KeptBuffer`, `KeptLocal`): the
  renderers' page, link and heading tables, paragraph and table-cell runs, the object table,
  `BuildInto`'s offsets, the outline's buffers and storage, the WinAnsi transcoding and the line
  tables of nested parser levels (`LevelLines`) are swapped in when an export starts and back
  when it ends; one grown past 1 MiB goes back to the allocator, which bounds a batch worker's
  memory. The Unicode renderer's CID set is kept and cleared by the words of the CIDs it
  recorded (`KeptCidSet`), not zeroed (16 KB) per export. The document's own parser level is
  not kept: a one-shot export would hold its lines to the end and later allocations take new
  pages (README +12 faults). Local image paths are parsed at the first image only.
- **Small objects assembled without strlen** (`AppendLit`): `out += "..."` reached
  `basic_string::append(const char*)` out of line, a strlen per piece; the info dictionary is
  reserved once and the font cache key written without `push_back`.

Allocations a warm build: polish 101 → about 50, notes 174 → about 115.

| fixture | against Phase 2b, time | instructions | against Phase 1, time | instructions |
| --- | ---: | ---: | ---: | ---: |
| polish.md | -16.1 % | -11.4 % | -23.6 % | -18.8 % |
| notes.md | -9.8 % | -8.3 % | -11.4 % | -10.0 % |
| README.md | -6.4 % | -3.7 % | -19.3 % | -21.7 % |
| native_math.md | -5.5 % | -3.4 % | -7.5 % | -5.6 % |
| tester-noimg.md | -4.1 % | -3.3 % | -8.2 % | -9.1 % |
| essay.md | -3.8 % | -2.0 % | -12.6 % | -8.5 % |
| code-heavy.md | -0.8 % | -0.9 % | -8.4 % | -15.0 % |
| table-heavy.md | +0.1 % | -0.5 % | -5.8 % | -8.4 % |
| geometric mean | -5.9 % | -4.2 % | -12.3 % | -12.3 % |

A/A within 1.1 %; alignment-pinned builds of the first change -5.1 % (unpinned -5.2 %). Fresh
exports (perfstat, 300 runs) stay within 10 µs and 3 faults. `--batch` on one CPU: the generated
corpus (305 documents) -2.1 %, peak RSS 17.4 → 18.2 MiB with one worker and 36.3 → 39.1 MiB with
the automatic count; 200 copies of polish.md -4.4 %, 12.8 MiB both.

Then the byte-class scans (*Byte classes scanned a byte at a time*), measured the same way, in
three changes:

- **The parser's ordinary-byte runs found 16 bytes at a time** (`FindByteClass`): SSE2 compares
  against the twelve bytes that stop a run (inline syntax, ':' and '@'), the last step over the
  text's last 16 bytes again; text under 16 bytes keeps the byte loop. The loop in
  `ParseInlineRuns` took 9.2 % of the essay's instructions (about 5 a byte), now 0.65 %.
- **A plain paragraph wrapped without a second syntax test**: `NeedsInlineParse` has read every
  byte of a paragraph without inline syntax, so the standard `RenderParagraph` calls
  `WrapAsciiWords` directly unless the paragraph holds 0xE2 (`ForEachWrappedAsciiLine`'s
  `ContainsByteClass` read it again: 1.4 % of the essay).
- **No second math search**: `WrapAsciiRuns` and `WrapStyledRuns` have looked for '$' and "\("
  already and pass `recognizeMath` false, which spares `ParseInlineRuns` the same two searches;
  `WrapStyledInline` takes 0xE2 from `NeedsInlineParse`'s classes instead of a `find`.

| fixture (against Phase 2c) | time | time, aligned | instructions |
| --- | ---: | ---: | ---: |
| essay.md | -5.2 % | -5.1 % | -7.2 % |
| native_math.md | 0.0 % | -1.5 % | -2.6 % |
| README.md | -0.8 % | -0.7 % | -1.4 % |
| polish.md | -0.4 % | -0.9 % | -1.2 % |
| notes.md | -0.2 % | -0.3 % | -0.9 % |
| tester-noimg.md | 0.0 % | -0.9 % | -0.2 % |
| code-heavy.md | -0.3 % | -0.2 % | -0.2 % |
| table-heavy.md | +1.3 % | -1.1 % | 0.0 % |
| geometric mean | -0.7 % | -1.3 % | -1.7 % |

A/A up to 1.6 % unaligned (table-heavy), 0.6 % aligned. Fresh exports (perfstat, 200 runs):
essay 1.02 → 0.99 ms, 3.455 → 3.223 M instructions; the others within 10 µs and their faults.

## Maintained workflows

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-native-ext4 --suite watch --label local
python3 tools/benchmark.py ab -- --baseline build/before/rayomd --candidate build/linux/rayomd --cpu 3 --instructions
python3 tools/benchmark.py fresh -- --baseline build/before/rayomd --candidate build/linux/rayomd --corpus CORPUS --root benchmark-output/fresh
python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe --platform windows --suite full --samples 5
python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --node-modules path/to/node_modules --root benchmark-output/competitors
```

The standard `run` workflow covers cold export, warm `--bench`, folder batch,
stdin batch, warm serve, ASCII, Unicode, tables, explicit rules/page breaks,
comments, and optional local images. Use it to protect the default fast path.

The `ab` workflow measures what one engine change costs. It runs `--bench` for
the baseline, an identical copy of the baseline, and the candidate in rotated
order on the same fixtures (by default the watch suite's documents and
`tester.md`). It reports the median of the paired per-round changes, so drift
of the machine cancels, and the copy's change is the noise floor. It also says
whether the two binaries write the same PDF bytes. `--instructions` adds the
callgrind instruction count of one warm build (Linux, valgrind). That count
does not move with code placement, which release-build timings do by about a
percent; see [Code placement](#code-placement). It does move with the size of
large copies: valgrind counts every byte of a `rep movsb`, which glibc's
`memmove` uses for large copies that do not overlap, so a change that only moves
page contents further in `BuildInto` can add instructions without adding time.

The `fresh` workflow (`scripts/fresh_process_ab.py`) times one-shot `--export`
processes of two binaries in pairs whose order alternates and whose cases
rotate, after uncounted preflights, and checks that both write one and the same
PDF per case. See its `--help` for the pinned inputs and acceptance limits.

The `competitors` workflow includes the three deterministic synthetic cases plus
John Gruber's authentic `Markdown: Syntax` source and deterministic 1 MiB and
5 MiB whole-document repetitions. It caches the download under ignored
`benchmark-output/`, records the source URL and SHA-256, and reports per-case,
overall, and large-document speedups against the fastest competitor. Use
`--offline` to reuse the cache. The source contains inline/block HTML and
Markdown features outside RayoMD's focused native subset, so timings must be
published with the compatibility caveat and not as rendering-equivalence claims.

Pandoc runs with XeLaTeX and, when `typst` is installed, with Typst; `--node-modules`
adds md-to-pdf (Node 22 or later for md-to-pdf 5; set `MD_TO_PDF_CHROME_ARGS=--no-sandbox`
inside a container) and `--glu` adds glu. A tool that is missing is skipped, and each
command starts in the document's folder. `--document FILE` measures that file instead of
the corpus; `--pandoc-from` and `--pandoc-mainfont`/`--pandoc-monofont` set Pandoc's input
format and fonts. RayoMD runs with `--no-image-cache` where it has that flag, so every run
re-encodes its PNGs as the other tools do; `--rayomd-image-cache` measures it with its
persistent image cache instead. The README's comparison on `tester.md` was taken this way:

```sh
python3 tools/benchmark.py competitors -- --rayomd build/linux/rayomd --node-modules /opt/node/node_modules \
  --glu /opt/glu/glu --document tester-local.md --pandoc-from markdown+tex_math_single_backslash-raw_tex \
  --runs 20 --timeout 120 --preflight-timeout 120 --root benchmark-output/competitors-tester
```

`tester-local.md` is `tester.md` without the lines that test fallbacks (the remote and
missing images and the paragraph with an unknown TeX command): with them XeLaTeX and glu
stop with an error and Pandoc waits on the network. `tester-noimg.md` also drops every other
line that starts with an image and the `[logo-ref]` definition, so it holds no image at all.

Each tool first gets a 15-second tiny-document preflight; a failure disables it
for the rest of the run. Measured invocations have a 60-second default timeout,
and scaled cases use one run without a warm-up. A timeout is recorded as a DNF
and disables that tool for the remaining cases instead of aborting the report.
The fastest competitor that actually completed remains the score baseline.
Override these controls with `--preflight-timeout`, `--large-runs`, and
`--large-timeout` for deliberate longer campaigns.

The `reversible` workflow covers all source-recovery work in one reproducible
run:

- ordinary versus embedded output at 0 B, 10 KiB, 1 MiB, and the 10 MiB cap;
- ASCII, Unicode, links, local image, failed/remote fallback, table/list,
  multipage, and source-only/private-content documents;
- folder batch, stdin batch, and warm serve, embedding both off and on;
- inspect and byte-exact recovery p50/p95;
- not-reversible, unsupported-profile, integrity, truncation, and source-limit
  rejection latency;
- output size, optional psutil peak RSS, raw-versus-Flate fixtures, qpdf and
  Poppler availability, and optional allocation deltas from a profiling build.

`quick` omits the 10 MiB scale case. `full` includes it. Both retain feature,
workflow, rejection, and storage coverage. Use at least five samples for a
publishable p95; the harness caps the maximum-size case at three runs.
Each child command is terminated after 120 seconds by default so a broken CLI
route cannot leave the benchmark apparently idle for an hour. Override this
only for deliberately slow environments with `--timeout-seconds N`.

To collect allocation deltas without changing the release binary:

```sh
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_PROFILING=ON
cmake --build build/profile --config Release
python tools/benchmark.py reversible -- \
  --binary build/windows/rayomd.exe \
  --profile-binary build/profile/rayomd.exe \
  --platform windows --suite full --samples 5
```

### How the published numbers are taken

- **Pin the CPU.** Run timings on one pinned core (for example `taskset -c 3` or
  `docker run --cpuset-cpus=3`), or on two or four pinned cores when threads are
  what is measured, with nothing else on those cores. Every published number was
  taken this way.
- **Warm builds**: `tools/benchmark.py ab`, seven to nine rounds, reported against
  its A/A change. A difference within the A/A spread is noise.
- **Fresh processes**: `tools/benchmark.py fresh`, or medians of 40 or more
  `rayomd --export` runs per binary in alternating order with the input on tmpfs.
  `perf stat -r 100` gives CPU time, page faults and user instructions of a fresh
  export; CPU time and wall time can disagree (work the kernel does at exit).
- **Instructions**: `valgrind --tool=callgrind` of one warm build (`ab
  --instructions`) or of a whole fresh process; `callgrind_annotate` shows which
  function moved. `--cache-sim=yes` (cachegrind) counts D1 misses, and
  `--branch-sim=yes` mispredictions.
- **Profiles**: `perf record` on a warm `--bench` loop or on repeated fresh
  exports shows where the time goes; confirm a finding with callgrind's counts.
- **Images**: in process (`DecodeImageBytes` on the PNG's bytes, the image cache
  bypassed, 1,000 iterations, p50 and p95) and as fresh exports with
  `--no-image-cache`; peak RSS with `/usr/bin/time -v`.

### Comparing whole PDFs

A performance change must keep every PDF byte unless changing them is its stated
purpose. Before and after, export the same documents with both binaries and
compare the files byte for byte (`cmp`, or their SHA-256): the repository's
Markdown, the watch fixtures and `tester.md`, each plain and with the options the
change can reach (`--compress`, `--page-numbers`, `--toc`, `--pdfa`, `--pdfua`,
`--embed-source`, a theme, letter pages, the three styles). `ab` and `fresh`
already report whether the two binaries agree on their fixtures.

A change that can reach the fallback fonts (font loading, the Unicode renderer's layout, what it
paints besides the text) needs CJK documents too, where the system font lacks characters: with
`fonts-noto-cjk` and `fonts-wqy-microhei` installed, export documents with CJK text early, late,
only in a comment or link, beside a formula, `[TOC]`, an SVG (also one named `.png`) and
footnotes, and a `--book` folder of CJK files, singly, as a `--batch` (its report's
`missing_characters` too) and as a book; each plain and with `--toc`, `--pdfa --page-numbers`,
`--pdfua --page-numbers` and `--compress`, once as the system finds the fallback font (Noto Sans
CJK, a CFF font) and once with `RAYOMD_FALLBACK_FONT` set to WenQuanYi Micro Hei (TrueType).

Where bytes change on purpose, show what did not change: the PDFs differ only in
the objects the change names (a dictionary entry, image streams), `qpdf --check`
passes, and Poppler or MuPDF give the same text (`pdftotext`, `mutool draw -F
txt`) and the same pixels (`pdftoppm`, `mutool draw` at 40 to 300 dpi) as before.
For a re-encoded image, decode its streams back and compare every pixel with the
source.

The core tests carry golden digests (`CheckNoMathGolden`,
`CheckOutputBufferReuse`, `CheckInlineLookahead`). Line breaks and coordinates
come from floating-point sums, so a reordered addition is enough to move a line;
for a rewrite of wrapping, parsing or emission, differential fuzzing of the old
against the new engine (two source trees compiled side by side, not part of the
repository; generated whole documents, block trees and inline spans, with
deliberate bugs to prove the generators notice) is how the October 2026 engine pass
was kept byte-exact, also under AddressSanitizer and UBSan.

## Regression and size gate

Record the baseline before changing performance-sensitive code. Compare an
identically configured candidate with the explicit record, not a local-history
guess:

```sh
python tools/benchmark.py run -- \
  --binary build/before/rayomd.exe --platform windows --suite watch --label before

python tools/benchmark.py run -- \
  --binary build/after/rayomd.exe --platform windows --suite watch --label after \
  --baseline-record benchmark-output/perf-watch/windows/<before-run>/record.json \
  --fail-on-slower-pct 5
```

The gate judges medians and totals. p95 values from two to a few dozen samples
are close to their maximum: on identical binaries `serve_reported_ms_p95` moved
by +5.6 % and `cold_export_ms_p95` (over eight runs) by +5.5 %. They are reported, not gated. Warm
averages come from `total_ms / iterations`, so a 0.22 ms feature case is no
longer read in 0.01 ms (4.5 %) steps, and the watch suite takes 24 cold
exports. With these rules, eight A/A runs of the 2026-10-08 binary against its
own record all passed; the largest gated change was +2.4 %
(`sized_unicode_warm_ms_median`).

Also compare executable/package byte size. A change that improves a tiny case
but regresses larger documents, batch/serve throughput, memory, or package size
must be fixed, isolated behind an opt-in/separate package, or documented as a
deliberate tradeoff.

Keep the candidate comparable to its baseline: same suite, seed, style, margin,
image mode, worker count, compiler, build type, and storage. Linux release
claims should use native/ext4 storage. Label WSL `/mnt/*` results explicitly.

## Native math

Native math (see `native_math.md`) must cost nothing for documents without
formulas, and those documents must keep their PDF bytes. Gate formula-free
documents with the usual 5 % rule. Report documents with formulas separately:
they do real typesetting work that 2.6.0 did not do, and the watch corpus
contains display formulas.

Measured on 2026-10-08, Windows 11, MinGW g++ 15.2 `-O2`, an engine-only warm
loop pinned to one core, best of nine order-rotated rounds against 2.6.0:

- 1 MiB ASCII and 1 MiB Unicode fixtures without math syntax: +1.4 % each, with
  identical output bytes. Run-to-run noise on that machine is about 1–2 %.
- One formula costs 0.4 µs (`$x$`) to 4.3 µs (the quadratic formula) for layout
  plus emission, with two heap allocations.
- A 1.0 MiB document with 1,500 display formulas: 22.2 ms to 25.5 ms. A 740 KiB
  document with 11,200 formulas (five per paragraph): 24.5 ms to 46.2 ms, about
  1.9 µs per formula including line layout.
- Executable size: Windows 2,820,608 to 2,952,704 bytes (+132 kB); Linux x64 CLI
  (g++ 13.3, `-O3`) 432,688 to 571,952 bytes (+139 kB). Both math translation
  units are built with `-Os`.

Formula-bearing text takes its own wrap and paint functions and must not share
the small span helpers or vector code of the plain text path: extra call sites
there stop the compiler from inlining them into the hot wrap functions.

## Hot paths

### The inlining budget of `tiny_pdf.cpp`

`tiny_pdf.cpp` sits at GCC's `-O3` `inline-unit-growth` limit: inlining grows the
unit by about a quarter and then stops, in an order set by the whole file. Any
edit can therefore move which calls are inlined, also far from the edit: removing
a style check from `WrapStyledRuns` changed the out-of-line `WriteFixed2` calls of
a build from 10,472 to 3,590. Check `--instructions` per function after every
edit there. Countermeasures that held up:

- `RAYOMD_HOT_INLINE` for hot helpers and `RAYOMD_HOT_LAMBDA` for hot lambdas;
  `TailWriter::Lit`, `TailWriter::Bytes`, `ContainsByteClass` and `ReadU32` are
  forced inline because edits elsewhere pushed them out of line (an out-of-line
  `Lit<2>` cost a call per separator; forcing it gave the watch fixtures -1.6 %).
- Rare paths in `RAYOMD_COLD` functions that do not call small hot helpers
  directly; drawing code shared by several callers in `RAYOMD_SHARED` (out of
  line, not cold: `PaintAsciiRuns`, `PaintStyledRuns`, `CodeTile`, highlighted
  code), one copy instead of a clone per caller.
- Cold features in translation units of their own built at `-Os`
  (`CMakeLists.txt` lists them: themes, PDF/A, footnotes, contents, books, SVG,
  tagging, the image cache, export options, math). Each one, written into
  `tiny_pdf.cpp` or `markdown_parser.cpp`, cost 0.85 to 2.9 % instructions on
  documents that never use it. The encoder, the inflater and the highlighter are
  at `-O2`.
- Values that a `char*` write could alias (buffer starts, run state) passed by
  value into hot helpers, so they are not reloaded per word.
- Byte compares instead of `string_view::compare` (a `memcmp` call) in the parser;
  string concatenations such as `"- " + text` spelled out, as GCC outlines them.

Forcing more calls inline was not kept: under the limit it only moves the loss to
other calls. See *A larger inlining budget* below for raising the limit.

### Code placement

Release-build timings move by about a percent with where the hot functions land,
which any edit to `tiny_pdf.cpp` changes. Read a release-build difference of that
size as placement until two other measurements agree with it: a build of both
binaries with `-falign-functions=64 -falign-loops=32` ("alignment pinned", noise
up to about 0.7 %) and the callgrind instruction count. Pinned alignment does not
fix which instruction-cache sets the functions fall into; code added before them
still moves them (see *Hot functions grouped in `.text.hot`*).

### Buffers and the content stream

- Page content is rendered straight into the caller's output buffer and the file
  is assembled in place around it in `BuildInto`: no string per page and no second
  copy (7 to 15 % of a warm build on Windows when it was one). The move of every
  page's content to its final place is the one copy left.
- Operators are written through `TailWriter`: an operation whose upper bound fits
  1 KB writes into a thread-local buffer and appends what it wrote (no zero-fill);
  a larger bound, a nested writer and Windows (MinGW's `thread_local` is a call)
  reserve the bound in the string, which zero-fills it. Numbers are formatted from
  digit-pair tables (`WriteFixed2`), not library calls. Writing a whole outline
  through one zero-filled tail evicted L1, so outline entries go through a scratch
  buffer that stays in L1.
- A paragraph is asked once whether its text needs escaping in a PDF literal
  (`HasLiteralSpecial`, SSE2; the plain-text path reuses the classes
  `NeedsInlineParse` summed); its runs are then copied as they are.
- Unicode text: `WriteCidText` copies a plain CID (`UsedCidSet::IsPlain`) without a lookup;
  anything that changes which CID, escape or missing count a code unit gets belongs in
  `WriteNewCid`, which decides when a unit becomes plain. Word widths come from `ScanWideWord`,
  which must keep `TextWidth`'s sum and order.
- Paragraphs are parsed and wrapped as flat runs: one reused text buffer per
  paragraph, plain lines as views into the source. Table cell and link-text
  buffers are reused from row to row and table to table.
- `Block` size and layout matter: every block is moved when the vector grows.
  Rare fields go behind a pointer (`Block::image`) or into existing padding
  (`task`, `alert`); a byte after `hasMath` instead changed how GCC copies the
  tail (+0.47 % instructions).
- Code and emphasis share one style byte in `AsciiRun` and `AsciiSegment`, so
  placing a word compares one byte; `StyleAdvances` reads advances through a
  table of pointers, not a computed index.
- Large temporaries meet glibc's dynamic mmap threshold; see *glibc's mmap
  threshold* below before changing a reservation.
- Containers that every export fills are kept by the thread between exports (`KeptBuffer`,
  `KeptLocal`, `KeptCidSet`, the parser's `LevelLines`), each up to 1 MiB. A new per-export
  container should be one of them; keep the document's top parser level out (fresh faults).

### Bounded scans

Every inline look-ahead (closing emphasis, code spans, link destinations,
autolinks, comments, reference labels, footnote labels) is either bounded per
opener or answered from tables once a paragraph has read eight times its own
size in look-ahead. `CheckInlineLookahead` holds up to a megabyte of
unterminated openers; a new scan must keep such text linear (each quadratic
case found so far took seconds for 40,000 to 200,000 openers).

## Measured opportunities

Findings that could make RayoMD faster later, with the evidence and the reason
they were not taken yet. Add to this list when a change turns one up.

**RayoMD's encoder against ISA-L.** Compressing the colour is the longest stage
of a new image: 4.2 ms of the mascot's 8.7 on one CPU, where ISA-L's level 1
(hand-written assembly over one hash probe) takes 2.5 ms for streams 16 % larger.
A C version of its parse would have to keep RayoMD's sizes. Cheap ways lose size:
one probe instead of two, 12.8 % faster, illustration +19 %; skipping sooner,
6.7 % faster, soft shadows +10 %. Flushing the bit writer only when full (+15 %
time), one table for literals and lengths (+6.9 %), three literals a flush,
prefetching, 16-byte match compares and early second-probe rejection (each within
0.4 %) lost. Helper threads now hide much of it on more than one CPU (2026-10-13).

**The inflater on literals.** 20 % slower than libdeflate on the opaque photo and
soft shadows, 9 % on the noisy photo, 2.7 % over all test images (`-O2`). A BMI2
build and libdeflate's `bitsleft` trick gained nothing (2026-10-13).

**`-O2` on Windows.** Windows release builds use `-O2`, Linux `-O3`; on Linux the
same source at `-O2` is 5.8 % slower (96 KiB Unicode +14.6 %) for 44 KB less code
(2026-10-08). Next: `-O3` for the engine units only (`tiny_pdf.cpp`,
`inline_markdown.cpp`, `markdown_parser.cpp`) with MinGW on Windows hardware,
weighed against size.

**Latin PDF/A on the standard renderer.** `--pdfa` sends ASCII and Latin documents
to the Unicode renderer: `baseline.md` +59 % time and +57 % size, `ascii_96kb.md`
+26 % and +25 % (2026-10-13). Writing one byte a glyph (a simple WinAnsi TrueType
font) gained only 0.6 to 1.3 % and 9 to 11 % size, so it was not taken. The cost is
wide-string wrapping and a glyph lookup per character; the fix is the standard
renderer on embedded TrueType faces with their own advances, which changes how
`--pdfa` documents look and must not slow the default path.

**A larger inlining budget for `tiny_pdf.cpp`.** Against `-O3` at the default
(597,840 bytes of code):

| `--param=inline-unit-growth` | Code | Time | Instructions | ASCII 96 KiB | Unicode 96 KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 60 | +32 KB | -0.9 % | -0.6 % | -3.5 % | -0.0 % |
| 100 | +93 KB | -1.9 % | -2.2 % | -4.6 % | -2.0 % |
| 200 | +242 KB | -2.9 % | -3.2 % | -5.1 % | -3.6 % |

Not adopted: +5 to +40 % of the executable for 1 to 3 %, and the cliff only moves.

The two renderers in translation units of their own do not help either (prototyped
2026-10-13): `StandardRenderer` with its ASCII wrapping, theme text and builder (2,290 lines) and
the Unicode `Renderer` with the subset fonts, fallback fonts, tagged hooks and builder (3,080),
the rest — `TtfFont`, `TailWriter`, `PdfObjects`, images and SVG hosts, math, links, headings,
the `RenderBlocks`/`RenderWithFootnotes`/`RenderBook` templates, theme, cover, contents and
page-number helpers, 5,500 lines — in a header of `static` functions that each unit compiles as
far as it uses them. Each unit gets a budget of its own but not a larger one, a share of its own
size, and still stops at it (470 and 563 calls kept out of line for the unit's growth, 935 in the
one file). Same bytes; warm instructions +0.05 %, time +1.0 % (aligned -0.9 %: placement);
fresh README +30 µs CPU, notes +6 µs; +70 KB of code, the shared functions both units keep a copy
of. A split also has to keep one instance of state that the shared code holds in function-local
statics (image-decoding counter, root cache, `--compress` work, curl's init), and `TailWriter`'s
scratch becomes an `inline thread_local`, defined once for all units.

**Byte classes scanned a byte at a time.** The parser's ordinary-byte runs take SSE2 now (Phase
2); left are `NeedsInlineParse` (3 % of the essay's instructions) and `ContainsByteClass` (table
cells, `WriteEscapedLiteral`'s runs, `StripInlineMarkdown`). Their loop ORs a table load per byte
into one register: about 17 instructions per 8 bytes, not the 28 measured on 2026-10-09. Ten SSE2
compares cost more (about 40 per 16 bytes without VEX: every `pcmpeqb` overwrites its operand, so
each needs a register copy); an SSSE3 `pshufb` nibble table about 16 per 16 bytes for the stops
and 13 for the classes of text without one, but behind a call, the CPU test and table loads per
paragraph it gained on the essay only (see *Tried and rejected*). Most table cells are under 16
bytes. The paragraph's searches for '\n', '$', "\(" and "[^" (`WrapAsciiRuns`) and for 0xE2
(`ParseInlineRuns`) are glibc's AVX2 `memchr`, a fraction of an instruction a byte: one SSE2
pass for all of them costs more on long paragraphs. A VEX (AVX) build would drop the copies;
not measured (2026-10-13).

**One text object per line.** Every run is its own `q … BT … ET Q` (about 70
bytes besides the text); 2,322 of 3,999 text objects in the 96 KiB ASCII fixture
follow one on the same line. Switching font and colour inside the line's text
object with `Td` would save 70 to 130 KB of that 569 KB PDF. Changes the bytes of
every document with links, code or emphasis.

**glibc's mmap threshold and per-build temporaries.** Allocations of 128 KiB or
more are mapped until one is freed, which raises the threshold. A reservation of
8x the input instead of 4x kept the threshold low and cost 75 faults and 10 %
per warm build. `mallopt` (`M_MMAP_THRESHOLD`, `M_TRIM_THRESHOLD`) in the CLI or
keeping large temporaries between builds would remove the faults; with
`GLIBC_TUNABLES` at 64 MiB thresholds and a 16 MiB top pad the 96 KiB Unicode
fixture ran 5.4 % faster and `baseline.md`'s bookmarks lost 10 of their 23 µs
(2026-10-09). Measure batch mode before choosing.

**CIDs in order of first use.** CIDs are code points, so `CIDToGIDMap` runs to the
largest one shown: 16 KB for one arrow, up to 80 KB for CJK, often more than the
subset. CIDs numbered by first use would shrink it to two bytes per character, or
to `/Identity`.

**The `name` table of the font subset.** 15.6 KB of a 33 KB DejaVu Sans subset.
It carries the license notice, so it stays; keeping only the records with the
notices would save about 10 KB per Unicode PDF.

**The object table.** `PdfObjects::Object` is 72 bytes. The table is now kept from export
to export (no growth after the first), so only a single export grows it; outline entries
could format their consecutive object numbers once (about 250 instructions a heading).

**`BuildInto` zero-fills what it then overwrites.** `pdf.resize(total)` fills the file's growth
(polish.md: 31 KB, mostly the font subset copied in after) with zeros first: 1.7 % of a warm
polish build. C++23's `resize_and_overwrite` would avoid it; C++17 has no growth without filling.
The remaining per-export allocations are the parser's block strings, the dictionaries moved into
`PdfObjects` (a head kept per object would need callers to write into it) and `WrapText`'s
vector of lines for headings and image fallbacks.

**Phase 2 items left open (2026-10-13).** The fresh-process leftovers (read() into the input
string, the 1 MB buffer and font mapping left to exit) and the renderers in translation units of
their own were not measured in this round.

**Branchless candidate choice in the DEFLATE encoder.** Font programs are its
slowest input (151 MB/s), with about 0.5 mispredictions a byte, 43 % of them on
the window test of the hash candidates. Comparing all three candidates without
branches would leave one unpredictable branch per position. Single large exports
could also compress pages on several threads.

**Hot functions grouped in `.text.hot`.** 16 KB of book code that no document runs
measured +0.58 to +0.66 % pinned on the documentation sites with the same
instructions everywhere (2026-10-09): placement. `__attribute__((hot))` on the
renderers' hot functions would keep edits to cold code from moving them; measure
their speed and whether later measurements steady.

**SVG conversion at `-O2`, and forms kept between documents.** `svg.cpp` at `-O2`:
nine charts 1.82 → 1.40 ms, the 2.1 MB scatter plot 69 → 57 ms, for 53 KB more
code. A form without text and images could be cached like a PNG. Measure how often
batches repeat an SVG first.

**Object streams for tagged documents.** A PDF/UA file has an object per structure
element: 24,228 in the fastapi documentation, 2.6 MB of its 4.4 MB over PDF/A.
Compressed object streams and a cross-reference stream would hold them in a
fraction; the reversible profile accepts only a classic xref table, so tagged
files with an embedded source would keep it.

**Table borders in one artifact per row.** Tagged, each cell's border is an
artifact before its TD: two marked-content sequences a cell, and the table fixture
takes 2.1 times its `--pdfa` time. Borders first, one artifact a row, would halve
them, at a change of `RenderTable`'s loop that untagged documents must not pay for.

**`BuildInto` moves every page's content once.** 2.6 to 4.7 % of a warm build.
Placing content where it ends up needs another object order, which changes the
bytes.

**The ASCII word loop.** `WrapAsciiInline`'s byte loop (a table load, a zero test
and a bound test per byte) mispredicts once per word: about 40 % of the essay's
branch misses. Removing that took more instructions than it saved (see *Tried and
rejected*); a cheaper branch-free sum of a word's advances would be needed.

**The Unicode word loop.** `ScanWideWord` is about 18 instructions a character, bound by its
chain of `addsd`; `WrapStyledInline` spends about 120 more a word (run bookkeeping, line
decisions) and 19 % of README's branch misses. Inlined into `WrapStyledInline` through
`WrapWideWords` (paragraphs without inline syntax), GCC keeps the sum on the stack, a store and
a load in the chain; see *Tried and rejected* for what did not fix it. Headings still wrap
through `WrapText` (a vector and strings per heading, under 1 % of README).

**Keeping the header row's wrapped cells.** A table that breaks wraps its header
row again (about 0.3 % of the table fixture's 1.7 %); keeping them needs a second
set of cell buffers in the hot loop. **Inserting the TOC block** moves every later
block (0.35 % of that build); a slot left by the parser would avoid it at the price
of a test there.

**Fallback-font documents whose first missing character comes late.** The probe stops the
layout in the system font at the first block with such a character; when it is near the end,
that layout is nearly whole, and the source is scanned on top (`NonAsciiCodepoints`, about 80
instructions a CJK character): the essay with CJK lines at its end is 2 % slower warm than
4.6.1. A source whose only characters the system font lacks are not drawn (a comment, a URL)
has the fallback fonts probed for its guess, about 2 M instructions in a fresh process. Both
parse the document again for the second layout; keeping the parse between the two would save
that.

## Tried and rejected

Each was measured, lost, and is described in full in the
[Performance History](https://github.com/Butterski/rayomd/wiki/Performance-History).

Text and layout:

- Carrying run widths from wrapping to painting in the Unicode renderer: no gain
  once the additions had to stay in order.
- Forcing more calls inline in `tiny_pdf.cpp`: moves the loss to other calls.
- Two style fields in `AsciiRun` instead of one byte: `WrapAsciiRuns` +6 %.
- Autolinks: testing each 8-byte block before summing its classes (+0.2 %), the
  check out of line (+0.45 %), a stop mask once no `:`/`@` lies ahead (+0.36 %).
- Counting missing glyphs by decoding the document again: +15 to 27 %; counted
  where `?` is written instead, at no cost.
- Bookmarks with a string per entry and a `/Dests` name tree: +6.5 % instructions,
  +8 % time; one `TailWriter` for the whole outline evicted L1.
- Two-byte CID strings with four compares a byte (+4.5 % instructions) or two
  loads and a byte-wise store (+0.6 %).
- A branchless `WriteFixed2` (+1.84 % instructions, +0.72 % time) and
  `WriteFixed2` forced inline (+1.19 %, +1.81 %). Copying a repeated operand (the
  font size, the baseline) from a cache instead of formatting it: -1.5 % essay
  instructions, +0.4 to +1.5 % cycles (the hit test mispredicts).
- Word ends in `WrapAsciiInline` and `WrapAsciiWords` found with SSE2 and the
  advances summed with four accumulators: +9 % instructions, +7 % cycles on the
  essay (the tail switch mispredicts). Words of up to 8 bytes summed without a
  length branch (16 bytes compared, the rest blanked to spaces): -29 % branch
  misses, +11 % instructions, cycles even, L1i misses x2.7.
- Keeping the default font alive at exit: 1 to 2 % less CPU, no faster by the
  clock (ending the one-shot export with `_Exit`, which skips all of the teardown, is
  kept). Loading the cmap block by block: 10 of 156 faults, a test per glyph
  lookup, atomics for shared fonts.
- Exact cell reservation in table rows: `table_96kb.md` -0.57 % instructions,
  others +0.10 to +0.29 %.
- An SSE2 scan of PDF literal strings: -0.07 %; most strings are short (the same
  scan once per paragraph instead, a flag for all its runs, is kept: essay -4.8 %).
- SSE2 runs of ASCII in UTF-8 decoding: single fixtures -0.5 %, `unicode_96kb.md`
  +0.53 %, as simdutf did.
- `NeedsInlineParse` in SIMD (2026-10-13): ten SSE2 compares a step, inline (essay +0.56 %
  instructions); SSSE3 nibble tables in a template kernel, stops and classes in one pass (essay
  -1.1 % instructions, other fixtures +0.6 to +1.8 % aligned with the same instructions: the
  template moved `tiny_pdf.cpp`'s inlining and took `NeedsInlineParse` out of `.text.hot`); the
  same tables out of line behind the old signature, stops and classes in two passes (essay
  -1.0 % instructions and -1.1 % aligned time; README +0.55 %, table-heavy +0.9 %, tester-noimg
  +0.8 % aligned, notes +0.1 % instructions).
- `ScanWideWord` out of line, the sum in a register: README +2.2 % instructions, +3.0 % time
  aligned. `WrapWideWords` out of line (+0.2 % instructions) and the width cache filled outside
  the word loop (+0.3 %): the sum stayed on the stack. A two- and three-byte fast path in
  `WriteUtf8Units`: -0.1 to -0.2 % instructions on Unicode fixtures, not kept (2026-10-13).
- One 8 KB bitset for plain and used CIDs: a CID shown for a character the font lacks is its
  code point too (drawn as '?'), so the used set needs a bit of its own for every CID.
- A static build against musl: short ASCII 41 % faster, `single_01.md` 45 % and a
  PNG 8 % slower (musl's allocator). glibc's static build is the `-static` asset.
- Highlighting at `-Os` (1.5 times slower lexers for 12 KB) and the highlighted
  path as `RAYOMD_COLD` (more instructions than the lexer).
- Keeping the parser's top-level line tables between exports: -0.35 % instructions on
  README, but a one-shot export held them to the end (README +12 faults, +30 µs). Reserving a
  line per 16 bytes instead of 48: essay +5 faults fresh.
- Headings wrapped through `WrapWideWords` directly instead of `WrapText`'s vector of lines:
  the new instantiation moved GCC's inlining of `WrapText` (table-heavy +0.8 %, aligned too).
- The fallback font guessed from the source before every Unicode layout (2026-10-14): the scan
  of the source cost the Polish note 5.7 % of its instructions; stopping at the first missing
  character without the guess laid `tester.md` with CJK lines out three times (+43 %). The probe
  at that character is kept.
- `StripInlineMarkdownView` in `markdown_parser.cpp`: GCC inlined less into `ClassifyLine`
  (+2 %, notes +0.4 %); it lives in `inline_markdown.cpp`. Plain table cells in buffers not
  kept between exports: notes +0.4 % in malloc.
- The CFF flattener's tokens copied with `memcpy`: at `-Os` that is `rep movsb`, 25 % fewer
  instructions and 8 % more time than the `std::string` it replaced; a byte loop is kept.
- Page footnotes' `ReplayNote` growing the content once (`resize`, then `memcpy`) instead of three
  appends (2026-10-18): perf -0.14 % instructions on 306 notes, but the note's copy of unknown
  length in a cold function became `rep movsb` (callgrind 2.8 times the function's cost). The
  appends are kept.

Images and compression:

- zlib at levels 1 to 9, zlib-ng 2.3.3, libdeflate 1.26 (levels 1 to 12) and ISA-L
  2.31 as encoders: slower, larger, or (ISA-L) larger and NASM assembly.
- zlib-ng and ISA-L inflate (2.1 and 2.3 ms on the mascot against libdeflate's
  1.75), Wuffs v0.4 (6.2 against 4.6 ms), and later libdeflate itself, replaced
  by RayoMD's row-at-a-time inflater.
- Fixed PNG filters: 6 to 20 % faster, streams up to 2.7 times as large.
- Uncompressed streams: a 4 MiB PDF for the mascot.
- SSSE3 or AVX2 RGBA split: no faster than SSE2 without dispatch.
- The 4-byte hash table in image mode: 6.4 times the time on the opaque photo.
- Keeping the four candidate residual rows while scoring: slower than filtering
  again. AVX2 Sub and Average one pixel at a time: slower than SSE2. Two filters'
  sums in one accumulator: within 0.6 %.
- Huge pages for the compressor's output (+1.7 MB, +1 %), for every buffer of a
  long process (batches 3.5 to 5.7 % slower), and glibc's `malloc.hugetlb` (less
  gain than aligned `MADV_HUGEPAGE`).
- A helper thread on a process pinned to one CPU (+1.5 %); filtering in the
  helpers (+3.4 %); segments taken only when complete (helpers idle); helpers for
  every CPU in a batch (1 to 4 % slower).
- Image cache keys by path, size and time (misses edits that keep both) or a
  non-cryptographic hash (collisions to order).
- `-no-pie` for the dynamic build (2026-10-13): 21 to 45 µs less per fresh export (no
  relative relocations), 17 to 48 µs with `-z noseparate-code`, but the executable loses
  address randomisation, and RayoMD reads untrusted Markdown, SVG and PNG input. Not taken;
  the static edition is the fast-start option.

## Keeping the release light

- Build `Release`; never benchmark Debug binaries.
- Keep curl, profiling, TSAN, and LTO off in the default package.
- On Linux, disable zlib discovery only when the smallest portable artifact is
  more important than PNG alpha decoding.
- Keep embedding opt-in. It has no dependency cost, but increases every
  reversible PDF by the exact source plus profile overhead.
- Keep remote image fetching opt-in and exclude network latency from stable
  performance claims.
- Keep generated corpora, PDFs, reports, profiling binaries, and build trees out
  of source control.
- Do not raise the 10 MiB reversible limit without maximum-case output/RSS
  measurements on both platforms.

The router (`tools/benchmark.py`) retains focused implementations under
`scripts/`. Dated measurements go to the wiki: the
[benchmark index](https://github.com/Butterski/rayomd/wiki/Benchmarks) and
[Performance History](https://github.com/Butterski/rayomd/wiki/Performance-History).
