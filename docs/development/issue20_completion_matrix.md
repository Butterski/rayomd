# Issue 20 completion matrix

Date: 2026-07-13
Status: complete

Generated artifacts remain under ignored `benchmark-output/`. Every product
requirement is backed by a maintained test, reproducible benchmark, external
validator, or explicit research decision.

| Area | Status | Evidence |
|---|---|---|
| Exact source fidelity | done | Core fixtures round-trip empty, ASCII/LF, Unicode/emoji, CRLF/comments, BOM, front matter, whitespace-only, unsupported syntax, and embedded NUL byte-for-byte. |
| Renderer coverage | done | Both standard and Unicode builders call the shared profile writer; all styles and compact/normal/wide/custom margins are exercised. |
| Document features | done | Reversible links, local images, failed/remote image fallback, tables/lists, and multipage output are covered. |
| Ordinary export isolation | done | Embedding defaults off; ordinary output remains PDF 1.7 and prior byte-identity/watch-suite evidence is recorded in the profile ADR. |
| Strict profile recognition | done | Ordinary PDFs and unrelated Markdown attachments return “not reversible”; unsupported profiles and corrupt/integrity failures have distinct results. |
| Bounded hostile parser | done | Structured dictionaries/arrays/streams and classic xrefs enforce size, count, nesting, offset, reference, filter, filename, UTF-8, and digest constraints. Duplicate keys/attachments, traversal names, cycles, huge IDs, truncation, bad xrefs/lengths, encryption, incremental updates, xref streams, and deep input fixtures are rejected. |
| Atomic recovery | done | CLI verifies fully before publishing, refuses overwrite, and hostile/tampered tests assert that no partial output exists. |
| Windows workflow | done | Open/Ctrl+O/drop validates before editor replacement; persistent profile/producer/size/attachment/digest/privacy state is shown; recovered source uses atomic save with overwrite confirmation. |
| Interoperability | done | qpdf validates PDF 2.0 raw/Flate prototypes; Poppler renders both and extracts both attachments exactly. qpdf rewrites are valid but conservatively unsupported by profile v1. |
| Storage strategy | done | Raw v1 is dependency-free and deterministic. A 1 MiB repetitive source produced a 1,050,026-byte raw fixture versus 5,163 bytes with Flate; compression is reserved for a future profile because it is not universally available in default builds. |
| Performance and memory | done | Maintained `tools/benchmark.py reversible` reports output size, p50/p95, peak RSS, exact recovery, validators, and optional built-in allocation deltas for 0 B, 10 KiB, 1 MiB, and the 10 MiB maximum. Existing watch suites cover cold, warm, batch, stdin-batch, serve, Unicode, tables, links, and images. |
| Package size | done | Windows: 2,760,704 to 2,782,720 bytes (+22,016, +0.80%). Linux: 367,096 to 391,728 bytes (+24,632, +6.71%). No runtime dependency was added. |
| Arbitrary PDF research | done | `arbitrary_pdf_research_decision.md` rejects shipping qpdf/PDFBox/PDFium/Poppler/MuPDF/Tesseract paths in the default product; no heuristic fallback or production issue is created. |
| Documentation/versioning | done | README covers commands, privacy, UI, limits, error behavior, exact-versus-heuristic distinction, and benchmark reproduction. `VERSION` is unchanged. |
| Release verification | done | Windows and Linux release builds, core tests, black-box CLI verification, Python/hygiene checks, and final diff audit pass. |
