# Reversible PDF feature benchmark — 2026-07-13

This report measures the complete `rayomd-source/1` surface added for exact
Markdown recovery. It is a local release-build snapshot, not a universal
performance claim. Each operation starts a fresh process unless identified as
folder batch, stdin batch, or warm serve. Tables report p50/p95 from five runs;
the 10 MiB scale case uses three runs.

## Environment and scope

- Windows 11 workspace build: `2,782,720` bytes; peak RSS measured with psutil.
- Linux WSL build on `/mnt/e`: `391,728` bytes; RSS unavailable because psutil
  was not installed. Mounted-storage results are not native ext4 claims.
- Style `modern`, margin `normal`, one worker for workflow comparisons.
- Remote images remain disabled, so the remote-image case measures deterministic
  fallback rather than network latency.
- Every embedded result was inspected and recovered byte-for-byte.

## Source-scale results

### Windows

| Source | Plain / embedded PDF | Plain p50/p95 | Embed p50/p95 | Inspect p50/p95 | Recover p50/p95 | Peak RSS |
|---:|---:|---:|---:|---:|---:|---:|
| 0 B | 970 / 1,914 B | 38.62/46.39 ms | 39.32/44.19 ms | 40.38/43.38 ms | 53.43/74.88 ms | 2.5 MiB |
| 10 KiB | 94,442 / 105,648 B | 62.95/85.40 ms | 49.37/56.61 ms | 46.54/57.48 ms | 47.29/49.64 ms | 9.7 MiB |
| 1 MiB | 9,656,443 / 10,706,007 B | 190.88/203.28 ms | 237.27/270.64 ms | 71.48/90.24 ms | 64.26/72.29 ms | 58.7 MiB |
| 10 MiB maximum | 96,795,566 / 107,282,325 B | 1,417.96/1,518.04 ms | 1,821.15/1,983.92 ms | 174.84/204.52 ms | 231.45/266.24 ms | 487.1 MiB |

### Linux WSL `/mnt/e`

| Source | Plain p50/p95 | Embed p50/p95 | Inspect p50/p95 | Recover p50/p95 |
|---:|---:|---:|---:|---:|
| 0 B | 29.36/31.28 ms | 23.90/28.66 ms | 18.40/24.01 ms | 35.90/39.50 ms |
| 10 KiB | 33.66/42.08 ms | 54.71/61.44 ms | 31.14/62.21 ms | 38.94/77.95 ms |
| 1 MiB | 236.33/346.58 ms | 265.43/291.92 ms | 173.69/245.92 ms | 107.07/128.11 ms |
| 10 MiB maximum | 3,370.85/3,671.15 ms | 2,511.80/2,539.52 ms | 822.56/941.22 ms | 876.21/903.89 ms |

## Feature-shaped Windows exports

Fresh-process startup dominates tiny cases. The local-image case separately
shows image decoding and PDF image storage cost.

| Feature | Plain / embedded PDF | Plain p50 | Embedded p50 | Inspect p50 | Recover p50 |
|---|---:|---:|---:|---:|---:|
| ASCII | 1,058 / 2,055 B | 31.59 ms | 31.06 ms | 42.45 ms | 45.16 ms |
| Unicode | 151,413 / 152,433 B | 40.90 ms | 55.80 ms | 41.98 ms | 45.66 ms |
| Links | 1,601 / 2,624 B | 58.77 ms | 62.78 ms | 92.81 ms | 76.12 ms |
| Local image | 258,421 / 259,399 B | 219.10 ms | 147.03 ms | 48.40 ms | 58.92 ms |
| Failed image fallback | 1,047 / 2,043 B | 48.46 ms | 48.06 ms | 31.30 ms | 43.05 ms |
| Remote fallback | 1,040 / 2,051 B | 54.42 ms | 48.69 ms | 44.86 ms | 30.44 ms |
| Table and nested list | 1,670 / 2,686 B | 31.08 ms | 32.38 ms | 31.98 ms | 46.75 ms |
| Multipage | 38,520 / 43,874 B | 44.79 ms | 47.21 ms | 42.03 ms | 47.05 ms |
| Source-only/private content | 1,138 / 2,164 B | 49.13 ms | 44.58 ms | 24.87 ms | 45.57 ms |

The small negative deltas are process-scheduling noise, not evidence that
embedding accelerates rendering. The meaningful result is that opt-in profile
work is small relative to process startup for tiny files and does not alter the
ordinary path.

## In-process warm builder matrix

This removes fresh-process startup and runs 80 iterations per cell. ASCII uses
the standard-font path; the last row exercises Unicode font subsetting.

| Case | Style | Margin | Plain | Embedded | Delta |
|---|---|---|---:|---:|---:|
| ASCII 10 KiB | elegant | compact | 1.610 ms | 2.780 ms | +72.7% |
| ASCII 10 KiB | elegant | normal | 1.520 ms | 1.380 ms | -9.2% |
| ASCII 10 KiB | elegant | wide | 1.400 ms | 1.530 ms | +9.3% |
| ASCII 10 KiB | elegant | 54 pt | 1.500 ms | 1.340 ms | -10.7% |
| ASCII 10 KiB | modern | compact | 1.100 ms | 1.100 ms | 0.0% |
| ASCII 10 KiB | modern | normal | 1.210 ms | 1.560 ms | +28.9% |
| ASCII 10 KiB | modern | wide | 1.360 ms | 1.320 ms | -2.9% |
| ASCII 10 KiB | modern | 54 pt | 1.560 ms | 1.290 ms | -17.3% |
| ASCII 10 KiB | tech | compact | 1.020 ms | 1.350 ms | +32.4% |
| ASCII 10 KiB | tech | normal | 1.250 ms | 1.510 ms | +20.8% |
| ASCII 10 KiB | tech | wide | 1.460 ms | 1.340 ms | -8.2% |
| ASCII 10 KiB | tech | 54 pt | 1.270 ms | 1.660 ms | +30.7% |
| Unicode 9.0 KiB | modern | normal | 0.780 ms | 1.130 ms | +44.9% |

At these sub-3 ms durations, per-cell deltas remain scheduler-sensitive. Keep
the matrix for coverage and regression detection; use the larger source-scale
and workflow rows for product-level cost decisions.

## Workflow results

The corpus contains 18 ASCII, Unicode, links, image/fallback, table/list,
multipage, and source-only documents. Per-file values below divide total p50 by
18; startup is amortized across the workflow.

| Platform/storage | Workflow | Plain | Embedded |
|---|---|---:|---:|
| Windows workspace | Folder batch | 8.85 ms/file | 10.08 ms/file |
| Windows workspace | stdin batch | 9.89 ms/file | 9.40 ms/file |
| Windows workspace | Warm serve | 9.43 ms/file | 9.35 ms/file |
| Linux WSL `/mnt/e` | Folder batch | 48.73 ms/file | 41.83 ms/file |
| Linux WSL `/mnt/e` | stdin batch | 45.85 ms/file | 38.43 ms/file |
| Linux WSL `/mnt/e` | Warm serve | 33.59 ms/file | 29.45 ms/file |

## Bounded rejection results

Windows p50/p95: ordinary non-reversible PDF `40.59/46.70 ms`, unsupported
profile `43.09/45.43 ms`, digest mismatch `42.03/45.23 ms`, truncated PDF
`26.48/50.50 ms`, and a 10 MiB + 1 byte source rejected before rendering at
`44.08/60.53 ms`. Peak RSS stayed at 9.7 MiB or below for these cases.

## Size and allocation evidence

- Release growth versus the pre-feature build: Windows `+22,016 B` (`+0.80%`),
  Linux `+24,632 B` (`+6.71%`); no new runtime dependency.
- Embedding added 13 allocations for 10 KiB, 1 MiB, and 10 MiB, and 14 for an
  empty source.
- Extra allocated bytes were 34,800 at 10 KiB, 3,149,846 at 1 MiB, and
  31,461,417 at 10 MiB.
- A 1 MiB repetitive-source interoperability fixture was 1,050,026 bytes raw
  versus 5,163 bytes with Flate. Profile v1 remains raw to avoid a mandatory
  compression dependency and keep every default build mutually recoverable.

## Reproduce

```sh
python tools/benchmark.py reversible -- \
  --binary build/windows/rayomd.exe \
  --profile-binary build/profile/rayomd.exe \
  --platform windows --suite full --samples 5

python3 tools/benchmark.py reversible -- \
  --binary build/linux/rayomd \
  --platform linux-native-ext4 --suite full --samples 5
```

Raw JSON, PDFs, recovered files, and corpora are generated under ignored
`benchmark-output/reversible-profile/`. Keep only dated, caveated summaries in
source control.
