# RayoMD 2.2.0 optimization audit (2026-07-13)

This report closes the research roadmap in GitHub issue #11. It records kept
changes, opt-in compiler experiments, and rejected prototypes. Generated corpora,
JSON records, PDFs, ETLs, and other raw artifacts remain under ignored
`benchmark-output/` (and `/home/mkuch/` for native-WSL-ext4 runs).

## Environment and decision summary

- Windows 11 Pro build 26200, AMD Ryzen 5 5600X (6 cores/12 threads), 128 GiB RAM.
- Windows compiler: MinGW-w64 GCC 15.2.0, CMake 4.2.3, `-O2` Release.
- Linux: WSL2 kernel 6.6.87.2, Ubuntu GCC 13.3.0, CMake 3.28.3, `-O3` Release.
- Linux batch/performance records in this report ran on native ext4 under
  `/home/mkuch`, not a `/mnt/*` Windows mount.

| Area | Decision |
|---|---|
| Phase/allocation profiling | Kept as compile-time opt-in; absent from normal builds |
| Windows WPR/ETW CPU captures | Completed for tiny ASCII, `tester.md`, 500 KiB table, and large mixed |
| `-O2`/`-O3` + LTO | Kept opt-in; rejected as the default because full-suite latency regressed |
| Fixed-two-decimal formatter | Kept; output-equivalent and repeatably faster with table changes |
| Table width/wrapping changes | Kept; avoids rescans/copies and preserves byte output |
| Whole-build mutex removal | Kept after shared-state hardening and concurrency stress |
| Bounded 1/2/4/6-worker batch pool | Kept; large throughput gains with explicit RSS tradeoff |
| PGO | Kept as a reproducible opt-in experiment; not a release input |
| Parse-to-render streaming | Rejected and reverted; small allocation-byte saving but tiny/serve regression |

The release version is `2.2.0`. LTO, profiling, TSAN, and PGO remain OFF by
default.

## Part 0 — Trustworthy profiles

### Instrumentation

`RAYOMD_ENABLE_PROFILING=ON` adds `src/common/profiling.cpp`, phase scopes, and
standard `new`/`new[]` allocation counters. With `RAYOMD_PROFILE=1`, builds emit
parse, render/layout, font, image, PDF assembly, and file-I/O deltas. Normal
release targets do not compile the profiling source or definition.

Reproduction:

```sh
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Release -DRAYOMD_ENABLE_PROFILING=ON
cmake --build build/profile --config Release
RAYOMD_PROFILE=1 build/profile/rayomd --export input.md output.pdf native modern normal
```

The Windows profile binary was 2,764,288 B versus 2,759,168 B for the release
binary (+5,120 B). A same-session quick suite with output disabled measured the
instrumented warm aggregate at 3.17 ms versus 2.70 ms (+17.4%), cold export at
26.22 ms versus 25.32 ms (+3.5%), and serve-reported median at 4.17 ms versus
3.92 ms (+6.5%). These numbers are profiler overhead, not release overhead.

### Phase and allocation results

Fresh-process Windows results for maintained 500 KiB inputs:

| Input | Parse | Render | Font | Assembly | Allocations | Requested bytes | Hot phase |
|---|---:|---:|---:|---:|---:|---:|---|
| ASCII | 3.98 ms | 13.61 ms | 0 | 2.34 ms | 95,322 | 40.99 MB | render, 68% of tracked phase time |
| Table | 6.35 ms | 12.71 ms | 0 | 2.27 ms | 103,383 | 41.08 MB | render, 60%; parse, 30% |
| Unicode | 3.62 ms | 21.36 ms | 0.60 ms | 3.92 ms | 158,445 | 49.96 MB | render, 72% |

`tester.md` showed why image work needs its own phase: 57.26 ms of image work
was nested inside 57.61 ms of render/layout on its cold local-image conversion.
The 77-byte standard-font ASCII probe used only 84 counted allocations and
142,391 requested bytes. The profiler reports requested allocation bytes, not
live bytes or allocator overhead.

The evidence points to renderer/wrapping work for large text and to decode/cache
work for cold images. It does not justify bundling a general allocator.

### WPR/ETW CPU captures

An elevated `scripts/capture_windows_profile.ps1` run produced four separate
Windows CPU/file-mode ETLs. `tracerpt` validated all four with zero lost events:

| Trace | Input/iterations | Events | Lost |
|---|---|---:|---:|
| `tiny-ascii.etl` | 77 B standard-font ASCII, 200,000 | 1,801,229 | 0 |
| `tester.etl` | `tester.md`, 100 | 1,598,702 | 0 |
| `table-500kb.etl` | 512,671 B table, 50 | 2,195,161 | 0 |
| `large-mixed.etl` | 627,895 B mixed/Unicode, 50 | 2,271,237 | 0 |

The raw ETLs are intentionally ignored (about 1.5 GiB total). The compile-time
phase profiler supplies stable project-level phase names; WPR supplies the
system CPU samples for deeper WPA inspection without burdening release builds.

## Part 1 — LTO validation

CMake exposes `-DRAYOMD_ENABLE_LTO=ON` through IPO support checks. Both LTO
binaries passed `tests/verify_cli.py`; final default packaging checks are listed
below.

### Full-suite comparison

| Platform/metric | Normal | LTO | Delta |
|---|---:|---:|---:|
| Windows binary | 2,758,656 B | 2,043,392 B | -25.9% |
| Windows warm aggregate | 7.565 ms | 7.410 ms | -2.0% |
| Windows cold export | 29.371 ms | 31.434 ms | +7.0% |
| Windows folder batch | 15.901 ms/file | 17.727 ms/file | +11.5% |
| Windows stdin batch | 7.087 ms/file | 7.346 ms/file | +3.7% |
| Windows serve reported | 7.175 ms | 7.385 ms | +2.9% |
| Linux binary | 363,000 B | 297,456 B | -18.1% |
| Linux warm aggregate | 5.665 ms | 5.720 ms | +1.0% |
| Linux cold export | 19.384 ms | 19.563 ms | +0.9% |
| Linux folder batch | 5.249 ms/file | 5.301 ms/file | +1.0% |
| Linux stdin batch | 5.436 ms/file | 5.468 ms/file | +0.6% |
| Linux serve reported | 5.040 ms | 5.305 ms | +5.3% |

Windows LTO also increased batch peak RSS from 19.69 MB to 20.05 MB and serve
peak RSS from 18.81 MB to 19.86 MB. Linux RSS was effectively neutral. The size
win is useful for deliberate packages, but latency is not broad enough to make
LTO the default.

## Parts 2 and 3 — Formatter and table work

Kept implementation:

- fixed-two-decimal scaled-integer formatting with rounding and `to_chars`;
- negative, negative-zero, integer, and trimmed trailing-zero tests;
- compile-time 128-entry ASCII width table with one font-size scale per span;
- wrapped ASCII lines carry their measured width, so aligned cells are not
  rescanned;
- table cells are wrapped by reference rather than copied;
- per-column row wrapping buffers are reused.

Structural pipe indexes were not added: the simpler changes delivered the win
without increasing parser complexity.

The original alternating maintained `watch` runs (Windows GCC 15.2) measured:

| Metric | Baseline range | Candidate range | Result |
|---|---:|---:|---|
| Warm aggregate median | 12.95–14.19 ms | 6.29–7.31 ms | 43.6–55.7% faster |
| stdin batch | 12.56–14.14 ms/file | 10.32–11.08 ms/file | 17.8–21.7% faster |
| Serve reported median | 10.89–12.69 ms | 9.37–9.50 ms | 12.8–26.2% faster |
| Cold export | 53.34–61.86 ms | 50.75–54.41 ms | neutral to 18.0% faster |
| Binary | 2,665,472 B | 2,664,960 B | 512 B smaller |

Folder batch was noisy in that early pair, so no folder-throughput claim comes
from it. The full suite includes randomized narrow 3-column and wide 5-column
tables, long wrapped cells, the 500 KiB dense table, rules, quotes, local/missing
images, normal prose, and Unicode.

A final immediate alternating internal A/B (four runs per binary) isolated the
final thread/cache hardening from machine drift:

| Warm case | Pre-hardening median (range) | Final median (range) |
|---|---:|---:|
| 77 B ASCII | 0.01 (0.01–0.01) ms | 0.01 (0.01–0.01) ms |
| 500 KiB ASCII | 19.31 (18.71–19.52) ms | 19.19 (18.89–20.10) ms |
| 500 KiB table | 20.78 (20.02–21.31) ms | 20.29 (20.07–21.37) ms |
| 500 KiB Unicode | 30.81 (30.26–32.22) ms | 29.02 (28.86–30.53) ms |

Final PDFs for `tester.md`, large ASCII, large table, and large Unicode inputs
were byte-identical to the pre-hardening outputs. This is stronger than a visual
comparison for table alignment, page breaks, object/xref syntax, links, and
font/image resource references.

## Part 4 — Safe concurrent builds and bounded batches

Shared-state changes:

- removed the whole-build mutex and made the last-error code thread-local;
- immutable font initialization now uses C++ thread-safe static initialization;
- the 65,536-entry Unicode hex table is immutable after thread-safe static
  construction (the old manual `static bool` initializer was a latent race);
- font width slots use relaxed atomics;
- image success/failure caches use short locks, decode misses outside the lock,
  double-checked insertion, a 64 MiB decoded-image cap, and a 256-entry failure
  bound;
- font file/CID/ToUnicode/width artifacts use locked shared-pointer caches with
  entry and byte caps; oversized values are returned without being cached;
- expensive cache misses are computed before insertion locks.

Folder and stdin-batch modes now use deterministic sorted jobs, one reusable PDF
buffer per worker, deferred ordered errors, and `--workers=N` (1–64). Automatic
mode caps at six workers, at two above a 16 MiB largest input, and at one above
64 MiB. Windows Pandoc remains sequential.

### Scaling, p50/p95, failures, and RSS

Five rounds, 160 files, all outputs syntax-checked:

| Platform | Workers | Folder median/p95 ms/file | stdin median/p95 ms/file | Peak RSS max | Failures |
|---|---:|---:|---:|---:|---:|
| Windows | 1 | 7.303 / 7.399 | 6.986 / 7.198 | 19.23 MB | 0 |
| Windows | 2 | 4.230 / 4.746 | 4.448 / 4.508 | 23.99 MB | 0 |
| Windows | 4 | 3.130 / 3.354 | 3.166 / 3.259 | 27.58 MB | 0 |
| Windows | 6 | 2.699 / 2.938 | 2.840 / 3.061 | 33.33 MB | 0 |
| Linux ext4 | 1 | 5.582 / 5.925 | 5.532 / 5.611 | 13.93 MB | 0 |
| Linux ext4 | 2 | 2.895 / 3.014 | 2.897 / 2.967 | 21.60 MB | 0 |
| Linux ext4 | 4 | 1.629 / 1.649 | 1.601 / 1.672 | 34.56 MB | 0 |
| Linux ext4 | 6 | 1.235 / 1.281 | 1.210 / 1.273 | 47.59 MB | 0 |

Six workers improved median folder throughput by 2.7x on Windows and 4.5x on
Linux, with the expected RSS increase. The automatic large-input caps prevent
that scaling policy from multiplying memory on very large documents.

`tests/core_tests.cpp` repeatedly builds deterministic ASCII, Unicode, links,
local images, failed images, and mixed table/list documents at 1/2/4/6 threads
and compares every PDF byte-for-byte with a single-thread baseline. It passed on
Windows and Linux. A GCC TSAN build compiled successfully, but the WSL2 runtime
terminated before the test with `FATAL: ThreadSanitizer: unexpected memory
mapping`; the report does not claim a TSAN-clean execution on this host. The
cross-platform deterministic stress, CLI worker matrix, zero failures, and
thread-safe-state audit are the documented equivalent here.

## Part 5 — Balanced PGO experiment

`RAYOMD_PGO=GENERATE|USE` and `RAYOMD_PGO_DIR` are opt-in. The training helper
covers tiny ASCII, feature-heavy ASCII, sized ASCII/table/Unicode, links/images,
folder batch, stdin batch, and serve.

Representative workflow (reuse the same build tree so object/profile names
match):

```sh
cmake -S . -B build/pgo -DCMAKE_BUILD_TYPE=Release \
  -DRAYOMD_ENABLE_LTO=ON -DRAYOMD_PGO=GENERATE -DRAYOMD_PGO_DIR=<profile-dir>
cmake --build build/pgo --config Release
python scripts/train_pgo.py --binary build/pgo/rayomd --corpus <full-suite-corpus>
cmake -S . -B build/pgo -DCMAKE_BUILD_TYPE=Release \
  -DRAYOMD_ENABLE_LTO=ON -DRAYOMD_PGO=USE -DRAYOMD_PGO_DIR=<profile-dir>
cmake --build build/pgo --config Release
```

Windows PGO versus Windows LTO:

| Metric | LTO | PGO-use | Delta |
|---|---:|---:|---:|
| Binary | 2,043,392 B | 1,770,496 B | -13.4% |
| Warm aggregate | 7.410 ms | 6.490 ms | -12.4% |
| Cold export | 31.434 ms | 27.801 ms | -11.6% |
| Folder batch | 17.727 ms/file | 14.912 ms/file | -15.9% |
| stdin batch | 7.346 ms/file | 6.220 ms/file | -15.3% |
| Serve reported | 7.385 ms | 6.055 ms | -18.0% |

Linux PGO was smaller (285,168 B versus 297,456 B) and improved aggregate
medians by 0–2.3%, but its warm p95 rose 4.2% and the comments feature case rose
from 1.46 ms to 1.84 ms (about 26%). That violates the no-underrepresented-case
regression guardrail. PGO therefore remains an experiment, never a checked-in
profile or silent release input. Regenerate profiles after any source, compiler,
flags, or release-corpus change.

## Part 6 — Allocation/streaming prototype

Profiles justified a narrow parse-to-render streaming prototype, not a broad PMR
rewrite. The measured lifetime groups were per-block inline/wrap temporaries, document-lifetime line metadata/owning blocks/pages/PDF objects, caller-owned final bytes, and process-lifetime bounded font/image artifacts.

In clean 500 KiB profiles it removed one large block-vector allocation:
requested bytes fell 2.5% for ASCII, 2.2% for tables, and 1.5% for Unicode, while
allocation counts changed by only one.

An immediate quick-suite A/B rejected it:

| Metric | Vector baseline | Streaming | Delta |
|---|---:|---:|---:|
| Warm aggregate | 2.725 ms | 3.175 ms | +16.5% |
| Cold export | 25.967 ms | 28.233 ms | +8.7% |
| Folder batch | 16.784 ms/file | 16.757 ms/file | -0.2% |
| stdin batch | 6.616 ms/file | 6.783 ms/file | +2.5% |
| Serve reported | 3.610 ms | 4.140 ms | +14.7% |

Peak RSS was effectively neutral: cold 13.03 MB versus 13.03 MB, folder batch
13.67 MB versus 13.61 MB, stdin batch 13.60 MB versus 13.64 MB, and serve
13.51 MB versus 13.34 MB (vector versus streaming). There was no hidden memory
win large enough to offset the latency loss.

The prototype kept public API types stable and produced byte-identical PDFs, but
it failed the tiny/startup-sensitive acceptance rule. It was fully reverted. A
PMR arena was not pursued because the measured allocation opportunity was too
small to justify retention/capacity risk in serve mode.

## Final current-release snapshot

These are dated snapshots, not comparisons across days:

| Platform | Binary | Warm aggregate | Cold export | Folder batch | stdin batch | Serve reported |
|---|---:|---:|---:|---:|---:|---:|
| Windows | 2,759,168 B | 8.110 ms | 29.752 ms | 16.053 ms/file | 7.730 ms/file | 7.295 ms |
| Linux ext4 | 367,096 B | 6.115 ms | 15.965 ms | 5.785 ms/file | 5.608 ms/file | 5.255 ms |

Final peak RSS was 15.81/18.58/20.06/18.49 MB on Windows for cold/folder/stdin/
serve and 9.70/13.89/13.83/13.69 MB on Linux. Worker-count comparisons must use
the dedicated matrix above; this snapshot explicitly uses one worker.

## Correctness, packaging, and release checks

Completed checks:

- clean Release builds with GCC 15.2 (Windows) and GCC 13.3 (Linux);
- Windows and Linux `rayomd-core` concurrency/formatter test;
- Windows and Linux `tests/verify_cli.py` (single-file/stdin exports, links,
  local-image containment, URL-image defaults, Unicode, and option errors);
- Windows and Linux `--doctor` and `--version` (`rayomd 2.2.0`);
- LTO verifier runs on both platforms;
- byte-identical final PDF comparisons for smoke, ASCII, table, and Unicode;
- Python helper compilation/help checks, `git diff --check`, release layout,
  dependency, archive-content, and generated-output hygiene checks;
- Windows single-executable linkage and default Linux no-curl linkage.

## Post-rebase release gate

The completed change was rebased onto `2a336a4` (`v2.1.0`) so the newer shared
renderer dispatch and 2.1 correctness work remain intact. Fresh builds, rather
than pre-rebase artifacts, passed both CTest entries on Windows and Linux,
`--doctor`, `--version`, Python helper compilation, and the deterministic
multi-thread/image core suite.

A final quick-suite smoke on the rebased candidate produced:

| Platform/storage | Warm aggregate | Cold export | Folder batch | stdin batch | Serve reported |
|---|---:|---:|---:|---:|---:|
| Windows workspace | 2.615 ms | 22.453 ms | 14.721 ms/file | 6.366 ms/file | 4.080 ms |
| Linux WSL ext4 `/tmp` | 1.500 ms | 8.060 ms | 2.497 ms/file | 2.487 ms/file | 1.685 ms |

A separate two-round Windows smoke over 160 documents retained scaling from
8.793 to 4.108 ms/file for folder batch and 8.496 to 4.099 ms/file for stdin
batch at one versus six workers, with zero failures. SHA-256 comparison found
all 160 folder PDFs and all 160 stdin-batch PDFs byte-identical across those
worker counts.
These quick-suite values use a smaller corpus than the full release snapshot
above, so they are a post-rebase regression smoke, not a replacement headline.
The Windows run recorded 13.08/13.59/13.59/13.48 MB peak RSS for cold/folder/
stdin/serve. The Linux run kept both the binary and corpus on WSL ext4.

Maintained raw record roots:

- `benchmark-output/issue11-full-final-v2/windows/`
- `benchmark-output/issue11-concurrency-windows/`
- `benchmark-output/issue11-etw/`
- `/home/mkuch/rayomd-issue11-full-final/`
- `/home/mkuch/rayomd-issue11-concurrency/`

The final default remains dependency-light: no browser, LaTeX, Pandoc, general
allocator, LTO, PGO profile, or profiler is added to the native release path.