# Optimization Research Notes

Research date: 2026-06-06

Scope: papers/specs/primary implementation docs that may inspire further PDF conversion speed work in this repo.

## Highest-Relevance Candidates

### 1. SIMD UTF-8 validation/transcoding

Sources:

- John Keiser, Daniel Lemire, **Validating UTF-8 In Less Than One Instruction Per Byte**, arXiv:2010.03090, Software: Practice and Experience 2021: https://arxiv.org/abs/2010.03090
- Daniel Lemire, Wojciech Mula, **Transcoding Billions of Unicode Characters per Second with SIMD Instructions**, arXiv:2109.10433: https://arxiv.org/abs/2109.10433
- Robert Clausecker, Daniel Lemire, **Transcoding Unicode Characters with AVX-512 Instructions**, arXiv:2212.05098: https://arxiv.org/abs/2212.05098
- `simdutf` project docs/repo: https://github.com/simdutf/simdutf and https://simdutf.github.io/simdutf/index.html

Key idea:

- Use SIMD kernels for UTF-8 validation/transcoding instead of scalar/library paths.
- `simdutf` already packages runtime CPU dispatch and has a single-header route.

How it maps here:

- Current Linux path uses `std::wstring_convert`; Windows uses `MultiByteToWideChar`.
- A hand-written scalar UTF-8 decoder was tested and reverted because results were mixed. A production SIMD implementation may be more worthwhile than custom scalar code.

Experiment to try:

- Vendor `simdutf` single-header in `third_party/` or add an optional CMake dependency.
- Replace non-Windows `Utf8ToWide`.
- On Windows, compare `simdutf` UTF-8->UTF-16LE against `MultiByteToWideChar`.
- Benchmark with `scripts/perf_watch.py --suite full` and targeted Unicode documents.

Risk:

- New dependency and CPU dispatch surface.
- Small docs may not benefit enough to justify the dependency.

Expected impact:

- Medium on Unicode-heavy docs.
- Low on ASCII docs.

Measured result:

- Tested on 2026-06-06 with vendored `simdutf` v9.0.0.
- Slower on the generated stress suite, so `FAST_MARKDOWN_USE_SIMDUTF` is available but defaults OFF.
- The current workload appears to spend more time in layout/PDF/font work than in UTF conversion.
- Focused large-Unicode follow-up was mixed: `unicode_1mb` improved 2.92%, `unicode_5mb` improved 8.99%, but `unicode_2_5mb` regressed 22.05% and `unicode_10mb` regressed 6.02%. This is not stable enough to enable by default.

### 2. SIMD structural scanning / parser index construction

Sources:

- Geoff Langdale, Daniel Lemire, **Parsing Gigabytes of JSON per Second**, arXiv:1902.08318, VLDB Journal 2019: https://arxiv.org/abs/1902.08318
- **Scanning HTML at Tens of Gigabytes per Second on ARM Processors**, arXiv:2503.01662: https://arxiv.org/abs/2503.01662
- `simdjson` performance notes: https://simdjson.github.io/simdjson/md_doc_performance.html

Key idea:

- First pass builds a structural index using SIMD byte classification.
- Second pass uses indexes instead of repeatedly searching/parsing the raw string.

How it maps here:

- Markdown block parsing already uses SIMD newline/ascii scanning.
- Inline parsing still repeatedly searches for markers like `](`, backticks, `$`, emphasis markers, pipes.

Experiment to try:

- Build a per-block marker bitmap/index for bytes relevant to inline Markdown.
- For tables, pre-scan pipe positions instead of rebuilding cells by character.
- For block parsing, extend `LineInfo` to include cheap flags: has pipe, has inline marker, has non-ASCII, has emoji-normalizable byte.

Risk:

- Markdown edge cases can make structural indexes tricky.
- May only matter on large paragraphs/tables.

Expected impact:

- Medium for large/style-heavy docs.
- Low for short docs.

### 3. Region / arena allocation for document-lifetime data

Sources:

- Emery Berger et al., **Reconsidering Custom Memory Allocation**, tech report: https://www.cs.utexas.edu/ftp/techreports/tr01-45.pdf
- Nicolas van Kempen, Emery D. Berger, **Reconsidering "Reconsidering Custom Memory Allocation"**, arXiv:2605.17119: https://arxiv.org/abs/2605.17119
- Dominik Durner, Viktor Leis, Thomas Neumann, **On the Impact of Memory Allocation on High-Performance Query Processing**, arXiv:1905.01135: https://arxiv.org/abs/1905.01135

Key idea:

- Region/arena allocation helps when many objects share one lifetime.
- Query-processing work shows allocator choice can matter significantly in allocation-heavy engines.

How it maps here:

- `Block`, table rows/cells, styled spans, wrapped lines, and page assembly all have document-lifetime or page-lifetime data.
- C++17 `std::pmr::monotonic_buffer_resource` is available without external dependencies.

Experiment to try:

- Add a `DocumentArena` for parsing and wrapping.
- Convert parser internals to `pmr::vector`/`pmr::string` first, not the public API.
- If the public `std::vector<Block>` API blocks this, try streaming parse->render instead.

Risk:

- Requires careful ownership/lifetime design.
- Can increase peak memory if arena capacity is too large or retained across requests.

Expected impact:

- Medium on large documents.
- Potentially good on stress `batch1000` if allocation churn is measurable.

### 4. Full TrueType subsetting / glyph remapping

Sources:

- Microsoft OpenType `loca` spec: https://learn.microsoft.com/en-us/typography/opentype/spec/loca
- Microsoft/OpenType `cmap` spec: https://learn.microsoft.com/en-us/typography/opentype/spec/cmap
- `fontTools subset` documentation: https://fonttools.readthedocs.io/en/latest/subset/
- PDF Tools AG, **Font subsetting - how it works and when to use**: https://blog.pdf-tools.com/2015/05/font-subsetting-how-it-works-and-where.html
- Boxes and Glue font subsetting docs: https://boxesandglue.dev/textshape/subsetting/

Key idea:

- Current code subsets `glyf`/`loca` but preserves original glyph IDs.
- Deeper subsetting remaps glyph IDs densely and rebuilds `cmap`, `hmtx`, `loca`, and compound glyph references.

How it maps here:

- PDF size is still dominated by embedded font data for Unicode docs.
- Smaller font streams reduce output bytes and may reduce write/cache pressure.

Experiment to try:

- Implement dense `old_gid -> new_gid` remap.
- Rewrite compound glyph component references.
- Rebuild `hmtx` for new glyph order.
- Rebuild minimal `cmap` format 4 or rely on `/CIDToGIDMap` if PDF readers permit dropping `cmap` safely for this embedding mode.
- Cross-check output with multiple readers.

Risk:

- High. Font subsetting is error-prone.
- Bad subset fonts can render fine in one viewer and fail in another.

Expected impact:

- High for PDF size.
- Medium for batch conversion time if file writing dominates.
- Lower for repeated in-memory `--bench` timings.

### 5. Batch I/O and io_uring

Sources:

- Constantin Pestka, Marcus Paradies, Matthias Pohl, **Asynchronous I/O -- With Great Power Comes Great Responsibility**, arXiv:2411.16254: https://arxiv.org/abs/2411.16254
- Matthias Jasny et al., **High-Performance DBMSs with io_uring: When and How to use it**, arXiv:2512.04859: https://arxiv.org/abs/2512.04859

Key idea:

- Modern async I/O APIs reduce syscall overhead, but papers warn that naive integration does not always improve end-to-end performance.

How it maps here:

- Stress `batch1000` has much higher wall time than user CPU time on WSL `/mnt/e`, indicating filesystem/host overhead.
- POSIX `write` was tested and reverted because it regressed this workload.

Experiment to try:

- First benchmark on WSL ext4 (`~/tmp/...`) versus `/mnt/e/...` before touching code.
- If Linux-native filesystem is much faster, document that as the recommended benchmark/deploy path.
- Only then consider parallel batch workers or async I/O.

Risk:

- High complexity for little benefit on small PDFs.
- Cross-platform support cost is significant.

Expected impact:

- Potentially high for huge batches on slow mounts.
- Low for single-file conversion.

Measured result:

- Confirmed on 2026-06-06: WSL `/mnt/e` was the main batch bottleneck.
- First controlled comparison: `batch1000` improved from `10.15 s` on `/mnt/e` to `2.20 s` on native WSL ext4.
- Final native WSL ext4 run reached `1.844 s` for `batch1000`.

### 6. Line-breaking algorithms

Sources:

- Donald Knuth, Michael Plass, **Breaking Paragraphs Into Lines**: https://docslib.org/doc/4186638/breaking-paragraphs-into-linesx
- James O. Achugbue, **On the line breaking problem in text formatting**: https://digitalcommons.mtu.edu/michigantech-p/12606/

Key idea:

- Dynamic-programming line breaking optimizes visual quality across the whole paragraph, rather than greedy line-by-line wrapping.

How it maps here:

- This is primarily layout quality, not speed.
- It may increase CPU unless carefully constrained.

Experiment to try:

- Only consider if PDF visual quality becomes a product priority.
- Keep greedy wrapping for speed/default; optional quality mode could use DP.

Risk:

- Slower conversion.
- Requires tuning and language/hyphenation decisions.

Expected impact:

- Negative or neutral for speed.
- Positive for typography.

## Practical Next Steps

1. Use WSL ext4 for Linux batch benchmarks/conversion when possible; `/mnt/e` is much slower for large batches.
2. Leave `FAST_MARKDOWN_USE_SIMDUTF` OFF by default; the measured stress suite was slower with it enabled.
3. Prototype parser/renderer arena allocation with `std::pmr` only if profiling shows allocation churn after the filesystem issue is removed.
4. Treat full TTF glyph remapping as a separate correctness-heavy project with PDF-viewer regression tests.
5. Do not pursue io_uring now; the main I/O win came from avoiding the WSL Windows mount, not changing write APIs.
