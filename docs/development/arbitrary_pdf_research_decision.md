# Arbitrary PDF-to-Markdown research decision

Date: 2026-07-13
Decision: do not ship

## Context

An arbitrary PDF contains painting operations and positioned glyphs, not the
author's Markdown. Text extraction cannot reliably restore comments, source
whitespace, heading markers, list syntax, table delimiters, link destinations,
image alternatives, or reading order. Untagged multi-column documents are
especially ambiguous; image-only pages additionally require OCR.

This study is separate from `rayomd-source/1`, which recovers exact embedded
bytes and never falls back to a heuristic conversion.

## Candidate review

| Candidate | License and package fit | Capability and quality | Decision |
|---|---|---|---|
| qpdf | Apache-2.0 and deliberately lightweight | Structural, content-preserving PDF transformations; its documentation explicitly says it does not interpret content-stream semantics or convert PDF to other formats | Useful validator only, not a converter |
| Apache PDFBox 3.0 | Apache-2.0, but requires Java 8 plus PDFBox, FontBox, XMPBox, and logging; some image codecs are optional extras | Extracts text, not Markdown structure; upstream warns that hostile PDFs can exhaust CPU/memory or trigger unchecked failures | Reject: runtime and attack surface conflict with the native package |
| PDFium | BSD-style, cross-platform | Full Chromium-derived parser/render stack built with GN/Ninja and many codec/font/color dependencies; V8/XFA can be disabled, but it still recovers page text rather than Markdown semantics | Reject: large build/package and ongoing security-update burden |
| Poppler | GPL-family codebase; not suitable for the Apache-2.0 single-binary distribution | Mature renderer and text/attachment tools, but extracted text loses Markdown syntax and semantic structure | Validator/viewer in the test environment only |
| MuPDF | AGPL or commercial license | Capable renderer/extractor, with font, shaping, image, color, and optional OCR dependencies | Reject: licensing and package cost |
| Tesseract OCR | Apache-2.0 engine, but also needs Leptonica and per-language trained data | Necessary for scanned pages; recognition remains probabilistic and cannot reconstruct original Markdown | Reject: separate large subsystem with weaker guarantees |

Primary references:

- [qpdf overview](https://qpdf.readthedocs.io/en/stable/overview.html)
- [Apache PDFBox](https://pdfbox.apache.org/), [dependencies](https://pdfbox.apache.org/3.0/dependencies.html), and [security model](https://pdfbox.apache.org/security.html)
- [PDFium build guide](https://pdfium.googlesource.com/pdfium/+/refs/heads/main/docs/getting-started.md) and [license](https://pdfium.googlesource.com/pdfium/+/refs/heads/main/LICENSE)
- [Poppler project](https://poppler.freedesktop.org/)
- [MuPDF licensing](https://mupdf.readthedocs.io/en/latest/license.html)
- [Tesseract installation and trained-data model](https://tesseract-ocr.github.io/tessdoc/Installation.html)

## Measured compatibility and quality evidence

The reversible-profile fixtures were processed by two independent tools:

- qpdf accepted both PDF 2.0 raw and Flate prototypes without syntax errors;
- Poppler rendered both prototypes identically and `pdfdetach` recovered both
  1 MiB attachments byte-for-byte.

A qpdf rewrite remained a valid PDF 2.0 file but intentionally failed the
narrow RayoMD parser. Version 1 therefore supports viewing and standards-based
attachment extraction, but not arbitrary rewrites. This avoids a repair parser
and keeps malformed-input behavior predictable.

Poppler text extraction was also unsuitable as a Markdown reconstruction
backend: it retained visible text while discarding source-only content and
Markdown delimiters. No candidate can infer those missing semantics reliably;
adding a layout heuristic would make the output appear more authoritative than
it is.

Generated fixtures, rendered pages, validator logs, and raw timings are kept
under ignored `benchmark-output/issue20-profile-final/`.

## Consequences

- RayoMD does not expose `--convert-pdf-best-effort`.
- The default Windows and Linux packages gain no PDF renderer, JVM, OCR engine,
  language models, or new runtime dependency.
- `--recover-source` remains exact-only and returns “not reversible” for
  arbitrary PDFs.
- A future implementation requires a separate issue, optional package or
  plugin boundary, explicit uncertainty in its command/UI, a representative
  quality corpus, and fresh package-size, latency, RSS, and security evidence.
