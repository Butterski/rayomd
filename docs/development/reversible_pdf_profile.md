# Reversible RayoMD PDF profile

Date: 2026-07-13
Status: implemented, opt-in
Profile identifier: `rayomd-source/1`

## Decision

RayoMD can optionally preserve the exact UTF-8 Markdown input inside a PDF. The
ordinary export path remains unchanged and emits PDF 1.7. An export requested
with `--embed-source` emits PDF 2.0 and uses the standard Associated Files
relationship to identify the attachment as the document source.

The profile is deliberately narrow. RayoMD recovers only the classic-xref
structure written by its own native exporter. It does not repair PDFs, parse
incremental updates, accept object or xref streams, decrypt documents, or infer
Markdown from painted page content.

## PDF object contract

The source profile adds three indirect objects:

1. An unfiltered `/EmbeddedFile` stream containing the original bytes, with
   `/Subtype /text#2Fmarkdown` and `/Params << /Size ... >>`.
2. A `/Filespec` named `source.md`, referenced through both `/F` and `/UF`, with
   `/AFRelationship /Source`.
3. A small XMP `/Metadata` stream using the
   `https://rayomd.dev/ns/source/1.0/` namespace.

The catalog references the file specification through both `/AF` and
`/Names << /EmbeddedFiles ... >>`. The XMP record contains only the profile,
RayoMD version, UTF-8 encoding, exact byte length, SHA-256 digest, and neutral
attachment name. Original paths, credentials, environment values, and fetched
resources are never embedded.

## Storage and performance decision

Profile version 1 stores the source without a PDF stream filter. This keeps the
default Windows and Linux packages mutually recoverable and avoids a mandatory
compressor or decompressor. Compression would save space for repetitive source,
but it requires a new compatible profile decision.

PDF 1.7 versus 2.0 header choice had no meaningful size or speed effect. In the
final Windows release build, a 1,638-byte source added 2,592 bytes total: the
source plus 954 bytes of profile structure. Source hashing, metadata, and copies
run only when embedding is enabled; the disabled path remains byte-for-byte
identical to the baseline PDF 1.7 output.

Measured on 2026-07-13:

| Windows warm case | Plain | Embedded | Opt-in cost |
|---|---:|---:|---:|
| 1.6 KiB Unicode smoke, 500 iterations | 1.25 ms | 1.29 ms | +3.2% |
| 1 MiB ASCII, 10 iterations | 114.27 ms | 132.12 ms | +15.6% |
| 10 MiB ASCII, 1 iteration | 864.44 ms | 1,180.64 ms | +36.6% |

The 10 MiB point is a single large-sample run and should be treated as a scale
check, not a stable headline benchmark. Its 82.6 MB reversible PDF inspected in
about 1.1 seconds and recovered the exact 10,485,760 bytes with matching SHA-256.

The normal Windows watch suite showed no regression versus its stored baseline
(warm median -8.2%, cold -33.7%, batch/file -28.5%, stdin-batch/file -8.4%). On
mounted WSL `/mnt/e`, the mixed I/O-sensitive result was warm +0.7%, cold +24.3%,
batch/file -8.1%, stdin-batch/file +8.6%, and serve +13.9%; these mounted-storage
numbers are regression context, not native Linux performance claims.

Optimizing the cold recovery translation unit for size limited final binary
growth to 17,920 bytes on Windows (+0.65%, 2,760,704 to 2,778,624) and 20,480
bytes on Linux (+5.58%, 367,096 to 387,576). The renderer keeps its normal
release optimization flags.

## Limits and hostile input policy

- Maximum PDF inspected or emitted for reversible recovery: 256 MiB.
- Maximum embedded Markdown: 32 MiB.
- Maximum classic xref entries: 1,000,001 including object zero.
- Maximum XMP profile stream: 16 KiB.
- Exactly one classic xref section and generation zero are accepted.
- Source streams with filters are rejected by profile version 1.
- `/Prev`, `/XRefStm`, `/Encrypt`, xref streams, object streams, duplicate
  `source.md` entries, duplicate profile attributes, malformed offsets,
  truncated streams, invalid UTF-8, and digest mismatches are rejected.

Recovery validates the complete object path, source length, SHA-256, and UTF-8
before returning or writing bytes. Embedded filenames never select the output
path. CLI recovery refuses to overwrite an existing destination and publishes
the completed file atomically.

## Privacy

Embedding is permanently opt-in for this profile. A reversible PDF may disclose
comments, front matter, reference definitions, internal notes, unsupported
syntax, whitespace, and other source content that is not visible on rendered
pages. The CLI flag and Windows GUI both make this distinction explicit.

## Non-goals

- Best-effort arbitrary PDF-to-Markdown conversion.
- OCR or page-geometry reconstruction.
- PDF/A conformance.
- General PDF parsing or repair.
- Embedding local or remote image bytes.
- Silent fallback from exact recovery to heuristic conversion.
