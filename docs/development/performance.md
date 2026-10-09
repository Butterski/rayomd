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

## Measured opportunities

Findings that could make RayoMD faster later, with the evidence and the reason
they were not taken yet. Add to this list when a change turns one up.

**`-O2` on Windows.** Windows release builds use `-O2`, Linux `-O3`. The same
source built both ways with g++ 13.3 on Linux (2026-10-08, `tools/benchmark.py
ab`, seven rounds, watch fixtures) is 5.8 % slower at `-O2` (geometric mean;
96 KiB Unicode +14.6 %, ASCII +1.4 %) and runs 3.0 % more instructions, with
44 KB less code. Next step: measure `-O2` against `-O3` for the engine
translation units only (`tiny_pdf.cpp`, `inline_markdown.cpp`,
`markdown_parser.cpp`) with MinGW g++ on Windows hardware, and weigh it
against the executable size.

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

**`ContainsByteClass` is scalar.** Every paragraph is classified with an
8-byte unrolled table scan, about 0.6 % of a Unicode build. A vector
classification would cut it, but the gain is small.

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
`CIDToGIDMap` stream runs up to the largest one a document shows, uncompressed:
one arrow (U+2192) makes it 16 KB, CJK text up to 80 KB, now often more than the
font subset itself. Numbering CIDs in the order characters first appear (a
code-point table per renderer) would shrink the map to two bytes per character
used, or remove it with `/CIDToGIDMap /Identity` when CIDs equal the subset's
glyph ids, and keep `/W` short. The ToUnicode CMap already maps CIDs back.

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
