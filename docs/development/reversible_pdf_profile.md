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

## Storage, interoperability, and performance decision

Profile version 1 stores the source without a PDF stream filter. This keeps the
default Windows and Linux packages mutually recoverable and avoids a mandatory
compressor or decompressor. A standards-valid 1 MiB repetitive-source prototype
was 1,050,026 bytes raw and 5,163 bytes with Flate (3,698-byte compressed
payload). The saving is substantial for repetitive input, but default builds do
not all provide zlib and a decoder would enlarge the strict parser. Compression
is therefore reserved for a future profile, not silently varied within v1.

qpdf accepted the raw and Flate PDF 2.0 prototypes without syntax errors.
Poppler rendered their pages identically and `pdfdetach` extracted both source
attachments byte-for-byte. A qpdf rewrite remained a valid PDF 2.0 document but
was rejected by RayoMD's narrow parser; v1 supports normal viewing and standard
attachment extraction, but does not promise recovery after another application
rewrites the object graph.

PDF 1.7 versus 2.0 header choice had no meaningful size or speed effect. Source
hashing, metadata, and copies run only when embedding is enabled; the disabled
path remains byte-for-byte identical to the baseline PDF 1.7 output.

The maintained harness uses cold process invocations and records p50/p95 wall
time, output size, peak RSS, exact recovery, validators, and allocation deltas:

```sh
python tools/benchmark.py reversible -- --binary build/windows/rayomd.exe \
  --profile-binary build/profile/rayomd.exe --platform windows --suite full
```

Windows results on 2026-07-13 (5 samples through 1 MiB, 3 at 10 MiB):

| Source | Plain / embedded PDF | Plain p50/p95 | Embed p50/p95 | Inspect p50/p95 | Recover p50/p95 | Peak RSS |
|---:|---:|---:|---:|---:|---:|---:|
| 0 B | 970 / 1,914 B | 45.71/189.26 ms | 45.35/47.93 ms | 47.12/49.72 ms | 60.12/75.95 ms | 11.0 MiB |
| 10 KiB | 94,442 / 105,648 B | 46.07/80.77 ms | 50.93/59.21 ms | 51.55/58.62 ms | 54.04/62.86 ms | 11.1 MiB |
| 1 MiB | 9,656,443 / 10,706,007 B | 242.40/253.39 ms | 281.99/318.13 ms | 58.17/72.61 ms | 82.78/91.09 ms | 58.8 MiB |
| 10 MiB maximum | 96,795,566 / 107,282,325 B | 1,845.80/1,915.97 ms | 1,679.28/1,953.34 ms | 216.31/234.09 ms | 187.04/233.80 ms | 486.4 MiB |

The instrumented build showed an embedding delta of 13 allocations for 10 KiB,
1 MiB, and 10 MiB (14 for empty input). Extra allocated bytes were 34,800,
3,149,846, and 31,461,417 respectively. This is approximately three source-size
copies across hashing, object assembly, and the final PDF buffer; it occurs only
on the opt-in path.

The corresponding Linux measurements were made from WSL mounted storage
(`/mnt/e`, not native ext4). The 10 MiB p50/p95 values were 2,847.55/3,182.33 ms
plain, 2,472.71/2,690.13 ms embedded, 727.21/731.95 ms inspect, and
818.69/956.66 ms recover. RSS was unavailable because the optional measurement
helper was not installed in WSL; these are storage-qualified regression data,
not native Linux headline claims.

The normal Windows watch suite showed no regression versus its stored baseline
(warm median -8.2%, cold -33.7%, batch/file -28.5%, stdin-batch/file -8.4%). On
mounted WSL `/mnt/e`, the mixed I/O-sensitive result was warm +0.7%, cold +24.3%,
batch/file -8.1%, stdin-batch/file +8.6%, and serve +13.9%. These suites cover
feature-heavy ASCII, Unicode, tables, links/images, cold and warm export, folder
and stdin batch, and warm serve.

Final binary growth is 22,016 bytes on Windows (+0.80%, 2,760,704 to 2,782,720)
and 24,632 bytes on Linux (+6.71%, 367,096 to 391,728). No runtime dependency was
added. The cold recovery translation unit is optimized for size while the
renderer keeps its normal release optimization flags.

## Limits and hostile input policy

- Maximum PDF inspected or emitted for reversible recovery: 256 MiB.
- Maximum embedded Markdown: 10 MiB. A 32 MiB profile experiment produced a
  310 MB plain PDF, exceeded the reversible PDF ceiling, and peaked near
  1.5 GiB RSS, so larger inputs are rejected before rendering.
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
