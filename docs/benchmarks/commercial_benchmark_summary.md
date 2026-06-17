# Commercial Benchmark Summary

Run date: 2026-06-06

Raw report:

- `benchmark-output/commercial/reports/commercial_benchmark_combined_combined_commercial_20260606.md`
- `benchmark-output/commercial/reports/commercial_benchmark_combined_commercial_20260606.json`

Scope:

- 60 total benchmark cases, all passed.
- 48 Linux cases on native WSL ext4.
- 12 Windows cases.
- Native Markdown-to-PDF path only.
- Covered CLI modes: `--bench`, `--export`, `--batch`, `--stdin-batch`, `--serve`.
- Covered styles: `elegant`, `modern`, `tech`.
- Covered margins: `compact`, `normal`, `wide`, `margin=0.75in`.
- Generated Markdown inputs ranged from about 100 KB to 100 MB.
- Extra Windows Pandoc compatibility smoke test: 100 KB ASCII export passed in 8.11 s, output 151,524 bytes.

## Headline Numbers

| Claim Candidate | Result | Caveat |
|---|---:|---|
| 100 MB Linux native export | 7.03 s | WSL ext4, synthetic ASCII-heavy doc, output PDF 428.43 MiB |
| 100 MB Windows native export | 13.48 s | Synthetic ASCII-heavy doc, output PDF 426.96 MiB |
| 100 MB Linux warm engine benchmark | 6.48 s avg conversion | `--bench` warm in-process PDF generation |
| Peak Linux warm throughput | 21.91 MiB/s | 250 KB ASCII warm benchmark |
| Linux 100-file stdin batch | 0.144 s | 100 synthetic ~20 KB files, WSL ext4 |
| Linux 10 MiB serve batch | 0.686 s | 5 synthetic files, warm process |
| Windows 20-file batch | 0.610 s | 20 synthetic ~100 KB files |
| Windows 10 MiB serve batch | 1.764 s | 5 synthetic files |
| Windows Pandoc compatibility smoke | 8.11 s | 100 KB ASCII export through external Pandoc/LaTeX path |

## Memory Notes

| Case | Peak RSS |
|---|---:|
| Linux 100 MB warm `--bench` | 3,260,736 KB |
| Linux 100 MB `--export` | 1,953,332 KB |
| Windows 100 MB `--export` | 2,199,860 KB |
| Linux 10 MiB `--serve` batch | 112,380 KB |
| Windows 10 MiB `--serve` batch | 77,940 KB |

## Marketing-Safe Wording

- "Converts a synthetic 100 MB Markdown document to PDF in 7.03 seconds on Linux WSL ext4 in local testing."
- "Completed 60 native conversion benchmark cases across Linux and Windows with zero failures."
- "Supports single-file, folder batch, stdin batch, warm serve, and warm in-process benchmark workflows."
- "Reached up to 21.91 MiB/s warm conversion throughput on synthetic ASCII Markdown in local Linux testing."
- "Converted 100 small Markdown files through stdin batch in 0.144 seconds on native WSL ext4."

## Caveats To Keep In The Copy

- These are synthetic benchmark documents, not a representative customer corpus.
- `--bench` measures warm PDF byte generation and should not be mixed with end-to-end file I/O timings.
- Linux batch numbers should say WSL ext4/native Linux storage; previous `/mnt/e` testing was much slower.
- The 100 MB synthetic input creates a very large PDF, around 427-428 MiB.
- Native mode is a fast Markdown subset, not a full Pandoc replacement.
- Pandoc mode depends on external Pandoc/LaTeX tooling and should be marketed separately from native speed numbers.
