# Performance development

Correctness and performance are intentionally separate. Build RayoMD first,
run `tests/verify_cli.py` for behavior, and use `tools/benchmark.py` only when
timing or release evidence is needed. Raw reports belong under ignored
`benchmark-output/`.

## Maintained workflows

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-native-ext4 --suite watch --label local
python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe --platform windows --suite full --samples 5
python tools/benchmark.py compare -- --rayomd build/windows/rayomd.exe --root benchmark-output/pandoc --runs 5
python tools/benchmark.py competitors -- --rayomd build/windows/rayomd.exe --node-modules path/to/node_modules --root benchmark-output/competitors
python3 tools/benchmark.py release -- --from-version 1.1.0 --suite quick
```

The standard `run` workflow covers cold export, warm `--bench`, folder batch,
stdin batch, warm serve, ASCII, Unicode, tables, explicit rules/page breaks,
comments, and optional local images. Use it to protect the default fast path.

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
