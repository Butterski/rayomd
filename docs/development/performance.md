# Performance development

Correctness and performance are intentionally separate. Build RayoMD first,
run `tests/verify_cli.py` for behavior, and use `tools/benchmark.py` only when
timing or release evidence is needed. Raw reports belong under ignored
`benchmark-output/`.

## Maintained workflows

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-native-ext4 --suite watch --label local
python3 tools/benchmark.py ab -- --baseline build/before/rayomd --candidate build/linux/rayomd --cpu 3 --instructions
python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe --platform windows --suite full --samples 5
python tools/benchmark.py compare -- --rayomd build/windows/rayomd.exe --root benchmark-output/pandoc --runs 5
python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --node-modules path/to/node_modules --root benchmark-output/competitors
python3 tools/benchmark.py release -- --from-version 1.1.0 --suite quick
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
percent; see "Layout fixes" below. It does move with the size of large copies:
valgrind counts every byte of a `rep movsb`, which glibc's `memmove` uses for
large copies that do not overlap, so a change that only moves page contents
further in `BuildInto` can add instructions without adding time.

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

## Engine pass, October 2026

A pass over the native engine (`src/core`, `src/common`) that keeps every PDF
byte. Startup, file I/O, and the executable were left alone.

What changed:

- Page content is rendered straight into the caller's output buffer and the
  file is assembled in place around it, so there is no string per page and no
  second copy of the content. This alone was 7-15 % of a warm build on Windows,
  where allocating and freeing a buffer of 8-64 KiB per page commits and
  decommits memory every time.
- Paragraphs are parsed and wrapped as flat runs: one text buffer per paragraph,
  reused, instead of a string per span and a vector per line. Plain lines are
  views into the source text.
- The block parser splits table rows without building them character by
  character, skips the nested parse for one-line list items and quotes, and
  classifies a line by its first character.
- Operators are written through a pointer into a tail reserved once; numbers
  are formatted without a library call; a line without characters that need
  escaping is copied into its literal string in one piece.
- Look-ahead scans of the inline parser (closing emphasis marker, end of a code
  span or link destination, autolink) are answered from tables once a paragraph
  has read eight times its own size in look-ahead. Ordinary text never gets
  there. Text made of unterminated openers used to be quadratic.

Measured on 2026-10-08, Windows 11, MinGW g++ 15.2 `-O2`, an engine-only warm
loop pinned to one core, best of nine order-rotated rounds, against 2.6.0.
Fixtures are synthetic and contain no math syntax:

| Fixture | 2.6.0 | Now | Change |
| --- | ---: | ---: | ---: |
| 1 MiB ASCII, inline styles, links, lists, tables | 52.5 ms | 12.8 ms | -75.6 % |
| 1 MiB ASCII, the same without inline styles | 36.9 ms | 9.1 ms | -75.4 % |
| 1 MiB Unicode | 79.7 ms | 23.7 ms | -70.2 % |
| 20 KiB ASCII | 1.00 ms | 0.20 ms | -80.0 % |
| 20 KiB Unicode | 1.65 ms | 0.43 ms | -74.0 % |

The executable's own `--bench` loop agrees: 70.7 ms to 18.0 ms and 117.1 ms to
34.0 ms for the two styled 1 MiB fixtures, and 0.93 ms to 0.59 ms for
`tester.md`, which also gained its formulas in between. These are warm numbers:
a single cold export is still dominated by process start and file I/O.

- Peak working set for the 1 MiB fixtures: 30.0 to 16.0 MiB (ASCII) and 38.6 to
  19.9 MiB (Unicode). Page faults per warm build: 3,597 to 802 and 5,305 to 754.
- A 120 kB paragraph made of one unterminated opener: `*a ` 3.3 s to 3 ms,
  `_a ` 3.7 s to 5 ms, `[a](` 2.3 s to 3 ms, backtick runs of growing length
  3.1 s to 5 ms, one table cell of `*a ` 7.4 s to 10 ms. Time now grows in
  proportion to the input: 1 MB of such text takes 25-70 ms.
- Executable size since the math commit: Windows 2,952,704 to 2,720,768 bytes
  (a 256 KiB lookup table is gone; 2.6.0 was 2,820,608); Linux x64 CLI
  (g++ 13.3) 571,952 to 596,528 bytes.

How byte identity was checked, on Windows and on Linux (WSL, native ext4):

- `rayomd-core-tests`, including the golden digests of `CheckNoMathGolden`,
  `CheckOutputBufferReuse` (reused, oversized, and aliased output buffers), and
  `CheckInlineLookahead` (scans against tables on fixed and generated text).
- 76 reference PDFs of the fixtures, `tester.md`, and the math corpus, compared
  with the build before the pass.
- Differential fuzzing of the old against the new engine with generated
  documents: whole PDFs (about 250,000 documents in ASCII, Unicode, and
  mixed-byte modes), block trees (about 1.5 million documents), and inline
  spans (about 2.5 million texts, each with scans, with tables, and with a
  switch between them). Every comparison was equal. Deliberate bugs in each
  changed function were used to confirm that the generators notice them.
- The same tests and fuzzers under AddressSanitizer and UBSan on Linux.

The fuzzers compile two source trees side by side and are not part of the
repository. Rebuild that comparison before any change that claims to keep
output bytes: floating-point sums decide line breaks and coordinates, so a
reordered addition is enough to move a line.

Tried and dropped: carrying run widths from wrapping to painting in the Unicode
renderer (no measurable gain once the additions had to stay in order).

## Layout fixes, October 2026

Six output fixes, so output bytes change on purpose:

- The standard renderer measures text with the exact AFM advances of the font
  that shows it (Helvetica, Helvetica-Bold, Courier) in integer units; the old
  six-class approximation let capitals and bold headings run past the margin.
- Its fonts declare `/Encoding /WinAnsiEncoding`, so `'` and `` ` `` are no
  longer shown as curly quotes.
- A word is the text between two white spaces of the source, across emphasis,
  code, and link boundaries: `[GitHub](u).` shows `GitHub.`, not `GitHub .`.
  Only a word wider than the line is cut by character.
- List items nest by the column of their content (CommonMark), so two or three
  spaces nest under `- ` or `1. `.
- A table row taller than a page continues over pages instead of running off
  the bottom; `[^1]: text` stays visible instead of vanishing as a reference.

Every ASCII document changes bytes. A Unicode document changes only where one
of these cases occurs: the 96 KiB Unicode watch fixture keeps identical bytes,
while `tester.md` and the watch suite's single and batch documents change.

Measured on 2026-10-08: AMD Ryzen 5 PRO 4650GE, Linux x64, g++ 13.3 `-O3` in
Docker. The engine `--bench` loop ran pinned to one core, as eleven rotated
rounds of about 0.35 s per binary, against `21b3ef1` and an identical copy of
that binary, whose difference is the noise:

| Fixture | Before | Change | Alignment pinned | Noise |
| --- | ---: | ---: | ---: | ---: |
| watch single document 1 (Unicode) | 2581 µs | +0.2 % | -0.4 % | +0.1 % |
| watch single document 4 (Unicode) | 1915 µs | -0.0 % | -0.1 % | +0.2 % |
| watch batch document 1 (Unicode) | 1662 µs | -0.0 % | -0.2 % | +0.1 % |
| 96 KiB Unicode | 2140 µs | -1.0 % | -0.8 % | +0.4 % |
| `tester.md` (Unicode) | 296 µs | +0.9 % | +0.8 % | +0.2 % |
| `README.md` (Unicode) | 168 µs | +1.0 % | +2.1 % | +0.1 % |
| 96 KiB ASCII | 1511 µs | -3.2 % | — | -0.6 % |
| 96 KiB tables | 1304 µs | -5.3 % | — | -0.1 % |

"Alignment pinned" builds all binaries with `-falign-functions=64
-falign-loops=32`, which takes most code-placement effects out; its noise was
up to 0.7 %. Callgrind counts +0.35 % to +0.62 % instructions on the Unicode
documents: words across style and link runs, and list nesting by content
column. `README.md` (+1.0 %) also cuts words wider than a line instead of
letting them run past the margin.

An intermediate version measured +0.9 % to +1.9 % on the Unicode documents in
the release build and nothing with alignment pinned, at an instruction count
within 0.05 % of the final one. Read a release-build difference of that size
as code placement until the alignment-pinned build and the instruction count
agree with it.

The watch suite on ext4, as medians of five alternating runs: `warm_avg`
-0.7 %, cold export +0.7 %, batch +0.4 %, stdin batch +0.0 %, serve -0.3 %,
`sized_unicode` -1.2 %, `sized_ascii` -3.0 %, `sized_table` -5.4 %; peak RSS
within run-to-run noise. Against the first record with `--fail-on-slower-pct
5`, the before binary's own reruns failed 2 of 4 times (`cold_export_ms_p95`),
the change 2 of 5 times (`cold_export_ms_p95`, and once a `feature_rules` case
of 0.22 ms that is reported in 0.01 ms steps). On this machine those metrics
move more than the gate allows. The Linux CLI grew from 596,528 to 612,912
bytes (text +16.1 kB).

`tiny_pdf.cpp` sits at GCC's `-O3` inline-unit-growth limit: inlining grows the
unit by 26 % and then stops, and the order is set by the whole file. Any edit
can therefore move which calls are inlined, also far from the edit: removing a
style check from `WrapStyledRuns` changed the out-of-line `WriteFixed2` calls
of a build from 10,472 to 3,590. An unrelated lambda in table cell wrapping and
`DrawStrokeRect`/`Rect` in the table loops went out of line during this work.
Countermeasures that held up:

- `RAYOMD_HOT_INLINE` for hot helpers and `RAYOMD_HOT_LAMBDA` for hot lambdas;
- rare paths in `RAYOMD_COLD` functions that do not call small hot helpers
  directly. Tall rows only plan their slices out of line; the one table loop
  paints them;
- values that a `char*` write could alias (buffer starts, run state) passed by
  value into hot helpers, so they are not reloaded per word.

Forcing more calls inline was not kept: under the limit it only moves the
loss to other calls.

## Latin text on the standard fonts, October 2026

A document whose characters all have a WinAnsiEncoding code (Western European
text, curly quotes, dashes, the Euro sign) is transcoded once in `BuildPdf` and
takes the standard-font renderer instead of embedding a TrueType subset. Pure
ASCII documents keep their path and their bytes; other documents first try the
transcoder, which stops at the first character outside WinAnsi (within the
first 175 characters in every watch fixture).

Measured on 2026-10-08 with `tools/benchmark.py ab` (g++ 13.3 `-O3`, nine
rounds, against `6bd2585`): Latin documents build 39 % to 48 % faster
(`README.md` -48 %), and their PDFs shrink from 140 to 225 KB to 5 to 30 KB.
Watch fixtures run 0.24 % fewer instructions (geometric mean); their time
moved +0.5 % in the release build and +0.2 % with alignment pinned, both
within the A/A spread of code placement.

Link targets of both renderers are now percent-encoded UTF-8: `/URI` strings
used to lose their non-ASCII bytes (`Größe` became `Gre`).

## Emphasis on the standard fonts, October 2026

The standard-font renderer used to drop emphasis: `**bold**`, `*italic*` and
`~~strike~~` came out as plain Helvetica. It now shows bold in Helvetica-Bold,
italic in Helvetica-Oblique, both in Helvetica-BoldOblique and strike-through as
a line, like the Unicode renderer. The oblique faces go into the PDF only when
text uses them, so documents without emphasis keep their bytes.

Measured on 2026-10-09 with `tools/benchmark.py ab` (g++ 13.3 `-O3`, nine
rounds, against `336e0df`): documents without emphasis build the same bytes in
the same time (-0.0 % to +0.4 %, within the A/A spread) and run 0.13 % more
instructions at 96 KiB (0.46 % for `baseline.md`). The watch fixtures with
emphasis run 5.2 % more instructions and take 4 % to 5 % longer: every change
of style is a text object of its own, and their PDFs grow by 12 %.

Two details keep documents without emphasis at their old cost:

- Code and emphasis share one style byte in `AsciiRun` and `AsciiSegment`, so
  placing a word still compares one byte with the run before it. Two fields
  made `WrapAsciiRuns` 6 % slower.
- `StyleAdvances` reads the advance table of a style from a table of pointers.
  From a computed font index GCC kept the table base and the font offset in two
  registers and added them for every byte the word loop looks up: one
  instruction more per byte, 0.3 % of a 96 KiB ASCII build.

## Table cells with inline Markdown, October 2026

The parser used to flatten every table cell to plain text: emphasis, code and
links in cells were lost, and `\*` became an emphasis marker once its backslash
was gone. Cells now keep their inline Markdown (tables with formulas still
flatten, as before), and both renderers wrap and paint them like paragraph
text. A cell without inline syntax keeps the plain path and its bytes; any
other goes through `WrapAsciiInline` or `WrapStyledInline` into buffers that
are reused from row to row and table to table. Unicode tables no longer build
a vector of wide strings for every row.

Two fixes came with it. Code backgrounds start at the first glyph of the code;
they used to cover the space before it and 1.5 pt of the word before. The
Unicode renderer measures the last run of a line only when it needs the width
(a link or a code background), so most lines are no longer measured twice.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`28692a1`): Unicode documents take 0.1 % to 4.6 % less time (`unicode_96kb.md`
-4.6 %, -7.0 % instructions); `baseline.md` +0.3 % and `ascii_96kb.md` +1.4 %,
whose tables now show formatting; `table_96kb.md` +5.6 %, whose 142 tables now
show bold, code and 142 links in 9.5 % more output. Its instruction count rises
10.8 %, of which 0.65 M per build is `memmove` in `BuildInto`: the extra link
objects move every page further, so the moves no longer overlap and glibc
copies them with `rep movsb`, which valgrind counts byte by byte.

`PaintAsciiRuns` and `PaintStyledRuns` paint a line for paragraphs, quotes and
table cells. Inlined into each caller, together with a clone GCC made of
`WrapStyledInline` for the table's constant text size, they added 13.5 KB of
code; `RAYOMD_SHARED` keeps one copy of each, and the whole change adds 3.6 KB.

## Link text with inline Markdown, October 2026

Link text was taken literally: `[**bold**](u)` showed its asterisks, and in a
linked image, `[![alt](src)](u)`, the link ended at the image's own `]`, which
left a broken link and stray brackets (the badge rows at the top of many
READMEs). Link text with inline syntax is now parsed on its own, out of the
parse loop, and its runs keep the link and the emphasis around it; plain link
text takes the old path. `[![` either starts a linked image or is a literal `[`,
so a failed attempt never sends a bracket search backwards. A line that is one
linked image becomes an image block with a link annotation over the image, or
over its fallback text. Pieces of one link that follow each other on a line now
share one annotation: link text in several styles would otherwise get one per
style, and `[**x` repeated 50,000 times before `](u)` made an 11.7 MB PDF
instead of 3.8 MB.

An image block's source and target sit behind a pointer, `Block::image`. Kept
as a second string in every block, the target made each move of a block dearer
(+0.4 % instructions for `baseline.md`); behind the pointer every block is 24
bytes smaller than before.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`5404696`): the watch fixtures write the same bytes with +0.06 % instructions
(geometric mean). Their time moved +0.8 % in the release build and +0.1 % with
alignment pinned, against an A/A spread of 0.2 %: code placement.

## Fallback fonts, October 2026

Characters the default font has no glyph for, such as Chinese with DejaVu Sans
or Segoe UI, came out as `?`. The Unicode renderer still draws a document in
one font, chosen after the first render: the characters it showed (a few
hundred CIDs) are checked against the font, and only when some are missing are
the system's broad fonts consulted, `RAYOMD_FALLBACK_FONT` first. Each
candidate is read once per process for the characters it covers, and the
document is drawn again in the one that shows the most; only fonts that drew a
document stay in memory. TrueType collections (`.ttc`) load now, the first font
of them, which brings the CJK fonts of Windows and of most Linux distributions.
A candidate must show Latin text too, since it draws the whole document, and
must have TrueType outlines (Noto Sans CJK is OpenType CFF and does not load).

What no font shows, including every character beyond the BMP such as emoji, is
counted where its text is written: `BuildResult::missingCharacters` now covers
the TrueType path too, and the CLI warns. A first version counted by decoding
the whole document again and cost 15 % to 27 % on documents with such
characters; counting on the path that writes `?` costs nothing measurable.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`65d457e`, no fallback font installed): the same bytes everywhere; the Unicode
fixtures, which hold CJK text DejaVu Sans lacks, run 0.2 % to 0.4 % more
instructions for the check and the count, in the same time.

## Smaller font subsets, October 2026

The embedded TrueType subset kept every glyph id of the font: `loca` and `hmtx`
held all 6,253 glyphs of DejaVu Sans, empty or not, and `post` all their names.
Of a typical 151 KB subset only 14 KB were outlines; with a CJK font a few
characters made a 959 KB subset. The subset now holds only the glyphs the
document shows, numbered from 0 (`SubsetGlyphs`, composites pointing at the new
ids of their components), `post` is format 3 without names, and the `cmap` maps
nothing, since the PDF's `CIDToGIDMap` does. A font whose tables cannot be read
for that (`CanSubset`) is still embedded whole with its own ids.

Measured on 2026-10-09: 66 Unicode documents render pixel for pixel the same in
MuPDF and Poppler (also at 300 dpi) with the same extracted text, in 34 % fewer
bytes (22.6 MB to 14.9 MB); a short Unicode document shrinks from 176 KB to
58 KB, the DejaVu subset in it from 151 KB to 33 KB. Single exports run 2.7 %
to 12.7 % faster; warm `--bench` builds, which reuse the cached font object,
run 1.4 % fewer instructions in the same time.

## Bookmarks and links to headings, October 2026

Headings now make the PDF outline (bookmarks), nested by level, and a link to
`#anchor` goes to the heading with that anchor as GitHub makes it: lower case,
spaces as `-`, punctuation and emoji dropped, repeats numbered `-1`, `-2`. A
percent-encoded fragment or one in other case still matches, and `#` and `#top`
go to the first page. A link to an anchor no heading has is dropped instead of
becoming a dead `/URI (#x)` annotation. The renderers note each heading's page
and top where they draw its first line, with a view of its text in the blocks.
Anchors are computed only at the first `#` link, in one hash map that also
numbers the repeats. Such a link's annotation is reserved in its page's place
and written with an explicit destination once the pages exist, so documents
without internal links keep their object numbers.

The outline entries go one after another into one buffer that the objects view
(`PdfObjects::AddView`); each is written in a scratch buffer that stays in L1.
A first version gave every entry a string of its own and named every heading in
a `/Dests` name tree: +6.5 % instructions and +8 % time over the watch fixtures.
Writing the whole outline through one `TailWriter` cut the instructions, but its
zero-fill evicted L1: cachegrind counted 514,800 D1 misses over 41 builds of
`baseline.md` before and 603,300 with it, 585,300 with the scratch buffer.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`a74d14b`): in all 2,700 corpus exports without `#` links the PDFs differ only
by the outline objects and the catalog's `/Outlines`. The watch fixtures run
0.6 % more instructions (geometric mean) in 1.8 % more time; `baseline.md`, 73
headings in 18 KB, +4.1 % instructions and +10 % time. Half of that is glibc:
the outline's temporaries lift the build's heap peak past the 128 KiB top pad,
so every build trims the heap and faults a page back in (1.1 faults per build
instead of 0.1). With the trim and mmap thresholds raised through
`GLIBC_TUNABLES`, `baseline.md` costs +3.9 % and the fixtures +1.0 %. 20,000
headings with 20,000 links to them export in 55 ms; a document of nothing but
20,000 headings grows from 3.6 MB to 6.6 MB, as an outline entry is about as
large as the heading it points to. The executable grows by 12 KB.

## UTF-8 decoding, October 2026

Text that is not UTF-8 ended the process. On Linux the Unicode renderer decoded
with `std::wstring_convert`, which throws on a malformed sequence, and nothing
caught it: one stray byte in a document the standard fonts cannot show aborted
the export. A small decoder (`WriteUtf8Units`, on the now inline
`RayoMd::Text::DecodeUtf8`) writes U+FFFD for each byte that breaks a sequence,
as `MultiByteToWideChar` does on Windows, and `AppendUtf8ToWide` decodes
straight into the caller's buffer instead of a temporary string.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`224b722`): all 2,757 corpus PDFs are byte for byte the same, and the Unicode
watch fixtures run 4 % to 6 % fewer instructions in 6.5 % to 9.5 % less time
(geometric mean of all fixtures -4.9 %): the library decoder's allocation and
per-call setup cost more than the decoding.

## Page numbers, October 2026

`PdfOptions::pageNumbers` (CLI `--page-numbers`) writes "N / M" centred in the
bottom margin of every page. The pages are rendered before their count is
known, so each number is a small content stream of its own that the page lists
after its content, in Helvetica (Unicode pages get it as `/FN`). It is opt-in:
without it the PDFs are byte for byte the same (2,757 corpus PDFs, against
`d2882a4`) and the watch fixtures run the same instructions. The release build
measured +1.1 % time with those same instructions, and +0.1 % with alignment
pinned: code placement again.

## HTML comments, line breaks and character references, October 2026

HTML comments are no longer shown. A line that starts with `<!--` hides
everything up to the `-->` that closes it (to the end of the document when none
does, as in CommonMark; text after `-->` on its line stays), and the inline
parser drops a comment inside text with the spaces after it when a space comes
before. Its `-->` lookups go through `InlineScanner::NextCommentClose`, which
keeps its last answer: 20,000 unterminated `<!--` in one paragraph take 3 ms.
`<br>` (`<br/>`, `<br />`, any case) ends the line: the inline parser writes it as
a run of one space with `InlineRun::lineBreak`, which the paragraph and table
cell wrappers act on once per run or segment, never per byte; every other
reader takes it as a space. Character references (`&amp;`, `&#169;`, `&#xA9;`,
the named ones of HTML 4 and `&apos;`, `&check;`, `&cross;`) show their character
except in code. The inline parser writes them in the build's encoding, UTF-8 or
the WinAnsi codes of the standard fonts (`WinAnsiReferences`), and `BuildPdf`
sends a document whose references need more than ASCII to the WinAnsi path and
one with a character WinAnsi lacks (`&rarr;`) to the Unicode renderer, after a
`memchr` for `&`.

A first version ran 1.1 % more instructions on `baseline.md`, which holds none of
this: `string_view::compare` called `memcmp` for every `<`, and the grown
`ParseInlineRuns` no longer inlined `RunWriter::Close` and
`InlineScanner::BacktickClose`. Byte compares and `always_inline` left the `memchr`
(+0.16 %). The names of the 255 references as pointers cost a relocation each in
the position-independent executable, 12 KB in all; held in place they cost 4 KB.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`ae4459c`): all 2,520 corpus PDFs that changed come from documents with a
reference, a comment or `<br>`; the watch fixtures write the same bytes with
0.09 % more instructions (geometric mean, at most 0.18 %) in the same time.

## Task lists, October 2026

A list item that starts with `[ ]`, `[x]` or `[X]` shows a checkbox, a check mark
in it when done, where its bullet or number would be (as on GitHub), and its text
indented after it. The box and the mark are paths (`AppendCheckbox`), so the
standard fonts need no glyph for them. The parser keeps the state in
`Block::task`.

Where that byte sits mattered. After `bool hasMath`, at the end of `Block`, it
left the size at 136 bytes but changed how GCC copies the tail: the
reallocation of the block vector grew from 1,143 to 1,716 bytes of code and
`baseline.md`, which has no task, ran 0.47 % more instructions. In the padding
after `int number` it costs nothing. Measured on 2026-10-09 against `e19cf7d`:
the 1,407 corpus PDFs that changed hold task items, and the watch fixtures
write the same bytes with the same instructions.

## Bare URLs and email addresses, October 2026

A bare `http://` or `https://` URL and an email address in text become links, as
on GitHub: the URL runs to white space or `<`, without the punctuation that ends
a sentence, a `)` that closes no `(` of its own or a character reference at its
end; an address needs a domain of two or more parts. Code spans, link text and a
scheme inside a word (`xhttps://`) stay plain. The parser finds both at their `:`
or `@`, a byte class of their own (`kByteAutolinkLead`), and `NeedsInlineParse`
sends text without other inline syntax to the parser only when it holds a `://`
or `@`, in the same pass that looks for syntax.

Every test that can fail takes constant time; only a URL that becomes a link is
scanned to its end. A first version scanned to the next white space before it
tested the byte after `://`: 40,000 `http://(` in one paragraph took 2.3 s, growing
with the square of the length. They take 5 ms now, and `CheckInlineLookahead`
holds a megabyte of them.

The time costs more than the instructions suggest, and it is work, not layout:
with pinned alignment in one session, a build with only the `:` and `@` stops in
the parser ran 0.62 % slower than `9bdb863`, one with only the fast-path check
0.65 %, both 0.90 %, and one with all the code but neither 0.38 % (0.07 % in
another session, so sessions differ by about 0.3 %). Callgrind's branch
simulation shows no new mispredictions. Variants that ran more instructions:
testing each 8-byte block before adding its classes to the sum (+0.2 % on the
table fixture, whose short cells end in the byte loop), the check out of line
(+0.45 %, a call per cell), and a stop mask that drops `:` and `@` once none
lies ahead (+0.36 % on `single_01.md`: `RunWriter::Flush` was no longer inlined).

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`9bdb863`): the 2,187 corpus PDFs that changed hold a bare URL or address; the
watch fixtures write the same bytes with 0.19 % more instructions (geometric
mean, at most 0.26 %) in 0.7 % more time (0.84 % with pinned alignment).

## Shortcut reference links, October 2026

`[label]` and `![label]` alone resolve when the label is defined, as in
CommonMark; before, only the full (`[text][label]`) and collapsed (`[label][]`)
forms did. An escaped `\[` no longer starts a reference of any form. Only
documents with a reference definition pay: `ResolveReferenceLinks` returns at once
without one. A label holds no `[`, so the labels tried never overlap. Without
that rule each `[` in front of one `]` normalized everything up to it: 20,000 of
them took 0.57 s, growing with the square of their number; 400,000 take 13 ms
now, and `CheckInlineLookahead` holds 120,000. Each label tried is
normalized into a string and looked up, which is why `tester.md`, whose
bracketed text resolves to nothing, runs 0.16 % more instructions.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`54d89b0`): the 435 corpus PDFs that changed come from documents with a
reference definition; the watch fixtures write the same bytes with 0.02 % more
instructions (geometric mean) in the same time.

## Inlined content-stream writes, October 2026

`TailWriter::Lit` and `TailWriter::Bytes`, which copy an operator or operand of
known size into a content stream, are always inlined now. GCC had left `Lit<2>`
out of line in the paint functions (`StandardRenderer::Text`,
`Renderer::PaintText`): a call for each one-byte separator. It came to light when
a cold caller added for alerts moved `Lit<5>` out of line as well.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`6a557f9`): all 2,757 corpus PDFs are byte-identical, and the watch fixtures run
1.6 % faster (geometric mean; the 96 KiB table fixture 3.3 %, `baseline.md` 2.7 %)
with 0.39 % fewer instructions. The executable grows by 4 KB, to 678,448 bytes.

## GitHub alerts, October 2026

A top-level quote whose first line is `[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`,
`[!WARNING]` or `[!CAUTION]` (any case) with text after it is an alert, as on
GitHub: its bar takes the alert's colour, and a bold title in that colour takes
the marker's place and keeps the first line of the text on its page. A quote in
an alert keeps a plain quote's bar; a marker alone, an unknown one, and one in a
list or a nested quote stay text. The parser keeps the kind in `Block::alert`, in
the padding next to `task`, and both renderers draw every quote strip through
`QuoteStrip` in the colour `quoteBar`.

Before the content-stream writes were inlined, the new cold `RenderAlert` made
GCC call `TailWriter::Lit<5>` out of line in `Renderer::PaintText` (+0.28 %
instructions on the 96 KiB Unicode fixture). `TakeAlertMarker`, inlined into
`ParseMarkdownImpl`, turned a string constructor there into a call (+0.24 % on
`baseline.md`), so it stays out of line.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`81b1f4e`): the 1,197 corpus PDFs that changed hold an alert; the watch fixtures
write the same bytes with the same instructions (+0.01 %) in the same time.

## Headings kept with what follows, October 2026

A heading no longer ends a page. Its page must keep room under its first line for
the space after it, the headings right after it (each with its spaces and first
line) and two lines of what follows them; otherwise the heading starts the next
page. `RenderBlocks` works that room out (`HeadingKeep`) and hands it to
`RenderHeading`, a few additions per heading.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`7cf79ee`): 23 of the 919 corpus documents (Modern style) paginate differently, 4
with a page more, and show the same text in the same order (`mutool draw -F txt`;
in one, the order in which it reads table cells next to a moved break); the watch
fixtures run the same instructions (+0.01 %) in the same time.

## Table headers on every page, October 2026

A table that goes on to a new page repeats its header row there, unless it and the
row that moved do not fit a page together, and the header row keeps a line of the
next row with it. The three table loops (`RenderTable` in both renderers and
`RenderMathTable`) do it the same way: the row loop goes back to row 0, then draws
the row that moved, so the drawing code stays in one place and both rows are
wrapped anew once per break. Keeping the header's wrapped cells for later would
need a second set of cell buffers in the hot loop.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`93e5743`): of the 919 corpus documents (Modern style), 134 show repeated header
rows and 17 only paginate differently; their own text is unchanged, compared as
lines (`mutool draw -F txt`) or, where mutool reads table cells next to a moved
break in another order, as words. Fixtures whose tables fit a page run the same
instructions; the table fixture draws 20 more header rows on its 61 pages with
1.7 % more instructions, about 0.3 % of them for wrapping again.

## Page sizes, October 2026

`PdfOptions::pageSize` (CLI `--page-size`) sets the page: the presets A4 (still the
default), A3, A5, Letter and Legal, each also in landscape, or any size in mm, cm,
in or pt. Each side stays within 144 to 14,400 points, PDF's own limit, and a
margin too wide for the page shrinks to leave 72 points of text. The 62 uses of
the `PAGE_W`/`PAGE_H` constants became the renderers' `pageW`/`pageH` members, read
where the constants were, so A4 runs the same arithmetic in the same order.

Measured on 2026-10-09 with `tools/benchmark.py ab` (nine rounds, against
`8b06b99`): all 2,883 corpus PDFs are byte-identical at A4, and the watch fixtures
run the same instructions (within 0.02 % each); time moved +0.19 % with pinned
alignment (A/A -0.60 %), which is noise for a load in place of a constant.

## Batch reports, October 2026

Batch modes gained `--report=FILE` (one JSON line per document: status, exit code,
pages, bytes, milliseconds, characters without a glyph, failed images; `-` for
standard output), `--recursive` (subfolders mirrored under the output folder,
hidden ones skipped) and `--skip-unchanged` (a PDF newer than its Markdown and
ending in `%%EOF` is kept, so a stopped batch can go on and a cut-short PDF is
redone). Workers write their own lines, buffered and flushed at least once a
second, so a large batch can be followed live. `BuildResult` now reports `pages`
and `failedImages`. The pieces both command lines share live in
`src/common/batch_report.cpp`; the Windows copy was checked in a MinGW cross-build
run under Wine.

Measured on 2026-10-09: all 2,883 corpus PDFs are byte-identical, the watch
fixtures run the same instructions, and time moved +0.51 % with pinned alignment
(A/A -0.10 %), code placement after 8 KB more command-line code. 20,000 report
documents of 4.3 KB (tmpfs, 6 workers) convert in 0.66 s, 0.67 s with
`--report` (0.5 µs a line); a rerun that skips them all takes 0.29 s, 14 µs per
document including listing 20,000 files.

## Opt-in compression, October 2026

`--compress` (`PdfOptions::compress`) writes page content, the font program, the
ToUnicode CMap and the CIDToGIDMap as FlateDecode streams, each only where the
file gets smaller, counting the 21 bytes of `/Filter /FlateDecode `. Page
numbers, images (already DCT or Flate), the XMP metadata and an embedded source
stay as they are, so the reversible profile still reads its source. Page streams
are compressed where they were rendered: none grows, so each moves to the front
of the output buffer without overwriting one not yet read, and `BuildInto`
assembles the file around them as before. Compressed font programs have a cache
of their own beside the plain ones.

The encoder is RayoMD's own, `src/core/flate.cpp` (11.6 KB of code at `-O2`), not
zlib: the Windows build has no zlib, zlib's level 1 is slower on these streams
and writes larger ones, and every `compress2` call sets up and clears about
270 KB. The design came from measuring libdeflate, zlib-ng, ISA-L and zlib on
streams RayoMD writes:

- Greedy parsing over two hash tables: the two most recent positions per 8-byte
  key, and the most recent per 4-byte key, which is what finds matches in text and
  CMaps.
- Every position inside a match of up to 16 bytes enters both tables, only the
  first and last four of a longer one: within 0.5 % of inserting them all, a
  fifth faster on pages and four times on the zero runs of CIDToGIDMaps.
- Past 32 literals in a row the following bytes are taken along unsearched at a
  growing step, as LZ4 does: font programs 14 % faster for 1.2 % more bytes,
  pages within 1.5 %.
- Blocks of at most 64 KB, each stored, fixed or dynamic by exact bit count;
  Huffman lengths limited to 15 bits as libdeflate does, which keeps the code
  complete at every step.
- SSE2 Adler-32 as in libdeflate (+9 % on pages over the unrolled scalar loop),
  and the bit writer copied into a local so it stays in registers.
- The hash tables are kept per thread and never cleared: positions count from a
  base that moves a window past each input, so no earlier entry is ever within
  reach. That took a 160 KB clear off every 10 KB page.

Output depends only on the input: the MinGW build under Wine (4 workers) wrote
the same bytes as Linux for all nine test documents. The core tests inflate the
encoder's output with zlib at block, window and length-limit edges (check 89) and
check that every stream of a compressed PDF inflates to the uncompressed
export's; `verify_cli.py` does the same with Python's zlib on both platforms.

In-process on 2026-10-09 (Ryzen 5 PRO 4650GE, one CPU, best of nine; zlib 1.3,
one `compress2` call per stream), on streams extracted from RayoMD's output of
the corpus:

| Streams | RayoMD | zlib -1 | zlib -6 | Size vs zlib -6 |
| --- | ---: | ---: | ---: | ---: |
| Pages, standard fonts (182, 1.97 MB) | 349 MB/s | 113 MB/s | 78 MB/s | 1.064 |
| Pages, Unicode font (344, 4.58 MB) | 369 MB/s | 165 MB/s | 91 MB/s | 1.084 |
| Font programs (46, 1.38 MB) | 151 MB/s | 73 MB/s | 48 MB/s | 1.049 |
| ToUnicode CMaps (46, 68 KB) | 142 MB/s | 23 MB/s | 18 MB/s | 1.025 |
| CIDToGIDMaps (46, 3.1 MB) | 2,700 MB/s | 988 MB/s | 330 MB/s | 1.006 |

RayoMD's output is smaller than zlib -1's on every set. End to end (`--bench`,
300 warm exports, best of three; the font program is compressed once and then
comes from the cache, as in a warm server):

| Document | Plain | `--compress` | Plain bytes | Compressed bytes |
| --- | ---: | ---: | ---: | ---: |
| `ascii_96kb.md` | 1.56 ms | 2.66 ms | 589,400 | 161,605 (0.27) |
| `table_96kb.md` | 1.33 ms | 2.74 ms | 739,391 | 157,901 (0.21) |
| `unicode_96kb.md` | 1.89 ms | 3.72 ms | 927,993 | 189,460 (0.20) |
| `single_01.md` | 2.32 ms | 3.45 ms | 520,909 | 105,188 (0.20) |
| `tester.md` (images) | 0.30 ms | 0.51 ms | 392,411 | 346,078 (0.88) |

20,000 documents of 4.3 KB (`--batch`, 6 workers, tmpfs) take 0.67 s plain and
0.92 s with `--compress`, and 609 MB of PDF become 201 MB. Peak memory grows by
about 0.5 MB per worker (the tables and tokens).

Without `--compress` all 2,883 corpus PDFs are byte-identical and the watch
fixtures run 0.19 % fewer instructions; time moved -0.61 % (A/A -0.31 %), and
+0.49 % with pinned alignment (A/A -0.16 %): placement of 6 KB more engine code.
The binary grew by 20 KB.

## Front-matter metadata, October 2026

The YAML front matter now gives the document information dictionary its
`/Author`, `/Subject` (else the description) and `/Keywords`, and the catalog its
`/Lang`, besides the title it gave before. `FrontMatterMetadata` reads the subset
documents use: scalars on the key's line, bracket lists, and `- ` lists whose map
items (Pandoc's structured authors) give their `name`. Several authors are joined
with "; " and keywords with ", ", as Pandoc's PDF metadata has them; `lang` must
be a well-formed language tag. The front matter is read once per export, after
rendering, and only when the document starts with `---`.

Measured on 2026-10-09 against the previous commit: of the 2,883 corpus PDFs,
1,881 are byte-identical and 1,002 differ object by object only by the new
entries (678 `/Author`, 558 `/Subject`, 528 `/Keywords`, 354 `/Lang`). The watch
fixtures, which have no front matter, run the same instructions (+0.02 %); time
moved +0.30 % (A/A -0.34 %), and -0.60 % with pinned alignment (A/A +0.19 %).

## Company themes, October 2026

`--theme=FILE` (`PdfOptions::theme`) lays a company look over a style: a TrueType
font for all text, heading, link and accent colours, header and footer fields with
placeholders and a logo, and a cover page. The file is `key = value` lines
(`ParseTheme`, relative paths the theme folder's).

- The renderers take their four themeable colours from a `ThemePalette` built per
  export: pointers to operand strings, the style's literals by default, so a hot path
  reads a member where it had a literal and nothing else changes.
- A theme font is loaded once per path and kept, and takes the document to the
  Unicode renderer without the fallback-font switch, which would undo the branding.
- The header and footer are one overlay stream per page, as the page numbers are,
  which they then also draw. Text that WinAnsi cannot hold takes the document to a
  Unicode font; there the overlay glyphs join the subset before it is cut.
- The cover is a page of its own, first in `/Kids`, with `/PageLabels` "Cover, 1, 2,
  …" so that viewers count as the footer does; outline entries and links keep their
  pages.
- The layout (placeholders, ellipses, wrapping, the cover) lives in `theme.cpp`, built
  at `-Os`, behind a small virtual font interface. Kept in `tiny_pdf.cpp`, this cold
  code moved GCC's inlining of the hot paths: `std::string::push_back` and two
  appends in the link annotations went out of line through the PLT, +0.85 %
  instructions on the watch fixtures and +2.6 % on `baseline.md`. Moving the layout
  out, and writing `AppendUriLiteral` and `AppendLinkAnnotationStart` through
  pointers instead of a call per byte or piece, brought every watch fixture to within
  0.1 % of the previous instruction count.
- `ContainsByteClass` is now always inlined: the colours alone had pushed it out of
  `WriteEscapedLiteral` (+0.74 % instructions on `baseline.md`).
- `export_options.cpp` joined the `-Os` files: the theme parser had taken it from
  3.8 KB of code to 12.1 KB at `-O3`; it is 6.5 KB at `-Os`.

Measured on 2026-10-09 against the previous commit: all 2,883 corpus PDFs are
byte-identical, also with `--page-numbers --compress`. The watch fixtures run 0.16 %
fewer instructions and took 0.53 % less time (A/A -0.00 %). The binary grew by
12 KB.

## PDF/A-3b, October 2026

`--pdfa` (`PdfOptions::pdfa`) writes PDF/A-3b (ISO 19005-3, level B), validated
with veraPDF 1.30.3 (`verapdf -f 3b`): plain, Unicode, fallback-font, image, math,
themed (cover, logo, header) and reversible exports pass, as do the Windows build's
under Wine.

- PDF/A embeds every font, so all text takes the Unicode renderer. Page numbers,
  Helvetica elsewhere, become the footer band that themes draw in the document's
  font (same grey, size and place). Formulas show their TeX source: the math fonts,
  Times and Symbol, are not embedded and RayoMD has no outlines of them. Without a
  TrueType font the export fails (`PdfaFontUnavailable`, exit 18) rather than fall
  back to the standard fonts.
- The sRGB output intent carries a 480-byte ICC v4.2 profile written as data in
  `pdfa.cpp`; common sRGB profiles are 3 KB or more.
- One XMP packet holds the PDF/A identification, the document's metadata and, with
  `--embed-source`, the reversible profile's properties with their extension
  schema; the file then stays PDF 1.7. `PdfaText` gives the Info dictionary and
  the packet the same text: well-formed UTF-8 without controls, at most 2 KiB of
  XML each, which keeps the packet within the 16 KiB `Inspect` reads.
- The trailer's `/ID` hashes the file's body with three independent lanes of
  128-bit multiplies: 30 GB/s, 31 µs for a 930 KB PDF. A first version, whose two
  lanes waited on each other, took 110 µs (8 GB/s): `--pdfa` then added 7.6 % to a
  96 KiB Unicode export, and adds 4.3 % now.
- Link annotations get `/F 4` (print), and the trailer and `Inspect` accept hex
  strings.

Cost of `--pdfa` per warm export (`--bench`, cpu 3, median of five):

| Fixture | Default | `--pdfa` | Size | With `--compress` |
|---|---:|---:|---:|---:|
| baseline.md (ASCII) | 0.25 ms | 0.43 ms (+72 %) | 109 → 202 KB | 71 KB |
| ascii_96kb.md | 1.56 ms | 2.06 ms (+32 %) | 589 → 905 KB | 197 KB |
| unicode_96kb.md | 1.84 ms | 1.92 ms (+4.3 %) | 928 → 930 KB | |
| tester.md | 0.32 ms | 0.26 ms (-19 %, no typesetting) | 392 → 391 KB | |

Measured on 2026-10-09 against the previous commit: all 2,883 corpus PDFs are
byte-identical, also with `--page-numbers --compress` and with `--embed-source`.
The watch fixtures run 0.10 % fewer instructions (time -0.66 %, A/A -0.75 %). The
binary grew by 20 KB: 3 KB of PDF/A code and data; the rest is GCC inlining more
in `tiny_pdf.cpp` (`RenderTable` +2.1 KB, `RenderCode` +1.1 KB) and splitting both
builders into hot and cold parts. Keeping `ThemePalette` and `TtfFont::Load` out
of line saved only 288 bytes.

## Footnotes, October 2026

Footnotes as GitHub reads them (cmark-gfm's rules): `[^label]` refers to a
`[^label]:` definition anywhere in the document, quotes and list items included; a
definition interrupts a paragraph and goes on in lazy lines and in lines indented
four columns. Notes are numbered by first reference in reading order, the text then
each note in turn (unlike GitHub, references in an unused definition take no
number), and set after the text below a short rule, at 0.85 times the body size,
each note's number linking back to its first reference. A reference is a raised
link number at 0.7 times the text size.

- A reference is one box in the `MathPool`, as a formula is. Its paragraph takes the
  cold formula wrap and paint; the fast paragraph path gains a test for "[^" in
  documents with footnotes only.
- Headings and table cells that refer to a note wait as Markdown until all notes are
  known (`Block::notesPending`), then become plain text with markers, as those with
  formulas are. A table with a reference therefore shows its cells without emphasis
  and links, as a table with a formula does.
- Bookmarks, anchors and titles leave the references out (`TitledHeadings`).
- The parsing of definitions and the numbering live in `footnotes.cpp`, built at
  `-Os`. Inside `markdown_parser.cpp` the same code took 20 KB and moved GCC's
  inlining in the line classifier and block parser: +1.17 % instructions over the
  watch fixtures, which have no footnotes, and +2.9 % on `baseline.md`, with
  `LTrimView`, `RTrimView`, `Block`'s destructor and vector growth out of line.
  Forcing the trim helpers inline only moved the cost (`TrimView` went out of line
  instead). The parser now keeps a callback for its line kinds, the hooks for
  headings and tables (a table waits only when a line has "[^"), and nothing else.
- A reference's label is looked for no further than the longest defined label can
  reach (`tFootnoteLabelBytes`): a document of 200,000 "[^" took 0.32 s to export
  with a look of up to 1,000 bytes per opener, and takes 0.02 s.

Measured on 2026-10-09 against the previous commit: of the 2,883 corpus PDFs, the
1,824 of documents without a footnote definition are byte-identical, also with
`--page-numbers --compress`, and the 1,059 that differ all come from documents with
one. The watch fixtures, which have no footnotes, run 0.14 % more instructions (0.09
to 0.23 %); pinned, time -0.34 % against A/A -0.58 %. The binary grew by 32 KB, 27 KB
of it code: 8.5 KB in `footnotes.cpp`, 7 KB in the renderers, 2.8 KB in the parser's
hooks and the rest GCC's inlining. Notes go after the text, as on GitHub; notes at the
foot of their page would need a reserve in `Ensure` and notes measured before their
line, in both renderers.

## Code tiles, October 2026

Each line of a code block draws its tint tile before its text. The tile reached from 3
points above the line to 2 points into the next one, so the next line's tile started 0.3
points under this line's baseline and covered its descenders and underscores: `__init__`
printed as `  init  ` on every line of a block but the last, in both renderers. Now the
first line of a block on a page keeps that tile and the others start at their own top,
3.3 points under the baseline above, deeper than Courier's underscore (1.2 points) or a
Unicode font's descenders, and still reach 2 points into the next tile, as
`RenderMathSource` already drew the source of formulas (`CodeTile`).

`CodeTile` is out of line (`RAYOMD_SHARED`). Written into `RenderCode`, or with only its
page check out of line, it moved GCC's inlining in `tiny_pdf.cpp` and outlined `"- " +
text` in `RenderBullet`: +0.32 % instructions on `baseline.md`, which has no code. Out
of line, `baseline.md` runs 0.01 % fewer instructions, and `RenderTable` takes the
shift instead, in documents with code blocks: +0.42 % on `ascii_96kb.md` and +0.78 %
on `table_96kb.md`. A cold `CodeTile` made that +1.10 %, an out-of-line `RenderCode`
brought `RenderBullet`'s back.

Measured on 2026-10-09 against the previous commit: of the 2,883 corpus PDFs, the
1,023 of documents without a code block are byte-identical and the 1,860 that differ
all have one. The watch fixtures run 0.15 % more instructions; pinned, time +0.13 %
against A/A -0.15 %. The binary is the same size.

## Syntax highlighting, October 2026

Fenced code whose info string names a language GitHub knows is drawn in the colours of
GitHub's light theme (Primer's prettylights): keywords, strings, comments, numbers and
constants, names of functions and types, tags and keys, variables, and a diff's lines on
tinted tiles. `highlight.cpp` has 29 lexers: shell and terminal sessions, PowerShell,
batch files, JavaScript, TypeScript, Python and its console, C, C++, C#, Java (also
Groovy), Kotlin, Go, Rust, Swift, PHP, Ruby, SQL, HTML, XML, CSS, SCSS, JSON, YAML,
TOML, INI, Dockerfile and diff. In 1,000 READMEs and 8 documentation sites (83,109
fenced blocks) they cover about 96 % of the blocks that name a language.

- One forward pass per block writes a class per byte, driven by a table of byte kinds.
  Words are looked up in an open-addressed table per language, built on its first use
  and hashed from the length and four bytes: a hash of every byte took an eighth of the
  lexer's time. Tokens up to 16 bytes are marked with two overlapping stores and plain
  ones not at all; a call to `memset` per token cost more than the token. The lexers
  run at 2.4 to 3.6 ns per byte (C++, Python and JavaScript sources of 240 to 820 KB) at
  `-O2`, 1.5 times as fast as at `-Os`, which would save 12 KB of the binary.
- Every rule is bounded: an unclosed string or comment ends with its line or the block,
  a regular expression is looked for once per line, the code of an interpolation is
  lexed once, at most 8 deep. 84 hostile inputs (unclosed openers of every kind) stay
  linear from 1 to 4 MB, at most 8.6 ns per byte.
- The renderers split a line into runs of one colour (`SplitCodeRuns`; spaces never
  start one) and draw them in one text object, with a fill colour before each run; a
  line of one colour is drawn as before. The standard renderer reads a line of
  printable ASCII where it is, the Unicode renderer the block decoded once, through
  views. The plain path's `WrapAsciiLiteral` and `WrapCodeLine` allocate a vector and a
  string per line, so a highlighted block costs fewer instructions than a plain one.
- `CodeLanguage` finds a name in a table made at compile time. It first scanned every
  alias, 2,000 instructions per fence, in the parser and so also with `--no-highlight`.
- The functions that draw highlighted code are out of line but not cold (`RAYOMD_SHARED`).
  As `RAYOMD_COLD`, optimized for size, and with `WrapCodeLine`, drawing took 2.8 million
  instructions besides the wrapping on the vercel documentation, more than the lexer.

Measured on 2026-10-09 against the previous commit: with `--no-highlight` all 2,883
corpus PDFs are byte-identical; with highlighting so are the 1,170 of documents without
a fence that names a language, also with `--page-numbers --compress`, and the 1,713 that
differ all have one. On the watch fixtures, documents without highlighted code run
-0.08 % and -0.01 % instructions; `ascii_96kb.md` (129 C++ blocks) +1.88 % and
`table_96kb.md` (142 Python blocks) -1.02 %. All nine: +0.25 % instructions, time
-0.04 % against A/A -0.22 %, pinned +0.47 % against A/A -0.05 %. Eight documentation
sites of 400 KB each (2 to 23 % code, the Unicode renderer): -0.13 % instructions, time
-0.43 % against A/A -0.74 %. The binary grew by 44 KB: the lexers' 28 KB of code and
11 KB of word lists, alias table and colours, and 7 KB in the renderers.

## Tables of contents, October 2026

A top-level paragraph `[TOC]` or `[[_TOC_]]`, in any case (the markers of GitLab, Azure
DevOps and Python-Markdown), becomes a table of contents, and `--toc` (`PdfOptions::toc`)
puts one first, or after the title heading, in a document without a marker. Only the
first marker draws a table, and a document that defines the label `toc` keeps `[TOC]` as
that link, as CommonMark reads it. The table lists the top-level headings to
`--toc-depth` levels (3, as in Pandoc) from the highest level it shows, without the title
heading (the first heading, when it is the document's only level-1 heading) and without
headings of footnote references alone.

- One layout pass. The renderers lay out the entries in the flow of the text: a
  "Contents" title, entries indented 1.5 em a level, the top level in bold with half an em
  above it, every line a link to its heading. A heading's page is known only once it is
  laid out, so each entry keeps where its last line ends (`ContentsLine`), and the builder
  writes the numbers and the dot leaders afterwards into one more content stream for each
  page of the table, as it does for page numbers and a theme's header and footer; LaTeX
  lays out twice instead. The entries end 2.55 em before the right margin, room for four
  digits.
- The dots stand on a grid from the left margin, half an em plus a dot apart, so those of
  all entries line up. A row is one string drawn with character spacing (`Tc`), not a
  text object a dot; the top level has none, as in LaTeX's book and report classes.
- An entry finds its heading by the address of the heading's text, which the heading mark
  keeps, and links to it through the reserved fragment `\x01N`, so no anchor is made for
  it. Links to footnotes and table entries no longer build the anchor index either: a
  document without `#anchor` links does no work for anchors.
- Every document pays for looking for a marker: one pass over the top-level blocks that
  compares a block's text length with 5 and 9 before its type and text, about 3,100
  instructions on `baseline.md` (0.1 %); the first version compared every paragraph's
  text, 5,200. `AppendContents`, which writes a page's `/Contents`, had become a loop
  over its extra streams, 33 instructions a page more; it is straight-line code again.

A table costs about what text of its length with a link on every line costs: with `--toc`,
`baseline.md` runs 18.4 % more instructions for 72 table lines (16 pages to 18), the
kubernetes documentation (400 KB, Unicode) 6.7 % for 357 (197 pages to 205). Inserting
the table's block moves every block after it, 0.35 % of that build; a slot the parser left
would avoid it, at the price of a test in the parser.

Measured on 2026-10-09 against the previous commit: of the 2,883 corpus PDFs, the 2,583
of documents without a marker are byte-identical, also with `--page-numbers --compress`,
and the 300 that differ (100 documents in three styles) all have one. The watch fixtures,
which have no marker, run 0.06 % more instructions (`baseline.md` 0.14 %, the marker
scan); time +0.47 % against A/A -0.36 %, pinned -0.06 % against A/A -0.33 %. Eight
documentation sites of 400 KB each: +0.03 % instructions, time +0.82 % against A/A
-0.35 %, pinned -0.48 % against A/A -0.09 %: code placement. The binary grew by 16 KB:
8.4 KB of code in `tiny_pdf.cpp` and 4.1 KB in `contents.cpp`.

## Books, October 2026

`--book` (`BuildBookPdf`) makes one PDF of several Markdown files: those a `SUMMARY.md`
lists as mdBook and GitBook read it (a first heading as the title unless it names the
list, mdBook's `book.toml` title before it; later headings of level 1 or 2 as parts, which
hold the list items after them; links of paragraphs as prefix and suffix chapters, in no
part; drafts skipped), a folder's Markdown files by name with `README.md` or `index.md`
first, or the files given, in their order. A file that is missing or listed twice fails
with exit 3.

- Each file is a document of its own, as Pandoc's `--file-scope` makes it: parsed alone,
  so its link definitions, footnotes (numbered from 1, after its text) and heading anchors
  are its own, and its relative images resolve from its directory but must lie in the
  book's. A file of `SUMMARY.md` that does not begin with a level-1 heading gets one of its
  title there, as mdBook prints it.
- Every file and part begins on a new page; a part's title, and the book's (not under a
  theme's cover), stand alone on a page. The bookmarks nest the files' headings under
  their parts and under the files the list nests them in, six levels at most
  (`AddOutline`). With `--toc` one table of contents lists the whole book after the title
  page; without, the first `[TOC]` of any file holds it.
- `#fragment` goes into the file whose page the link is on; `b.md` and `b.md#fragment` go
  into another file of the book (relative to the linking file, a folder to its `README.md`
  or `index.md`, a fragment the file lacks to its top); other links stay URIs. No structure
  the renderers fill learned which file it belongs to: every file begins on a new page, so
  the page of a link, a heading mark or a note mark tells its file, and `BookTargets` keeps
  a `HeadingTargets` per file over its marks and notes.
- The fonts are chosen once for the whole book as `BuildPdf` chooses them for a document
  (`ChooseFonts`, `BuildDocument`): the standard fonts when all of its text is ASCII or
  WinAnsi, else one TrueType font. A book of the eight documentation sites below (3.2 MB,
  1,762 pages) takes 0.9 % more instructions and 1.9 % more time (141 ms, eight files read)
  than their text as one document.

Single documents keep their bytes and their work: `book` is null through the builders,
and the book's state is a pointer they leave empty. As a local of the builders it grew
their stack frames by 224 bytes and moved everything they call by half a cache line. The
first version added 54 KB of code, the final one 44 KB (the file 40 KB): 25 KB in
`book.cpp` (reading SUMMARY.md, folders and files, and parsing a book's files, at `-Os` and
outside `tiny_pdf.cpp`'s inlining budget), 17 KB in `tiny_pdf.cpp` (the links of a book,
title pages, building one) and 4 KB in the command line. Constant arguments from the
title page made GCC clone `ForEachWrappedAsciiLine` and `WrapAsciiWords` (2.5 KB), so it
wraps through the vector-returning `WrapAsciiText` as other cold callers do; the parse
moved out of `tiny_pdf.cpp` took its vector and `Block` code (8 KB at `-O3`) with it.

Measured on 2026-10-09 against the previous commit: all 2,883 corpus PDFs are
byte-identical, also with `--page-numbers --compress`, `--toc` and `--pdfa`. The watch
fixtures run 0.02 % more instructions (`baseline.md` 0.14 %: `AppendUriLiteral` called,
no longer inlined, from the link annotations, now a template over the targets); time
+0.65 % against A/A -0.46 %, pinned -0.18 % against A/A -0.26 %. Eight documentation
sites: -0.02 % instructions, time +0.25 % against A/A -0.52 %, pinned +0.58 % against A/A
0.00 % (+0.62 % and +0.66 % in two runs before the frame fix, whose unpinned time was
-1.0 %). No function runs more instructions; the new code moves the hot functions into
other instruction-cache sets, which pinned alignment does not undo (see Measured
opportunities).

## SVG images, October 2026

An image whose file or URL holds SVG (sniffed: `<svg` after an XML declaration, comments and
a document type) is drawn as vectors instead of its alt text. `src/core/svg.cpp` converts it
into a form XObject whose `/Matrix` maps it onto the unit square, so a page draws it with the
`cm … /ImN Do` of a raster image, sized from its width and height as 96 dpi pixels, and
layout, theme logos and books need no change.

- Drawn: paths (all commands, arcs as at most four cubics), the six shapes, transforms,
  `viewBox` and `preserveAspectRatio`, nested `svg`, `symbol` and `use` (a viewport clips
  when its content overflows it), clip paths (children united, even-odd, `objectBoundingBox`),
  linear gradients as axial shadings, opacity as `/ca` and `/CA` graphics states (a group's
  folded into its leaves, exact where they do not overlap), dashes, caps and joins, text
  (`text`, `tspan`, anchors, bold) in the document's font, and `data:` PNG and JPEG images
  as image XObjects of the document. Styles cascade in SVG 2's order: presentation
  attributes, `<style>` rules of type, class, id and `*` selectors by specificity (what
  Illustrator writes), the `style` attribute, `!important`.
- Approximated: a radial gradient paints its average colour and a pattern the fill of its
  first rectangle; markers, filters and masks are not drawn; `font-family` gives way to the
  document's font, and italic stands upright.
- Alt text, and one more failed image: a script, HTML in a `foreignObject`, a rule whose
  selector it does not match that sets a drawn property (Mermaid's `#id .class` sheets), a
  document type with entities, malformed XML, nothing painted, and every limit: 16 MiB of
  markup, 64 levels, 500,000 elements, 8 million path numbers, `use` 8 deep, 64 MiB of
  content, and a budget of 12 times the input plus 16 MiB for the work that `use`, style
  rules and viewport measuring multiply. ASan and UBSan found nothing in 112 real and hostile
  SVGs, each exported as an ASCII, Unicode, PDF/A and theme-logo document.
- Text takes the standard documents' Helvetica in WinAnsiEncoding, where a character without
  a code shows as its base letter or `?`, and the minus sign that matplotlib writes, other
  dashes and spaces as `-` and a space (also elsewhere the transcoder replaces); in a Unicode
  document the TrueType font, whose subset gains the glyphs. A form names its document's
  fonts and images, so it is converted for each document and never enters the cache that
  PNG and JPEG images share between documents.
- No dependency: nanosvg flattens shapes for its own rasterizer and has no text; lunasvg
  (about 300 KB) and resvg (Rust, megabytes) render to pixels. SVG maps onto PDF operators
  almost one to one, and 87 KB of code draws the matplotlib, Vega-Lite, Plotly and Graphviz
  samples (Mermaid falls back).

Nine charts of those four tools (138 KB of SVG) take 1.82 ms a build (`--bench`), 76 MB/s;
a 33 KB matplotlib chart 0.53 ms, an 8 KB Graphviz graph 0.11 ms, a 2.1 MB scatter plot of
20,000 markers 69 ms. The first version took 3.12 ms: at `-Os` GCC called
`string_view::substr`, `operator==` and the byte tests out of line, and names were looked up
through `strlen`. Name tables tagged with their lengths (offsets instead of pointers, which
also took 206 relocations out of the position-independent executable), byte helpers forced
inline, an attribute scan that returns the markup itself when nothing needs decoding, a
one-pass scanner for plain decimals, and path segments appended whole took 42 % off.

Measured on 2026-10-09 against the previous commit: all 2,883 corpus PDFs are byte-identical
with no flags, `--page-numbers --compress`, `--toc`, `--pdfa` and a theme (96 of them name a
missing `charts/sales.svg` and keep its alt text). The watch fixtures run -0.01 %
instructions; time -1.50 % against A/A -1.62 %, pinned +0.31 % against A/A -0.31 %; the eight
documentation sites pinned +0.11 % against A/A -0.13 %. The executable grows by 92 KB: 87 KB
in `svg.cpp` and 6 KB in `tiny_pdf.cpp` (the two hosts, the forms, the registry). Linux and
Wine write identical bytes, and veraPDF passes PDF/A-3b files with forms, shadings,
transparency and an SVG logo. Under Wine the Windows build overflows the stack in Wine's WIC
decoding an RGBA PNG, before this change too; Windows CI decodes them, so Wine smoke tests
leave them out.

## Tagged PDF/UA, October 2026

`--pdfua` (`PdfOptions::pdfua`) writes PDF/UA-1 (ISO 14289-1), validated with veraPDF 1.30
(`verapdf -f ua1`): all 970 corpus documents, the documentation sites, `tester.md` and a book,
with page numbers, a table of contents, `--compress`, a theme with an SVG logo and a cover,
and with `--pdfa` (which then passes `-f 3b` as well). It takes the Unicode renderer and its
embedded font, as PDF/A does, and adds a structure tree (`src/core/tagged.cpp`).

- Elements: headings as H1 to H6, never deeper than one below the last (`## A` first becomes
  H1); P; L, LI, Lbl and LBody (an item's text is its body's own content, a nested list goes
  into the body, a task's box is its label with ☐ or ☑ as ActualText); BlockQuote, and Div for
  an alert; Code with block placement; Table, THead and TBody, TR, TH with a column scope and
  TD, a header row repeated on a new page an artifact; Figure with its alt text and box, an
  image without alt text an artifact; Formula with its TeX source as Alt (shown as source, as
  with PDF/A); Link around link text and linked images, with its annotation as an object
  reference; Reference and Note (with an /ID) for footnotes; TOC, TOCI and Caption.
- Content: the renderer says before each paint whether it draws content (`PaintText`,
  `DrawImage`, highlighted code) or decoration (`DrawRect`, `DrawStrokeRect`, rules, the note
  separator); the tree opens a sequence only when that changes and closes it at a page break,
  so a paragraph is one sequence a page. Bold text is drawn once, filled and stroked (`2 Tr`),
  instead of twice: a screen reader read the doubled text twice. The streams of headers,
  footers, page numbers, a cover and the table of contents' dots and numbers are artifacts.
- The rest of PDF/UA: `/StructParents` and `/Tabs /S` on each page, `/StructParent` and
  `/Contents` (the link's text) on each link annotation, `/MarkInfo`, `DisplayDocTitle`, the
  front matter's language (else `en`) and a title (the front matter's, else the first
  heading, else the file name), and XMP with `pdfuaid:part` 1, declared to PDF/A by an
  extension schema when both are claimed. With `--embed-source` the file stays PDF 1.7.

Untagged documents keep their bytes (all 2,883 corpus PDFs, also with `--page-numbers
--compress`, `--toc`, `--pdfa`, `--pdfa --embed-source` and a theme). The tests of `tags` in
the hooks cost the watch fixtures 0.09 % more instructions (0.06 % to 0.22 % a build by
callgrind) and the documentation sites 0.15 %; pinned time +0.04 % against A/A -0.01 %, sites
-0.12 % against -0.27 %. GCC outlined `"- " + text` in the standard renderer's bullets again
(+0.3 % on `baseline.md`); it is spelled out now. The executable grows by 33 KB: 19 KB in
`tagged.cpp`, the rest the hooks and the tagged annotation writer. Tagging
first took 5 to 9 times as long as PDF/A and 1 s for the fastapi documentation: the builder
counted the tree's objects once per object, a quadratic loop, and writing an element cost 4,300
instructions (a string per object, `std::to_chars` at `-Os`, three strings and a vector in each
element). Elements without heap memory, kids as a linked list in one vector, attributes in one
pool, the tree written once into a buffer the objects view, digits by multiplication, ASCII
text strings as literals, and a list item without a P of its own left these, against `--pdfa`:

| Document | `--pdfa` | `--pdfua` | Size |
| --- | ---: | ---: | ---: |
| `baseline.md` | 0.44 ms | 0.76 ms | 202 → 343 KB |
| `ascii_96kb.md` | 2.11 ms | 3.22 ms | 923 → 1,502 KB |
| `table_96kb.md` | 2.20 ms | 4.59 ms | 1,112 → 1,810 KB |
| `unicode_96kb.md` | 1.96 ms | 3.64 ms | 930 → 1,527 KB |
| kubernetes documentation | 9.24 ms | 11.16 ms | 2,748 → 3,639 KB |
| fastapi documentation | 22.01 ms | 37.67 ms | 6,425 → 10,823 KB |

The rest grows with the structure itself: an object for every element, a sequence for each run
of content, the ParentTree's reference for each, and the links' text in their annotations (see
Measured opportunities).

## Cold exports, October 2026

A one-shot `rayomd --export` spends most of its time outside the document: in 3.2.0 a short
ASCII file took 1.63 ms, of which 1.47 ms was the process starting, and a 1024x1024 RGBA PNG
took 87 ms, 99.7 % of it in re-encoding the image. Measured on 2026-10-12 with g++ 13.3 on
Linux (Docker, files on tmpfs), fresh processes pinned to one CPU, medians of 40 runs, and
whole-process instructions by callgrind; 3.2.0 (`7056be1`) against the last change below:

| Export | 3.2.0 | Now | Change | Instructions |
| --- | ---: | ---: | ---: | ---: |
| one 1024x1024 RGBA PNG | 87.25 ms | 38.94 ms | -55.4 % | -71.1 % |
| `tester.md` | 89.15 ms | 39.84 ms | -55.3 % | -70.8 % |
| short ASCII document | 1.63 ms | 0.98 ms | -39.9 % | -86.7 % |
| short Unicode document | 2.53 ms | 1.36 ms | -46.4 % | -60.1 % |
| `single_01.md` | 6.22 ms | 4.87 ms | -21.7 % | -9.6 % |
| short Chinese document, WenQuanYi fallback | 7.60 ms | 2.82 ms | -62.9 % | -50.7 % |
| `single_01.md`, WenQuanYi installed | 13.12 ms | 7.91 ms | -39.7 % | -14.4 % |

Warm `--bench` builds are 5.17 % faster (geometric mean of the watch fixtures, pinned, nine
rounds; A/A -0.32 %), all from the static runtime. Every PDF is byte-identical: 334 corpus
PDFs (the repository's Markdown and the watch fixtures in three styles and with `--compress`,
`--pdfa`, `--pdfua`, `--toc`, `--page-numbers` and `--embed-source`, and 629 generated PNGs
in every colour type, bit depth, input filter, interlacing and row-width remainder). The
Linux executable grows from 1,034,800 to 1,215,464 bytes, the Windows one by 7,168 bytes.

One change at a time, each against the one before:

| Change | Commit | Effect |
| --- | --- | --- |
| PNG filters chosen in one pass | `177f5e6` | image 87.4 → 61.9 ms. The five filters were scored in five passes with a switch at every byte; one branch-free pass adds up all five. |
| Pixels split through pointers | `5b65f09` | image -16 %: `push_back` per byte was not inlined. `ReadU32` is `RAYOMD_HOT_INLINE` since: the edit made GCC stop inlining it into `ParseLoca` (+56,000 instructions per Unicode export). |
| One switch per row when unfiltering | `db8877b` | image -3 %. |
| SSE2 filter scoring and filtering | `ca2094f` | image -18 %: `min(r, 256 - r)` is `pminub` of r and -r, summed by `psadbw`; floor((a + b) / 2) is `pavgb` less the odd bit; Paeth on 16-bit lanes. |
| SSE2 unfiltering | `1408697` | image -5 %: Up sixteen bytes at a time, Sub, Average and Paeth one 3- or 4-byte pixel at a time. |
| Fonts mapped, not read | `c1b5a58` | short Unicode -18 %, Chinese fallback -54 %: a fallback candidate was read whole (5 MB for WenQuanYi) to learn its coverage. |
| C stdio instead of iostreams | `af966d7` | short ASCII -2.6 % instructions; no stream or locale symbol left to link. |
| Static libstdc++ and libgcc, `--gc-sections` | `7265c5c` | short ASCII -41 %, warm -5.2 %: 92 % of a short export's instructions were the dynamic linker relocating `libstdc++.so`. |

Static linking costs 181 KB here because nothing uses iostreams any more: a 40-line program
using `std::filesystem` and one `std::cerr` links 769 KB of libstdc++ with `--gc-sections`
and 306 KB without the `std::cerr`; without `--gc-sections` `std::filesystem` alone pulls in
1.4 MB of locale code. The PNG code was checked against its scalar reference by core check 99
(rows of 1 to 4 bytes per pixel, every width to 80, ties and saturation), by a build without
SSE2, and with 164 hostile PNGs (bad filters, short and long rows, huge sizes, missing or
short palettes and tRNS, random byte flips) under AddressSanitizer and UBSan.

## 4.0, October 2026

A second pass over what the profiles of the cold-export work still showed: zlib's checksum and
whole-image buffers in PNG re-encoding, number and hex formatting, per-word strings in formula
text, character widths, and font allocation. Measured on 2026-10-12 like the section above
(g++ 13.3, Linux in Docker, files on tmpfs, CPU 3); 3.2.0 (`7056be1`) against 4.0:

| Export | 3.2.0 | 4.0 | Change | Instructions |
| --- | ---: | ---: | ---: | ---: |
| one 1024x1024 RGBA PNG | 87.56 ms | 27.26 ms | -68.9 % | -75.7 % |
| `tester.md` | 89.31 ms | 28.13 ms | -68.5 % | -75.4 % |
| short ASCII document | 1.69 ms | 1.04 ms | -38.2 % | -86.6 % |
| short Unicode document | 2.53 ms | 1.31 ms | -48.2 % | -66.6 % |
| `single_01.md` | 6.27 ms | 4.14 ms | -34.0 % | -36.4 % |
| short Chinese document, WenQuanYi fallback | 7.59 ms | 2.67 ms | -64.9 % | |
| `single_01.md`, WenQuanYi installed | 13.09 ms | 6.89 ms | -47.4 % | |

Warm `--bench` builds run 16.1 % faster (geometric mean of the watch fixtures, pinned, nine
rounds; A/A -0.07 %): the formula fixtures 21.6 to 25.2 %, `unicode_96kb.md` 12.9 %, the
ASCII ones 7.4 to 8.6 %. Every change kept the bytes: all 3,353 corpus PDFs (319 Markdown
documents, 300 of them generated with formulas, tables, lists, links, footnotes, code and
Unicode, and 629 PNGs, in three styles and with `--compress`, `--pdfa`, `--pdfua`, `--toc`,
`--page-numbers`, `--embed-source` and letter pages) are byte for byte those of the commit
before the first change, and the fixtures those of 3.2.0.

| Change | Commit | Effect |
| --- | --- | --- |
| zlib streams wrapped by RayoMD | `966527f` | image -4 %: zlib deflates and inflates raw data, RayoMD writes and checks the header and the Adler-32 with the SSE2 checksum of `flate.cpp` (zlib's was 11 % of an image export). |
| PNGs re-encoded a row at a time | `22214ca` | image -26 %: no whole-image buffer is zeroed or kept but the alpha plane; zlib's deflate output does not depend on how its input arrives. |
| PDF numbers two digits at a time | `64c890e` | warm -2.2 %, tables -4.2 %: `WriteFixed2` was 5 to 14.5 % of a warm build. |
| Formula text without a string per word | `a3ace2e` | formula fixtures warm -11.6 to -15.5 %. |
| Fonts allocated zeroed | `2446541` | cold Unicode -4.6 %, `single_01.md` -7.5 %: each of up to 11 fallback candidates zeroed a 128 KB width cache. |
| A character's width in one lookup | `cbc4830` | warm Unicode -3.0 to -3.9 %: the width cache is keyed by code point too. |
| CID hex from a pair table | `325f3aa` | warm Unicode -1.8 to -3.2 %. |
| A fully static Linux edition | `b606f17` | a second Linux asset: short ASCII 1.05 to 0.68 ms (-35 %), short Unicode -22.5 %, `single_01.md` -9.3 %, images -2.5 %, for 2,165,888 bytes against 1,215,464. |

Checks: core check 99 and the `--compress` round trips (the shared Adler-32); 29.6 million
values of `WriteFixed2` against the previous implementation (every hundredth to 12,000 and its
rounding neighbours, signs, the 2^52 boundary); 217 altered zlib streams and 255 hostile PNGs
give the PDFs of the commit before, under AddressSanitizer and UBSan, which also ran the tests
and the corpus; a build without SSE2 writes the same corpus; 22 CLI cases behave the same.

Measured and not taken (see Measured opportunities): exact cell reservation in table rows,
an SSE2 scan of PDF literal strings, SSE2 runs of ASCII in UTF-8 decoding, and a static build
against musl.

## Image re-encoding, October 2026

A PNG with transparency is the slowest thing RayoMD exports: 4.0 took 27 ms for the 1024x1024
RGBA mascot, all but 1 ms of it decoding and re-encoding the image, 63 % of that in zlib. Every
stage was measured against the alternatives, in process (`DecodeImageBytes` on the PNG's bytes,
the image cache bypassed, 1,000 iterations, p50 and p95) and as cold `--export` processes
(medians of 60), on 2026-10-13 with g++ 13.3 in Docker, Ryzen 5 PRO 4650GE, one pinned CPU.
Seven RGBA images: the mascot, the README's screenshot, and five generated with different
entropy and transparency (a noisy photo with an alpha ramp, soft shadows, sparse sprites, an
opaque photo stored as RGBA, a 2048x2048 flat illustration with anti-aliased edges).

The image is now inflated whole by libdeflate's decompressor (vendored, `third_party/libdeflate`)
instead of zlib, its colour split from alpha sixteen pixels at a time in SSE2, and both planes
compressed by RayoMD's own encoder in an image mode (`ZlibCompressImage`) instead of zlib's
level 1. Every re-encoded stream changes and every test image shrinks; no pixel changes.

| Image | 4.0 p50 / p95 | Now p50 / p95 | Speed-up | Streams 4.0 → now |
| --- | ---: | ---: | ---: | ---: |
| mascot, 1024x1024 | 23.72 / 24.00 ms | 10.10 / 10.27 ms | 2.35x | 301.6 → 238.6 KiB |
| screenshot, 1086x753 | 8.24 / 8.33 ms | 4.77 / 4.95 ms | 1.73x | 69.9 → 61.6 KiB |
| soft shadows, 1024x1024 | 18.88 / 19.01 ms | 10.19 / 10.38 ms | 1.85x | 218.6 → 159.2 KiB |
| sparse sprites, 1024x1024 | 16.52 / 16.62 ms | 6.72 / 6.93 ms | 2.46x | 30.7 → 15.6 KiB |
| opaque photo as RGBA, 1024x1024 | 73.63 / 75.37 ms | 17.82 / 18.03 ms | 4.13x | 1999.3 → 1896.2 KiB |
| noisy photo, 1024x1024 | 93.51 / 94.06 ms | 21.55 / 21.74 ms | 4.34x | 2822.8 → 2798.4 KiB |
| illustration, 2048x2048 | 58.70 / 59.05 ms | 29.17 / 29.37 ms | 2.01x | 252.3 → 143.0 KiB |

Cold exports (files on the host's `/tmp`): the mascot alone 27.30 → 15.35 ms (-43.8 %),
`tester.md` without its remote and missing images 28.17 → 16.26 ms (-42.3 %), the noisy photo
106.96 → 36.27 ms, a short ASCII document 0.90 → 0.84 ms, as the executable no longer loads
`libz.so`. Inflating whole raises an image's peak heap by 2.4 to 11.3 MiB (the mascot 6.1 → 8.5
MiB): the filtered colour rows are written back into the inflated rows' buffer, behind the row
being read, so no third plane is allocated. The Linux executable grows from 1,215,464 to
1,227,776 bytes and needs only libc and libm; the Windows one grows by 19,968 bytes, and its
transparent PNGs no longer go through WIC (decoded, re-encoded as PNGs, parsed again), which
was not timed here.

Stage by stage, mascot, 4.0 → now: inflate 7.5 → 1.3 ms, unfiltering and splitting 2.8 → 1.5,
filter choice and filtering 2.7 → 2.6 (the same SSE2 code), compression 12.0 → 4.9 (colour 4.2,
alpha 0.7). The 4.0 stages are those of the same pipeline staged, its streams byte for byte.

Measured and not taken:

| Alternative | Why it lost |
| --- | --- |
| zlib at levels 3, 6, 9 | up to 18 times the time of level 1 (mascot 32, 72 and 443 ms) for streams 0 to 61 % smaller; level 3 writes larger streams than RayoMD's encoder on all seven images. |
| zlib-ng 2.3.3, level 1 | compression 2.9 times as fast as zlib's, but streams 16 to 72 % larger than zlib's level 1 (mascot 412 KiB). Level 3 is within -6 to +14 % of RayoMD's encoder in size at 1.7 to 8 times its time. |
| libdeflate 1.26's compressor, levels 1 to 12 | level 1 is barely faster than zlib's (mascot 11.3 against 12.0 ms) for streams -4 to +12 % of RayoMD's; level 12 takes 0.2 to 5 s. |
| ISA-L 2.31 (igzip), levels 0 to 3 | the fastest compressor (mascot 2.5 ms against 4.9), but streams 0 to 33 % larger than RayoMD's, and it is NASM assembly, not a small dependency to vendor. |
| zlib-ng and ISA-L inflate | 2.1 and 2.3 ms on the mascot against libdeflate's 1.75. |
| Wuffs v0.4 PNG decoder | 6.2 ms from PNG to colour and alpha planes on the mascot, against 4.6 for libdeflate with RayoMD's unfiltering and split (both splitting a byte at a time). |
| fixed filters None, Sub, Up or Paeth | 6 to 20 % less time than choosing per row, but streams up to 2.7 times as large (soft shadows with None); the best, Up, is still 31 % larger on the sparse sprites. |
| uncompressed streams | the mascot in 10.7 ms, as a 4 MiB PDF. |
| SSSE3 or AVX2 RGBA split | 0.43 and 0.46 ms per megapixel against 0.45 for SSE2, which needs no CPU dispatch. |
| the 4-byte table in image mode | `ZlibCompress` as it is for page text: 6.4 times the time on the opaque photo; streams as large on four images, larger on two, smaller only on the screenshot (61 against 62 KiB). |

Checks: `verify_cli.py` decodes every image and soft mask of 60 generated PNGs (RGBA, grey and
alpha, palette and tRNS, RGB and grey with a transparent colour, opaque RGBA; every row filter;
widths 1 to 130 around the SSE2 steps) with Python's zlib and a textbook unfilter and compares
every pixel, and fails on a build with two SSE2 loads swapped. Core check `CheckCompression`
round-trips both encoder modes through libdeflate and an Adler-32 of its own. 1,279 hostile
PNGs (flipped and truncated deflate data, bad checksums and headers, wrong sizes, bad filters
and palette indices) fail or decode exactly as with 4.0, under AddressSanitizer and UBSan.
Every PDF without such a PNG is byte-identical (108 corpus PDFs in three styles, with
`--compress`, `--page-numbers` and `--toc`), and warm builds of text are within 0.01 % of their
instructions.

**A persistent image cache.** A process keeps a decoded image for its later documents; now a
re-encoded PNG of 32,768 pixels or more is also kept on disk for later processes
(`PdfOptions::imageCacheDirectory`; the CLI and the app use the user cache directory, `--bench`
and `--no-image-cache` do not). An entry is named by the SHA-256 of the PNG's bytes, so an
edited file is a new image and a copy under another name is the same one; it holds the streams
and what is needed to check them, never PDF syntax. Cold exports with SHA-NI, 60 runs, one
pinned CPU, p50 / p95:

| Document | Cache off | Miss | Hit |
| --- | ---: | ---: | ---: |
| the mascot alone | 15.26 / 15.36 ms | 15.82 / 15.93 ms | 2.19 / 2.23 ms |
| `tester.md`, local images | 16.10 / 16.19 ms | 16.58 / 16.83 ms | 3.04 / 3.12 ms |
| screenshot | 8.69 / 8.76 ms | 8.94 / 9.33 ms | 1.20 / 1.28 ms |
| illustration, 2048x2048 | 41.16 / 41.38 ms | 41.54 / 41.77 ms | 1.66 / 1.71 ms |
| noisy photo, a 3.4 MB PNG | 35.99 / 36.85 ms | 41.27 / 41.68 ms | 14.75 / 14.83 ms |
| 182x182 RGBA | 2.12 ms | 2.28 ms | 0.96 ms |

The PDF is byte for byte the same in all three. A miss costs the hash and writing the entry: 0.2
to 0.6 ms, 5.3 ms for the noisy photo's 2.8 MB entry. A hit still reads and hashes the PNG and
writes the PDF. SHA-256 is 7.5 times as fast with the x86 SHA extensions (2,095 MB/s against
278 MB/s for the portable code, which serves CPUs without them); with the portable code alone
the noisy photo's miss cost 15 ms and its hit 24.8 ms. Below 32,768 pixels an image re-encodes
in under 0.7 ms (128x128 in 0.26 ms), about what finding, reading and checking its file takes.
Taken over the other keys: the path with its size and time, which misses an edit that keeps
both, and a non-cryptographic hash, whose collisions can be made to order. The cache lives in
`image_cache.cpp`: inside `tiny_pdf.cpp` it took inline budget from `AppendInt`, whose
`std::to_chars` became a call (+0.15 % on a warm `tester.md`); outside it, warm text builds
are within 0.03 % of 4.0. The Linux executable grows by 16,384 bytes, to 1,244,160.

Checks: `verify_cli.py` keeps the cache in its own directory and checks that a second export
reads the entry without writing it, that a flipped or truncated entry is rewritten, and that the
PDF never changes (it fails on a build that never reads the cache); 400 damaged entries, among
them colour and mask lengths shifted with their sum kept, give the PDF of an export without the
cache under AddressSanitizer and UBSan; the core tests compare the SHA extensions with the
portable SHA-256 on FIPS 180-4's vectors and every length to 300 bytes and up to 1.2 MB.

**Alpha on a second thread.** A translucent PNG of 262,144 pixels or more has its alpha rows
filtered and compressed on a thread of their own while the colour is compressed, when the
process may run on more than one CPU (its affinity mask: `std::thread::hardware_concurrency`
counts the machine's CPUs, and a thread on a process pinned to one CPU cost 1.5 %). The bytes
are the same: each stream is compressed whole, as before. In process, two CPUs, p50: soft
shadows 10.18 → 8.32 ms (-18.3 %), the illustration 29.20 → 25.14 (-13.9 %), the mascot 10.09 →
8.78 (-13.0 %), the sprites -9.7 %, the noisy photo -4.3 %; opaque images start no thread.
Cold, two CPUs: the mascot 15.19 → 13.85 ms (-8.8 %), `tester.md` -8.4 %, soft shadows -12.4 %,
the illustration -9.6 %; one CPU within ±0.8 %. 48 documents with a distinct copy of the mascot
each, `--batch --no-image-cache` on 12 CPUs: -7.8 % with the default workers, +0.6 % with
`--workers=12`, which leave no CPU free. ThreadSanitizer finds nothing in single exports and in
a four-worker batch of these documents. Below 262,144 pixels the alpha takes under 0.4 ms (the
mascot's is 1.5 ms per megapixel), and a thread with fresh encoder tables cost 0.15 ms where it
could not run beside the colour.

**Huge pages for decoded images.** In a fresh process every 4 KB page of the whole-image
buffers faults on first touch: 1,869 faults for the mascot, against 887 when 4.0 decoded a row
at a time, 5,572 for the illustration. On Linux those buffers (the inflated rows, a palette's
colour rows, the alpha rows) are now 2 MB-aligned and advised `MADV_HUGEPAGE`, which transparent
huge pages in the common `madvise` mode honour. Cold exports, one pinned CPU, p50:

| Document | Before | Now | Faults | Peak RSS |
| --- | ---: | ---: | ---: | ---: |
| the mascot | 15.26 ms | 13.68 ms (-10.4 %) | 1,869 → 849 | 9.5 → 11.4 MB |
| soft shadows | 15.13 ms | 13.58 ms (-10.2 %) | 1,769 → 747 | 9.2 → 11.1 MB |
| screenshot | 8.72 ms | 7.31 ms (-16.2 %) | 1,332 → 538 | 7.6 → 8.3 MB |
| illustration, 2048x2048 | 41.35 ms | 32.59 ms (-21.2 %) | 5,572 → 462 | 24.4 → 28.7 MB |
| noisy photo | 35.49 ms | 33.90 ms (-4.5 %) | 5,310 → 4,289 | 20.6 → 22.8 MB |

The compressor's output buffer stays on ordinary pages: it is sized for the worst case and
mostly written in part, and on huge pages it took the mascot 1.7 MB more memory and 1 % more
time (only the noisy photo, whose 2.8 MB stream fills it, gained 3 %). glibc's own switch,
`GLIBC_TUNABLES=glibc.malloc.hugetlb=1`, gained less (the mascot -5.3 %, the illustration
-16.4 %), most likely as its large chunks are not 2 MB-aligned and so cover fewer whole huge
pages. Elsewhere, and on Windows, the buffers are plain `malloc`.

Only the first 12 such buffers of a process, about four images, take huge pages: later ones get
memory the allocator has already mapped, and on huge pages 48 images in one `--batch` (12 CPUs,
`--no-image-cache`) ran 3.5 to 5.7 % slower (default workers 117.7 → 123.9 ms, `--workers=12`
112.9 → 119.3, `--workers=1` 472.4 → 488.9). With the budget: 119.5, 113.9 and 472.8 ms, and
the cold exports above unchanged (the mascot 13.65 ms, the illustration 32.54).

**Colour compressed as it is decoded.** The encoder's image mode can start before its input is
complete (`InputProgress`): before each 64 KB block it waits until the block and the longest
match past it, 266 bytes, are written. From 262,144 pixels, on a process that may use another
CPU, a second thread now compresses the colour rows while this one unfilters, splits and
filters the next, then filters and compresses the alpha. That replaces the alpha thread above,
which overlapped only the alpha. The bytes are the same: each stream is still compressed whole,
and a core test compresses input written by another thread, pieces of up to 70,000 bytes at a
time, to the bytes of the whole input, and stops on input abandoned half way. In process, two
CPUs, p50, against the alpha thread:

| Image | Alpha thread | Now |
| --- | ---: | ---: |
| the mascot | 8.71 ms | 5.99 ms (-31 %) |
| screenshot | 4.74 ms | 3.59 ms (-24 %) |
| opaque photo as RGBA | 17.90 ms | 14.71 ms (-18 %) |
| noisy photo | 20.64 ms | 16.99 ms (-18 %) |
| sparse sprites | 6.32 ms | 5.74 ms (-9 %) |
| soft shadows | 8.32 ms | 7.71 ms (-7 %) |
| illustration, 2048x2048 | 25.02 ms | 25.12 ms |

The illustration's decoding (10.7 ms of unfiltering, 6.5 of filter choice) is longer than its
colour's compression, and its alpha (4.3 ms) now follows the decoding on this thread instead of
running beside it, so it ends where it did. Cold, two CPUs: the mascot 12.18 → 9.19 ms (-24.5 %),
`tester.md` 13.05 → 10.09 (-22.7 %), the screenshot -19.6 %, the noisy photo -11.3 %, soft
shadows -6.3 %, the illustration -3.1 %; one CPU within 1 %. 48 such documents in one `--batch`:
-9.4 % with the default workers, -0.7 % with `--workers=12`, 458 → 325 ms with `--workers=1`.
ThreadSanitizer finds nothing in the core tests, single exports and a four-worker batch.

**AVX2 filter choice.** With decoding now the longer path, choosing and applying the row filters
was its largest kernel left: 1.8 ms on the mascot's colour, 0.6 on its alpha. Both now run 32
bytes a step in AVX2 where the CPU has it, compiled with GCC's `target("avx2")` and chosen once by
`__builtin_cpu_supports`, so the build stays baseline x86-64; the SSE2 code serves the other CPUs
and MSVC. On the mascot's rows: colour 1.789 → 0.868 ms, alpha 0.581 → 0.345 ms, the same filters
and bytes. Keeping the four candidate residual rows while scoring and copying the best, instead
of filtering again, was slower (0.934 and 0.377 ms): the stores cost more than the second pass.
In process, p50, one CPU: the illustration 29.82 → 24.52 ms (-17.8 %), the mascot 9.91 → 8.67
(-12.5 %), the sprites -14.6 %, the screenshot -14.4 %; two CPUs: the illustration 25.15 →
19.74 (-21.5 %), soft shadows -17.0 %, the mascot 6.00 → 5.65 (its colour's compression on the
other thread is now the longer path). Cold, one CPU: the mascot 13.49 → 12.27 ms (-9.0 %),
`tester.md` 14.44 → 13.27, the illustration -14.2 %; two CPUs: the mascot 9.25 → 8.76, the
illustration 27.57 → 23.06. The core test checks the dispatched and the SSE2 code against the
scalar reference and fails on a build with Paeth's blend swapped.

**Where it ended.** From PNG to finished PDF, cold `--export`, p50, 4.0.0 against `9db72df`
(2026-10-13, one or two pinned CPUs, 43 runs). A new image is converted; a kept one comes from
the persistent cache, as a logo or a diagram does from its second export on:

| Document | 4.0.0 | New image, 1 CPU | New image, 2 CPUs | Kept image, 1 CPU |
| --- | ---: | ---: | ---: | ---: |
| the mascot alone | 27.31 ms | 12.32 ms (2.2x) | 8.70 ms (3.1x) | 2.18 ms (12.5x) |
| `tester.md`, local images | 28.08 ms | 13.20 ms (2.1x) | 9.53 ms (2.9x) | 3.02 ms (9.3x) |
| screenshot | 10.37 ms | 6.54 ms (1.6x) | 5.31 ms (2.0x) | 1.23 ms (8.4x) |
| soft shadows | 22.06 ms | 12.15 ms (1.8x) | 9.55 ms (2.3x) | 1.81 ms (12.2x) |
| illustration, 2048x2048 | 63.39 ms | 27.84 ms (2.3x) | 23.12 ms (2.7x) | 1.65 ms (38x) |
| noisy photo | 106.20 ms | 32.72 ms (3.2x) | 29.11 ms (3.6x) | 14.18 ms (7.5x) |
| short ASCII, no image | 0.90 ms | 0.85 ms | 0.75 ms | |

In process, the image alone (`DecodeImageBytes`, 1,000 runs, p50 / p95), one CPU: the mascot
23.72 / 24.00 → 8.66 / 8.80 ms, the illustration 58.70 → 24.47, the noisy photo 93.51 → 20.34;
two CPUs: 5.66 / 5.85, 19.70 and 17.01 ms. Allocations per image 13 to 23 → 8 to 13; peak heap
the mascot 6.1 → 8.5 MiB, the illustration 21.0 → 36.3 MiB. Every image decodes back to its
source pixels. The mascot's stages, one CPU, 4.0.0 → now: inflate 7.5 → 1.3 ms, unfiltering and
splitting 2.8 → 1.5, filter choice and filtering 2.7 → 1.2, compression 12.0 → 4.9.

**Memory.** A new image costs more memory than in 4.0.0, which inflated, unfiltered and
compressed a row at a time and kept only the alpha plane whole. Now the inflated rows are kept
whole (4 bytes a pixel plus one a row for RGBA; the filtered colour rows go back into them),
beside the alpha rows (1 byte a pixel); the first image buffers of a process are rounded up to
2 MB pages on Linux; and on two CPUs the colour thread has encoder tables of its own. Peak RSS
of a cold export, one CPU, 4.0.0 → new image → image kept (2026-10-13, `/usr/bin/time`):

| Export | 4.0.0 | New image | Image kept |
| --- | ---: | ---: | ---: |
| short ASCII document | 2,992 KiB | 2,832 KiB | 2,916 KiB |
| the mascot alone | 5,472 KiB | 11,484 KiB | 3,808 KiB |
| `tester.md`, local images | 6,064 KiB | 12,132 KiB | 4,540 KiB |
| screenshot | 4,220 KiB | 8,404 KiB | 3,128 KiB |
| illustration, 2048x2048 | 8,356 KiB | 28,592 KiB | 3,388 KiB |
| noisy photo | 16,784 KiB | 22,752 KiB | 15,064 KiB |
| 48 mascot documents, `--batch`, six workers (12 CPUs) | 36,168 KiB | 61,416 KiB | |
| the same, `--workers=1` | 20,888 KiB | 22,196 KiB | |

Two CPUs add up to 776 KiB (`tester.md`; the illustration 236 KiB). At the size limit,
`kMaxDecodedImageBytes`, an RGBA image of about 24 million pixels has 96 MiB of inflated rows
and 24 MiB of alpha rows where 4.0.0 kept the 24 MiB alpha plane alone. A kept image needs no
decode buffer, so it takes less memory than 4.0.0 did.

Every pipeline measured on the mascot, in process, one CPU unless noted, fastest first:

| Pipeline | p50 | Streams | Status |
| --- | ---: | ---: | --- |
| kept image (cold, whole export) | 2.18 ms | 238.6 KiB | kept: the cache |
| libdeflate inflate, RayoMD image mode, SSE2 split, AVX2 filters, colour on a second thread (2 CPUs) | 5.66 ms | 238.6 KiB | kept |
| the same, one CPU | 8.66 ms | 238.6 KiB | kept |
| libdeflate inflate, ISA-L level 1, alpha on a second thread (1 CPU, scalar split) | 10.1 ms | 277.1 KiB | rejected: larger, NASM dependency |
| libdeflate inflate, RayoMD image mode, SSE2 split (13537ab) | 10.10 ms | 238.6 KiB | superseded |
| uncompressed streams | 10.72 ms | 4096 KiB | rejected: 17 times the size |
| libdeflate inflate, RayoMD text mode | 12.8 ms | 246.4 KiB | superseded by the image mode |
| zlib inflate, ISA-L levels 0 to 2 | 15.2 to 15.5 ms | 277 to 325 KiB | rejected |
| zlib inflate, zlib-ng level 1 | 16.98 ms | 412.4 KiB | rejected: 73 % larger |
| zlib inflate, RayoMD text mode | 18.42 ms | 246.4 KiB | superseded |
| Wuffs decoder, zlib level 1 | 20.77 ms | 301.6 KiB | rejected |
| zlib inflate, zlib-ng level 3 | 21.41 ms | 257.3 KiB | rejected |
| 4.0.0: zlib inflate, zlib level 1 | 23.72 ms | 301.6 KiB | baseline |
| zlib inflate, libdeflate level 1 | 24.16 ms | 260.3 KiB | rejected |
| zlib inflate, ISA-L level 3 | 26.76 ms | 277.4 KiB | rejected |
| zlib inflate, zlib level 3 | 32.11 ms | 286.1 KiB | rejected |
| zlib inflate, libdeflate levels 3 to 12 | 34.1 ms to 0.91 s | 213 to 251 KiB | rejected |
| zlib inflate, zlib-ng level 6 | 38.40 ms | 244.3 KiB | rejected |
| zlib inflate, zlib level 6 | 72.41 ms | 245.2 KiB | rejected |
| zlib inflate, zlib-ng and zlib level 9 | 230 and 443 ms | 234 KiB | rejected |

Fixed filters, SSSE3 and AVX2 splits, the 4-byte table in image mode, fused filter scoring, huge
pages for the compressor's output and for every buffer of a long process are in the paragraphs
above, with why each lost.

**Segments compressed by helper threads** (2026-10-13, after 4.1.0). Each plane's filtered rows
are now compressed in segments that any thread can take (`ImagePlanesEncoder`,
`DeflateImageSegment`): 1 MiB while more than 2 MiB are left, then halves down to 128 KiB so that
the last one is short, each with 2 KiB of history before it and ending on a byte with an empty
stored block. The stream is the same however many threads compress it, and a core test checks
it against one thread for 0, 1 and 3 helpers. A process-wide pool of up to three helpers takes a
segment once its first block is published and follows the decoder block by block; with no
helper the decoding thread compresses each segment as soon as its rows are complete, while they
are still in its cache. A helper is asked for each usable CPU that no other decode of the
process holds, so a batch whose workers keep the CPUs busy asks for none. On the test planes the
segments cost 1.7 % of the encoder's time and 0.02 % of the size of one segment a plane (8 KiB of
history: 4.7 % and 0.01 %; none: 0.9 % and 0.03 %); the screenshot's small alpha plane grows
2 %. Cold, p50 of 40, against 4.1.0:

| Document | 1 CPU | 2 CPUs | 4 CPUs |
| --- | ---: | ---: | ---: |
| the mascot | 12.25 → 12.27 ms | 8.70 → 8.47 (-2.6 %) | 8.66 → 8.10 (-6.5 %) |
| `tester.md`, local images | 13.14 → 13.18 | 9.51 → 9.36 (-1.7 %) | 9.49 → 8.97 (-5.5 %) |
| screenshot | 6.45 → 6.21 (-3.8 %) | 5.28 → 5.18 (-1.8 %) | 5.22 → 5.14 (-1.5 %) |
| noisy photo | 33.27 → 33.72 (+1.4 %) | 29.58 → 28.16 (-4.8 %) | 29.64 → 26.88 (-9.3 %) |
| illustration, 2048x2048 | 27.71 → 27.82 | 22.99 → 21.09 (-8.2 %) | 22.98 → 21.13 (-8.1 %) |

`--batch` of 48 mascot documents and of 21 mixed ones: within 4 % of 4.1.0 either way, run to run.
Peak RSS grows 0.2 to 0.5 MiB on one CPU (the segments' output buffer) and 1.2 to 1.5 MiB on
four (three helpers' encoder tables). Three designs lost on the way. Filtering in the helpers
too, from whole decoded planes, added a pass over the rows and a 1 MiB buffer per thread to
fault in: the mascot +3.4 % in process on one CPU. Taking a segment only once all its rows were
there left the helpers idle: the mascot's second 1 MiB arrived two thirds into decoding and took
2.3 ms to compress, so four CPUs gained nothing. Helpers for every CPU in a batch made it 1 to
4 % slower. ThreadSanitizer finds nothing in the core tests, single exports and two batches on
four CPUs; under ASan and UBSan the 1,279 hostile PNGs decode as before. The pool, its condition
variables and the segment code add 16,384 bytes to a local Linux release build (1,272,832 →
1,289,216).

**RayoMD's own inflater, and no plane held whole** (2026-10-13). A new image no longer inflates
into a buffer of its whole size. `ZlibReader` (`src/core/inflate.cpp`) hands out a row at a time
from a window of the last 32 KB and a chunk of at least 256 KB, and the decoder filters each
plane's row straight into the buffer of the encoder segment it belongs to. A segment's buffer is
reused once the segment is compressed, and while as many buffers as planes plus helpers plus one
are full, the decoder compresses one itself or waits for a helper. The first four encoders of a
process carve their buffers from 2 MB pages: on the mascot that cut a fresh process's page faults
from 1,357 to 719 (4.1.0: 838).

The inflater: two-level decode tables (11 and 8 root bits), a fast loop that looks up the next
code before its refill, so that the refill is off the chain of lookups, up to three literals a
refill, lengths and distances that take their code and extra bits at once, 16-byte SSE2 copies.
It accepts exactly what zlib's `uncompress()` accepts for an output of known size: against zlib
1.3 (Ubuntu 24.04), 1,080 valid streams (zlib levels 0 to 9, its five strategies, windows of 8
to 15 bits, flushes half way, and both RayoMD modes, up to 1.5 MB) and 2 million mutated ones
gave the same answer and bytes, under ASan and UBSan (an earlier version, 1.8 million more). The
core test holds it to libdeflate on 6,000 mutated streams (libdeflate agreed with zlib on 100,000)
and fails on an inflater that skips the checksum for one byte in 256. Against libdeflate 1.26, in
process, one CPU, a row at a time against the whole image, best of 15:

| Image | libdeflate | RayoMD |
| --- | ---: | ---: |
| illustration, 2048x2048 | 2.67 ms | 1.65 ms (-38 %) |
| the mascot | 1.14 ms | 1.15 ms (+1 %) |
| opaque photo as RGBA | 6.25 ms | 7.58 ms (+21 %) |
| noisy photo | 9.06 ms | 9.74 ms (+8 %) |
| screenshot | 1.43 ms | 0.94 ms (-34 %) |
| soft shadows | 0.99 ms | 1.21 ms (+22 %) |
| sparse sprites | 0.43 ms | 0.29 ms (-32 %) |
| all seven | 21.97 ms | 22.56 ms (+2.7 %) |

All seven took 13.9 % longer than libdeflate's when each refill came before its lookup. At `-O2`
the inflater is 1.5 % slower than at `-O3` for half the code (8.9 KB), so it builds at `-O2`.
Adler-32 now runs in AVX2 where the CPU has it, 32 bytes a step: 15.6 → 20.2 GB/s on 16 MiB, the
same sums. libdeflate stays only as the tests' reference; the Linux executable is 1,289,192 bytes
in a local release build.

Cold, p50 of 40, against 4.1.0:

| Document | 1 CPU | 2 CPUs | 4 CPUs |
| --- | ---: | ---: | ---: |
| the mascot | 12.27 → 11.55 ms (-5.9 %) | 8.69 → 7.66 (-11.9 %) | 8.69 → 7.23 (-16.8 %) |
| `tester.md`, local images | 13.17 → 12.50 (-5.1 %) | 9.54 → 8.54 (-10.5 %) | 9.54 → 8.10 (-15.1 %) |
| screenshot | 6.45 → 5.49 (-14.9 %) | 5.25 → 3.78 (-28.0 %) | 5.36 → 4.43 (-17.3 %) |
| noisy photo | 33.30 → 33.71 (+1.2 %) | 29.53 → 26.55 (-10.1 %) | 29.52 → 27.03 (-8.4 %) |
| illustration, 2048x2048 | 27.70 → 25.40 (-8.3 %) | 22.96 → 20.23 (-11.9 %) | 22.99 → 19.27 (-16.2 %) |

The noisy photo is mostly literals, where the inflater is still slower than libdeflate. In
process, p50 of 300, one CPU: the mascot 8.67 → 8.39 ms, the screenshot 3.97 → 3.27, the
illustration 34.41 → 22.37 (4.1.0 faulted its 16 MB buffers in again once its 2 MB pages were
used up), the noisy photo 20.48 → 21.14; two CPUs: 5.63 → 4.64, 3.17 → 1.90, 19.71 → 17.16,
17.04 → 14.83. `--batch` of 48 mascot documents: -2.3 % on one CPU, -3.6 % on two, -5.9 % on
four; of 21 mixed ones -10.6 % on one CPU and within 4 % either way, run to run, on two and four.
Peak RSS of a cold export:

| Export | 4.1.0, 1 CPU | Now, 1 CPU | 4.1.0, 4 CPUs | Now, 4 CPUs |
| --- | ---: | ---: | ---: | ---: |
| the mascot | 11,496 KiB | 9,040 KiB | 12,176 KiB | 12,900 KiB |
| `tester.md`, local images | 12,008 KiB | 9,632 KiB | 12,876 KiB | 13,280 KiB |
| screenshot | 8,404 KiB | 6,080 KiB | 8,484 KiB | 9,212 KiB |
| illustration, 2048x2048 | 28,468 KiB | 8,668 KiB | 28,796 KiB | 11,976 KiB |
| noisy photo | 22,804 KiB | 20,320 KiB | 23,536 KiB | 21,572 KiB |

On four CPUs three helpers' buffers and encoder tables cost up to 0.7 MiB more on the mascot.
The noisy photo's memory is now mostly its own PNG and its 2.8 MB stream. At the size limit an
image no longer needs 120 MiB of decode buffers: a window, a few rows and at most six 1 MiB
segment buffers. TSan finds nothing in the core tests, five single exports and two batches on
four CPUs; under ASan and UBSan the 1,279 hostile PNGs decode as before; the corpus differs
from 4.1.0 only in image streams, with the same pixels.

**The helper pool shared among decodes.** A `--batch` of 48 mascot documents with its default six
workers on twelve CPUs peaked at 68.5 MiB of RSS against 4.1.0's 61.9: each worker's decode found
eleven CPUs free of other decodes, asked for three helpers, and so kept six segment buffers,
while the pool has three helpers for the whole process. A decode now asks for at most its share,
three divided by the decodes in progress: 59.9 → 61.8 MiB against 4.1.0, the rest being the
documents' streams, which the batch keeps. Single exports ask for what they did, and batches on
two and four pinned CPUs ran -5.1 and -8.7 % (48 mascots), -4.0 and +2.0 % (21 mixed) against
4.1.0; unpinned, +2.6 and -3.9 %, within the batches' run-to-run spread of about 4 %.

**Paeth rows unfiltered in pairs.** Unfiltering Paeth is one chain of dependent pixels, so its
latency bounded it, not its width. Two Paeth rows now go together: one AVX2 register holds row
A's pixel k and row B's pixel k-1, whose upper neighbour is A's pixel k-1, just computed, so the
two chains run side by side; the predictor picks with `abs` and `blendv` on 16-bit lanes. After a
Paeth row the decoder takes the next row too and gives it back (`ZlibReader::Unread`) when it is
not Paeth: pairing every row slowed the screenshot, whose rows are all None. Sub with four bytes
per pixel is a prefix sum over sixteen bytes. On 2048 RGBA rows of 2048 pixels, Paeth took 12.83
ms one pixel at a time on SSE2, 9.05 with the AVX2 blend and 6.67 in pairs; Sub 1.41 → 1.02 ms
per 8 MiB. AVX2 Sub and Average one pixel at a time were slower than SSE2 (2.93 against 2.63 ms,
5.30 against 5.12). Filter scoring and filtering compute Paeth on 32 bytes without widening
(|a + b - 2c| from byte differences; equal to the scalar predictor on all 2^24 inputs), and a
residual's cost is `vpabsb` summed by `vpsadbw`: 2 to 4 % on the mascot and the illustration.
Two filters' sums in the halves of one accumulator, which removes the scoring loop's one spill
to the stack, stayed within 0.6 % in process and was not taken.
The inflater's fast loop keeps the output position before the window in a register and tests
end-of-block and invalid codes in one branch: against libdeflate on the test images (`-O2`, as
built), +4.0 → +2.7 % in total, the opaque photo +23.8 → +20.4 %, soft shadows +21.5 → +19.5 %,
the noisy photo unchanged at +8.8 %. The PDFs are the same bytes as 4.2.0's (corpus, 108
documents). Cold `--export`, p50 of 40, against 4.2.0 built from its tag (2026-10-13, pinned
CPUs):

| Document | 1 CPU | 2 CPUs | 4 CPUs |
| --- | --- | --- | --- |
| the mascot | 11.49 → 11.05 ms (-3.8 %) | 7.71 → 7.16 (-7.1 %) | 7.19 → 6.97 (-3.0 %) |
| `tester.md`, local images | 12.40 → 11.94 (-3.8 %) | 8.59 → 8.01 (-6.7 %) | 8.04 → 7.83 (-2.6 %) |
| screenshot | 5.48 → 5.52 (+0.8 %) | 3.81 → 3.95 (+3.6 %) | 4.41 → 4.43 (+0.5 %) |
| noisy photo | 33.75 → 32.58 (-3.5 %) | 26.73 → 25.34 (-5.2 %) | 26.58 → 25.35 (-4.7 %) |
| illustration, 2048x2048 | 25.02 → 20.44 (-18.3 %) | 20.24 → 14.27 (-29.5 %) | 19.03 → 14.35 (-24.6 %) |

`--batch` without the image cache, 48 mascot documents: -6.2, -5.4 and -5.1 % on one, two and four
CPUs; 21 mixed ones -7.2, -6.5 and -6.2 %. The screenshot's loss on two CPUs is a race, not
work. Its first colour segment is a full 1 MiB; when the decoder finishes it before the helper
has compressed its end, the next segment takes a third buffer, which lies in the region's second
2 MB page, and that page is zeroed whole. The faster decoder wins that race more often, so 4.2.0
faulted 1.0 huge pages per export and this code 1.8; the screenshot's code path is the same as
before. On 4 CPUs the same race makes peak RSS bimodal, about 10.2 or 12.7 MiB on the mascot:
4.2.0 stayed low in 7 of 21 runs, this code in none. The maximum is unchanged, as the buffer
limit is. On 4 KiB pages a third buffer would take about 175 faults for its 0.7 MB instead (an
estimate, not measured), and waiting for the helper would stall the decoder on images that
compress slowly.

**Images checked against their files.** A process keeps decoded images and failed sources for
later documents (`--serve`, `--batch`, the app), and until 4.2.1 kept them by path alone: a PNG
replaced on disk during `--serve` still exported as the old image. Each entry now holds the
file's stamp (size, last write, last status change or creation, and inode on POSIX;
`RayoMd::File::StampOf`), a lookup with another stamp misses, and storing replaces the entry.
The price is one `stat` per image a document shows, on top of the canonicalisation the image
policy already does: a warm `--bench` of `tester.md` with its one PNG 251.5 → 258.5 µs per
export (+2.7 %, 20,000 exports, one pinned CPU, 2026-10-13), of which callgrind counts 1,570
user instructions (+0.07 %); the rest is the system call, 2.3 µs alone on that path. One-shot
exports and documents without images are unchanged.

## Text path, October 2026

**Plain code blocks without a vector per line.** A code block without a highlighted language
(or under `--no-highlight`) was split into a vector of lines, each line wrapped into a vector of
strings, and in the Unicode renderer decoded from UTF-8 line by line. Both renderers now read its
lines and pieces where they are, as the highlighted path does: the standard renderer copies a
line only when it has bytes to drop, the Unicode renderer decodes the block once. The same sums
in the same order, so the same breaks: the corpus' 108 PDFs and 12,000 generated documents of
code blocks (tabs, CR and CRLF, byte-order marks, non-ASCII and invalid UTF-8, long and empty
lines; both renderers, highlighting on and off) are byte-identical. `tools/benchmark.py ab`,
seven rounds, one pinned CPU (2026-10-13): 30 KiB of C++ in 30 blocks 350.2 → 201.6 µs
(-42.4 %), with a Polish comment on every line 483.6 → 317.8 µs (-34.3 %); callgrind -44.7 % and
-22.9 % instructions, README.md -2.2 %. The watch suite, which has few such blocks, ran +0.85 %
(A/A +0.23 %), `performance.md` +1.65 % on the same instructions (callgrind, `tester.md` -0.4 %),
and every function but the two `RenderCode`s kept its size; built with `-falign-functions=64
-falign-loops=32` the suite is +0.01 % (A/A -0.42 %) and `performance.md` -0.90 %. That is
placement, which any change to `tiny_pdf.cpp` moves.

**Two-byte CID strings.** The Unicode renderer wrote text as hex strings, four digits a CID;
it now writes literal strings of two bytes a CID (the code point, high byte first). `(`, `)`
and `\` take a backslash, CR is written as `\r`, which a reader would otherwise take for LF;
NUL and LF stand for themselves. One table lookup tests a CID below U+0D00 (no high byte
there needs a backslash), two the others, and an escape-free CID is one 16-bit store. Text
bytes halve: `performance.md` 711,291 → 526,833 bytes (-26 %), README 252,185 → 206,091,
`unicode_96kb.md` 927,993 → 779,145; with `--compress` -9 % and -4 %. Fresh `--export`, one
pinned CPU, median of 41: `performance.md` 4.70 → 4.52 ms (-3.8 %), `unicode_96kb.md` 5.17 →
5.01 ms (-3.0 %). Warm, against the build before (`tools/benchmark.py ab`, seven rounds):
-1.10 % instructions (Unicode documents -0.7 to -2.3 %), time +0.12 % (A/A -0.17 %). Testing
each byte with four compares cost +4.5 % instructions on the Unicode watch documents and two
loads with a byte-wise store still +0.6 %; a parenthesis is a mispredicted branch (668 in
`performance.md`, 713 more misses an export). 650 PDFs of 130 documents (generated text full of
escapes, Braille and U+29xx arrows, plus the repository's Markdown) under five option sets
(plain, `--pdfua`, `--pdfa`, `--compress`, `--page-numbers --toc`) pass `qpdf --check`, and
Poppler 22.12 extracts the same text and renders the same pixels at 40 dpi as from the hex
strings.

## Measured opportunities

Findings that could make RayoMD faster later, with the evidence and the reason
they were not taken yet. Add to this list when a change turns one up.

**RayoMD's encoder against ISA-L.** After the image work, compressing the colour is the longest
stage of a new image: 4.2 ms of the mascot's 8.7 on one CPU, where ISA-L's level 1 takes 2.5 ms
for streams 16 % larger (2026-10-13). ISA-L's speed is hand-written assembly over a single
hash probe; a C version of its parse in the image mode would have to keep RayoMD's stream sizes
to be worth taking. The cheap ways lose size on some image (the test planes, one thread): one
probe instead of two is 12.8 % faster but the illustration's colour grows 19 %; skipping sooner
and inserting less inside matches is 6.7 % faster and soft shadows' colour grows 10 %. Flushing
the bit writer only when full cost 15 % more time, and one table for literal codes and lengths
6.9 %. Three literals a flush, prefetching the next bucket, comparing matches sixteen bytes at a
time and rejecting the second probe early were each within 0.4 % (2026-10-13). Helper threads
(above) now hide much of the compression on more than one CPU.

**The inflater on literals.** RayoMD's inflater is 20 % slower than libdeflate on the opaque
photo and soft shadows, 9 % on the noisy photo, streams of mostly literals or short matches, and
2.7 % over all the test images (2026-10-13, `-O2`). A BMI2 build of the inflater (`bzhi`,
`shrx`) gained nothing, and neither did libdeflate's `bitsleft` with garbage above its low byte,
which subtracts whole entries and saves an AND on each code: the total the same, the opaque photo
-0.7 %, the illustration 2 to 6 % slower.

**`-O2` on Windows.** Windows release builds use `-O2`, Linux `-O3`. The same
source built both ways with g++ 13.3 on Linux (2026-10-08, `tools/benchmark.py
ab`, seven rounds, watch fixtures) is 5.8 % slower at `-O2` (geometric mean;
96 KiB Unicode +14.6 %, ASCII +1.4 %) and runs 3.0 % more instructions, with
44 KB less code. Next step: measure `-O2` against `-O3` for the engine
translation units only (`tiny_pdf.cpp`, `inline_markdown.cpp`,
`markdown_parser.cpp`) with MinGW g++ on Windows hardware, and weigh it
against the executable size.

**Latin PDF/A on the standard renderer.** Under `--pdfa` an ASCII or Latin document
leaves the standard fonts for the Unicode renderer: 2026-10-13, `baseline.md` +59 % time
and +57 % size, `ascii_96kb.md` +26 % and +25 % (with four hex digits a glyph: +72 % and
+85 %, +32 % and +54 % on 2026-10-09). The bytes of the text are not the cost: a build that
wrote one byte a glyph, the most a TrueType font embedded as a simple WinAnsi font could
give, was 0.6 to 1.3 % faster and 9 to 11 % smaller, so that second embedding path (simple-font
widths, a `cmap` subtable PDF/A accepts) was not taken. The time is the Unicode renderer's:
wrapping and measuring wide strings with TrueType advances (`WrapStyledInline`, `TextWidth`,
`AppendUtf8ToWide`) and a glyph lookup per character. Latin PDF/A at the standard renderer's
speed needs that renderer on embedded fonts: TrueType regular, bold, italic and mono faces with
their own advances in place of the AFM tables, which changes how `--pdfa` documents look (real
bold instead of a second offset copy) and must not slow the default path's constant metrics.

**A larger inlining budget for `tiny_pdf.cpp`.** The file sits at GCC's
`inline-unit-growth` limit, so edits move which hot calls stay inlined.
Raising the budget for that file alone, against `-O3` at the default
(597,840 bytes of code):

| `--param=inline-unit-growth` | Code | Time | Instructions | ASCII 96 KiB | Unicode 96 KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 60 | +32 KB | -0.9 % | -0.6 % | -3.5 % | -0.0 % |
| 100 | +93 KB | -1.9 % | -2.2 % | -4.6 % | -2.0 % |
| 200 | +242 KB | -2.9 % | -3.2 % | -5.1 % | -3.6 % |

At `-O2`, 60 gave -2.4 % (96 KiB Unicode -7.0 %) for +26 KB. Not adopted:
+5 % to +40 % of the Linux executable for one to three percent, and a larger
budget only moves the cliff. Splitting the two renderers into their own
translation units would give each its own budget and stop edits in one from
moving inlining in the other; that is the option to try before raising it.

**Byte classes are scanned one byte at a time.** The fast-path checks
(`ContainsByteClass`, `NeedsInlineParse`) and the parser's runs of ordinary bytes
look up a table entry per byte, about 28 instructions per 8 bytes: about 0.6 % of
a Unicode build, but the out-of-line copy of the check alone runs 3.2 % of the
instructions of the 96 KiB ASCII fixture and 3.3 % of the table fixture
(callgrind, 2026-10-09). Comparing 16 bytes at a time with the ten syntax bytes in
SSE2, which every x86-64 CPU has, takes about 22 instructions per 16 bytes; a
`pshufb` nibble table (SSSE3, behind a CPU check) about 8. Measure on short table
cells, where the byte loop dominates, before choosing.

**One text object per line.** Every run of a line is a text object of its own
(`q … rg BT /F1 … Tf 1 0 0 1 x y Tm (…) Tj ET Q`, about 70 bytes besides the
text). In the 96 KiB ASCII watch fixture 2,322 of 3,999 text objects follow
another on the same line. Those could switch font and color inside the text
object of their line and move with `Td` by the width RayoMD computed, so
placement stays exact: 30 to 55 bytes less each, 70 to 130 KB of that 569 KB
PDF, and fewer coordinates to format. Code backgrounds and strike lines are
paths, which a text object cannot hold, so a line would paint its rectangles
first and its strike lines last. It changes the bytes of every document with
links, code or emphasis.

**glibc's mmap threshold and per-build temporaries.** glibc serves an
allocation of 128 KiB or more with `mmap` until it frees such a chunk, and then
raises the threshold to that chunk's size. When the output buffer outgrows its
first reservation, its first buffer is freed, the threshold rises, and later
builds reuse heap memory. A reservation of 8x the input instead of 4x fits the
96 KiB ASCII fixture at once, so the threshold stays low and every warm build
maps its large temporaries afresh: 75 page faults per build and 10 % more time
(`--bench`, 2026-10-09; table fixture +7 %, `baseline.md` +6 %). Batch and serve
modes meet this for every document that fits its reservation. Fixing
`M_MMAP_THRESHOLD` and `M_TRIM_THRESHOLD` with `mallopt` in the CLI, or keeping
the large temporaries alive from one build to the next, would remove the faults;
measure batch mode before choosing. With `GLIBC_TUNABLES` setting
`glibc.malloc.trim_threshold` and `glibc.malloc.mmap_threshold` to 64 MiB and
`glibc.malloc.top_pad` to 16 MiB, the 96 KiB Unicode watch fixture runs 5.4 %
faster (2026-10-09), and a document whose build lifts the heap past glibc's
128 KiB top pad stops paying a trim and a page fault per build: 10 of the 23
µs the bookmarks cost `baseline.md`.

**CIDs in order of first use.** CIDs are Unicode code points, so the
`CIDToGIDMap` stream runs up to the largest one a document shows, uncompressed
without `--compress`: one arrow (U+2192) makes it 16 KB, CJK text up to 80 KB,
now often more than the font subset itself. Numbering CIDs in the order
characters first appear (a code-point table per renderer) would shrink the map to
two bytes per character used, or remove it with `/CIDToGIDMap /Identity` when
CIDs equal the subset's glyph ids, and keep `/W` short. The ToUnicode CMap
already maps CIDs back.

**The `name` table of the font subset.** At 15.6 KB it is now the largest part of
a DejaVu Sans subset (33 KB). It carries the font's copyright and license
notice, which the license asks to keep with copies, so it stays; a subset of its
records that keeps those notices would save about 10 KB per Unicode PDF.

**The object table.** `PdfObjects::Object` is 72 bytes, and the table grows by
doubling: the 73 outline entries of `baseline.md` take it from 110 objects to
184, past the 128 the table had room for, so it is copied into one for 256
(about 6,000 more D1 write misses over 41 builds). Reserving it from the block
count, or a smaller entry (the in-place offset and length could share the view's
fields), would avoid both. Each outline entry also formats up to seven object
numbers with `std::to_chars`, about 50 instructions each; the entries' own
numbers are consecutive and could be formatted once and copied, some 250
instructions per heading.

**Branchless candidate choice in the DEFLATE encoder.** Font programs are its
slowest input (151 MB/s). Callgrind's branch model counts about 0.5 mispredicted
branches per byte there, 43 % of them on whether the newest long-table
candidate and the short-table candidate are within the window, which is close to
random while the tables fill. Comparing all three candidates' first eight bytes
without branches (an out-of-window candidate made to fail) and taking the
longest with conditional moves would leave one unpredictable branch per
position, match or literal. Single large exports could also compress their
pages on several threads; batches already use every core.

**Hot functions grouped in `.text.hot`.** Pinned alignment (`-falign-functions=64
-falign-loops=32`) takes alignment out of a comparison, not which instruction-cache sets
the hot functions fall into: code added anywhere before them moves them. 16 KB of book
code that runs for no single document measured +0.58 % to +0.66 % pinned on the eight
documentation sites, with the same instructions in every function (2026-10-09), and an
unpinned -1.0 % to +0.25 % from build to build. Marking the renderers' hot functions
`__attribute__((hot))` would put them together in `.text.hot`, where edits to cold code
no longer move them; measure whether grouping them changes their speed, and whether the
measurements of later changes become steadier.

**SVG conversion at `-O2`, and forms kept between documents.** `svg.cpp` at `-O2` converts
the nine charts of SVG images in 1.40 ms a build instead of 1.82 ms, and the 2.1 MB scatter
plot in 57 ms instead of 69 ms, for 53 KB more executable (2026-10-09). A batch whose every
document shows the same SVG, a theme logo for one, converts it once per document, as a form
names the document's fonts and images; a form without text and images could be kept like a
PNG, keyed by its path. Measure how often batches repeat an SVG before taking either.

**Object streams for tagged documents.** A PDF/UA file has a structure element for every
paragraph, list item part, table cell and link: 24,228 objects in the fastapi documentation,
each 66 bytes of dictionary on average plus 40 of `obj`/`endobj` and xref entry, 2.6 MB of the
4.4 MB it has over PDF/A (2026-10-09). Compressed object streams (PDF 1.5, allowed by PDF/UA-1
and PDF/A-3) would hold them in a fraction of that, and BuildInto would write a
cross-reference stream. The reversible profile accepts only a classic xref table, so tagged
files with an embedded source would keep the table.

**Table borders in one artifact per row.** Tagged, each table cell paints its border as an
artifact and then its text in its TD: two marked-content sequences a cell, and the tagged
table fixture runs 2.1 times as long as with `--pdfa`. Painting a row's borders first, in one
artifact, then its cells' text would halve the sequences; it changes the order of RenderTable's
loop, which untagged documents must not pay for.

**CID hex text.** `WriteHexText` is still 11.7 to 15.9 % of a warm Unicode build after its
pair table (2026-10-12): per character a glyph lookup and a bitset insertion, scattered loads
that SIMD does not help.

**`BuildInto` moves every page's content once.** 2.6 to 4.7 % of a warm build is that move:
a page is rendered before the text of the objects in front of it is known (font objects,
`/Length` values, later pages), so its bytes shift by that much when the file is assembled.
Rendering elsewhere copies as much; placing the content where it ends up needs another object
order, which changes the bytes.

**Growth without zero-filling in `TailWriter`.** Each drawing operation grows the content
stream by an upper bound, which `std::string::resize` zero-fills before the operators
overwrite it: `_M_replace_aux` is 0.8 to 3.6 % of a warm build. C++23's
`resize_and_overwrite` skips the fill; in C++17 it takes a content buffer type of its own.

**Exact cell reservation in table rows** (2026-10-12): counting a plain row's pipes to reserve
its cells saved `table_96kb.md` 0.57 % of its instructions and cost the other fixtures 0.10 to
0.29 %, most of whose rows fit the four cells reserved now.

**An SSE2 scan of PDF literal strings** (2026-10-12): testing and copying sixteen bytes at a
time in `WriteEscapedLiteral` saved 0.07 % (geometric mean): most strings drawn are shorter.

**SSE2 runs of ASCII in UTF-8 decoding** (2026-10-12): widening sixteen ASCII bytes at a time
in `WriteUtf8Units` saved the single fixtures 0.5 % and cost `unicode_96kb.md` 0.53 %, where
other characters break the runs, as simdutf did.

**A static build against musl** (Alpine, 2026-10-12): 1,304,960 bytes, a short ASCII export
41 % faster than the default build, but `single_01.md` 45 % and a PNG 8 % slower: musl's
allocator. glibc's static build is the `-static` asset.

## Keeping the release light

- Build `Release`; never benchmark Debug binaries.
- Keep curl, simdutf, profiling, TSAN, LTO, and PGO off in the default package.
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

The router retains focused implementations under `scripts/`. Curated, dated
release records live under `docs/benchmarks/`; raw JSON and artifacts do not.
