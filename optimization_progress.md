# PDF Conversion Optimization Progress

Objective: process all `todo*.md` optimization notes, implement viable speedups, benchmark after each batch, and keep only changes that improve or preserve conversion speed.

## Files Reviewed

- [x] `todo.md`
- [x] `todo_part2.md`
- [x] `todo_v3.md`

## Benchmark Evidence

WSL Release benchmarks using `build/linux-verify/fast-markdown --bench ... elegant normal`.

| Input | Baseline avg | Final avg | Notes |
|---|---:|---:|---|
| `tester.md` | 0.20 ms | 0.12 ms | 40% faster on final post-clean 1000-iteration run. |
| verifier ASCII | 0.017 ms | 0.015 ms | About 12% faster; displayed as 0.02 ms due report formatting. |
| `todo.md` | 1.93 ms | 0.91 ms | 53% faster on final post-clean 1000-iteration run. |
| `todo_part2.md` | 2.64 ms | 1.28 ms | 52% faster on final post-clean 1000-iteration run. |
| `todo_v3.md` | 2.29 ms | 0.79 ms | 66% faster on final 1000-iteration run. |
| emoji-only status-symbol doc | not measured | standard-font-ascii path | Verifies supported emoji normalization can avoid Unicode path when no other Unicode remains. |

Rejected experiment:

- Direct-mapped glyph lookup cache: removed after `tester.md` worsened from 0.14 ms to 0.17 ms and `todo_part2.md` worsened from 1.31 ms to 1.35 ms.

Verification notes:

- WSL Release build and `scripts/verify-linux.sh` completed.
- Windows Release build completed from fresh `build/windows-fix` tree.
- Windows benchmark mode initially failed for nested output paths because Win32 used one-level `CreateDirectoryW`; fixed with recursive directory creation.
- Windows GUI-subsystem executable benchmarks were run hidden with `Start-Process -Wait`.

Windows Release benchmarks using `build/windows-fix/fast-markdown-imgui.exe --bench ... elegant normal`.

| Input | Windows final avg | Notes |
|---|---:|---|
| `tester.md` | 0.35 ms | 1000 iterations. |
| Windows ASCII | 0.06 ms | 5000 iterations. |
| `todo.md` | 2.11 ms | 1000 iterations. |
| `todo_part2.md` | 3.94 ms | 1000 iterations. |
| `todo_v3.md` | 2.39 ms | 1000 iterations. |

## Current Step

1. Complete. All todo files were reviewed; implemented speedups are benchmarked and deferred items are documented.

## Implemented In This Run

- Reused `HexText` for faux-bold text instead of encoding/inserting CIDs twice.
- Replaced `std::set<uint16_t>` CID collection with a bit-backed collector and sorted vector.
- Computed the CID cache key once per Unicode PDF build and reused it across font caches.
- Replaced `std::to_string`/`ostringstream` PDF hot-path assembly with reserved strings and `to_chars` append helpers.
- Hoisted `TextWidth` scale calculation and removed `WrapCodeLine` string copies.
- Converted UTF-8 conversion to `std::string_view` inputs to avoid substring allocations.
- Merged styled spaces into adjacent spans where visually equivalent, reducing `PaintText`/`HexText` calls and PDF content size.
- Added fast paths for plain `StripInlineMarkdown`, rule-line checks, and non-table line rejection.
- Added Linux CLI `mmap` reads with `ifstream` fallback.
- Added GUI/native PDF output-buffer reuse.
- Treated supported status-symbol emoji as ASCII-path-safe after normalization.
- Changed CID width caching from `unordered_map` to a direct lazy array for faster large Unicode documents.

## Deferred Or Skipped

- Full TTF glyph-ID remapping for `cmap`/`hmtx`/`name` subsetting: not implemented yet; needs correctness work because current subset preserves original glyph IDs.
- Parser arena/streaming refactor: not implemented yet; requires changing `Block` ownership/lifetime or rendering directly from a scanner.
- `simdutf`/StringZilla dependencies: not added; external dependency and licensing/build integration need separate decision.
- Knuth-Plass line breaking: skipped for this performance goal because it improves layout quality rather than conversion speed.

## Todo Item Audit

### `todo.md`

- 1.1 Parser `string_view`: already mostly present; added plain inline/table/rule fast paths. `Block::text` ownership refactor deferred.
- 1.2 `IsBlockStart` redundant parsing: already solved by `ClassifyLine`.
- 1.3 SIMD line/ascii scanning: already present.
- 1.4 Arena allocator: deferred; current parser returns owning `Block` data.
- 2.1 Font object/map caches: already present; improved by computing CID key once.
- 2.2 Font subsetting: existing `glyf`/`loca` subset retained; full glyph-ID remap for `cmap`/`hmtx` deferred.
- 2.3 Width lookup: changed from hash-map lazy cache to direct lazy CID cache for large-document speed.
- 2.4 BMP glyph map: already sparse `unordered_map`.
- 3.1 `ostringstream`: removed from `tiny_pdf.cpp` PDF generation.
- 3.2 `F()` allocations: hot PDF paths use `AppendF`; `F()` remains public/reporting helper.
- 3.3 Single small-file write: already present in Win32 writer.
- 3.4 No zlib: left unchanged intentionally.
- 3.5 `PdfObjects::Build` reserve/to_string: reserve already present; integer appends now use `to_chars`.
- 4.1 `WrapText`: partially improved with view/code-line changes; full output-buffer redesign deferred.
- 4.2 UTF-8 conversion: improved to `string_view` inputs and fewer substring allocations; full per-block lazy UTF-8 rendering deferred.
- 4.3 Hex table: already present.
- 4.4 Page reserve: already present.
- 5.1 Font cache in serve: already present; CID key reuse improved.
- 5.2 File reads: Win32 mmap already present; Linux CLI mmap added.
- 5.3 Output buffer reuse: CLI already reused buffers; GUI/native export now reuses a thread-local buffer.
- 6.1 GUI stats: already updated only on text changes.
- 6.2 WaitMessage/vsync: WaitMessage already present; vsync left unchanged.

### `todo_part2.md`

- A.1 Duplicate `HexText` on faux-bold: implemented.
- A.2 Oversized word fallback: improved by avoiding one-character `TextWidth` calls; binary-search splitter not needed for measured win.
- A.3 `MakeWidths` integer allocations: implemented with `AppendInt`.
- B.1 `usedCids` `std::set`: replaced with bit-backed collector and sorted vector.
- B.2 Repeated `MakeCidKey`: computed once per Unicode PDF build.
- B.3 Full `cmap`/`hmtx`/`name` pruning: deferred pending safe glyph-ID remap.
- B.4 Lazy cmap parsing: deferred; startup-only in current benchmark because font is cached.
- C.1 UTF-8 fragment conversion: partially improved with view conversion and no substring allocation.
- C.2 Styled spaces: implemented by merging spaces into adjacent spans when visually equivalent.
- C.3 TextWidth division: implemented with precomputed scale.
- C.4 cp-to-cid cache: tested direct glyph cache and rejected because it was slower.
- C.5 `WrapCodeLine(wstring_view)`: implemented.
- D.1 `SplitStyledWords` reserve: implemented.
- D.2 `SplitTableRow` trim allocation: reduced via `TrimView`.
- D.3 `PdfObjects::Build` `to_string`: implemented with `AppendSize`/`AppendInt`.
- D.4 Numbered list marker `to_string`: implemented with reserved strings and `AppendInt`.
- D.5 `NormalizeSymbols` copy/replace work: added no-symbol fast exits and plain inline fast path.
- E.1 SIMD mixed-mode renderer: deferred as future architecture work.
- E.3 Faster integer formatting: implemented via C++17 `to_chars`.
- E.4 Parser arena: deferred with 1.4.

### `todo_v3.md`

- Streaming parser / incremental read+parse: deferred as larger architecture work; current gains came from low-risk parser/rendering changes.
- `simdutf`: not added; external dependency integration deferred.
- Arena allocator: deferred with `todo.md` 1.4.
- SIMD ascii/line scan: already present.
- Knuth-Plass: skipped for speed goal because it is a quality/layout feature.
- StringZilla: not added; external dependency integration deferred.
- Emoji fallback before path decision: implemented for supported status symbols.
- `usedCids` set: implemented with bit-backed collector.
- `TextWidth` `Codepoints()` allocation: `TextWidth` uses `ForEachCodepoint`; unused allocating helper removed.
- `WrapText` normalization copy: no `NormalizeSpaces` hot-path copy exists in current code; wrapping already scans views.
- Precomputed width scale: implemented.
