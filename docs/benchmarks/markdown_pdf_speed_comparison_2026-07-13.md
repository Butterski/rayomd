# RayoMD 2.2.0 Three-Tool Windows Comparison

Run date: 2026-07-13.

## Scope

This report compares fresh-process elapsed time for RayoMD, Pandoc/XeLaTeX,
and Node `md-to-pdf` on three deterministic synthetic Markdown inputs. RayoMD
had the lowest median in these cases. This is not a universal speed ranking:
the tools have materially different feature and rendering scope, and
the result applies only to the recorded machine, versions, inputs, and options.

## Results

| Case | Input | RayoMD 2.2.0 | Pandoc + XeLaTeX | `md-to-pdf` | Next comparator / RayoMD |
|---|---:|---:|---:|---:|---:|
| Tiny README | `205 B` | **15.45 ms** (15.34-16.55) | 3,210.69 ms (3,078.22-3,527.92) | 1,717.89 ms (1,666.46-1,766.14) | **111.2x** |
| Medium feature mix | `15,102 B` | **17.29 ms** (16.49-18.03) | 3,377.94 ms (3,355.08-3,641.19) | 2,281.90 ms (2,261.89-2,330.59) | **132.0x** |
| 500-row table | `33,353 B` | **18.66 ms** (16.97-19.63) | 3,427.50 ms (3,383.43-3,575.74) | 2,340.90 ms (2,305.30-2,408.92) | **125.5x** |

Values are median milliseconds with min-max ranges across seven measured runs.
The next-comparator ratio divides the lower comparator median by the RayoMD
median; it does not compare rendering fidelity or feature completeness.

For context only, the 2026-07-11 RayoMD 2.0.0 medians on the same corpus and
machine were 17.93, 19.74, and 30.64 ms. The new medians are 13.8%, 12.4%, and
39.1% lower respectively, but cross-day measurements are not treated as a
controlled version-to-version benchmark.

## Environment and Versions

- Gigabyte B550 AORUS ELITE system, AMD Ryzen 5 5600X (6 cores, 12 threads),
  128 GiB RAM, Windows 11 Pro build 26200.
- RayoMD 2.2.0 native mode, Windows GCC 15.2 Release build.
- Pandoc 3.9.0.1 with MiKTeX-XeTeX 4.16.
- `md-to-pdf` 5.2.5 on Node 25.8.1 with Puppeteer/Chromium.

## Method and Caveats

- Every measurement launched a fresh process and included startup, Markdown
  parsing, PDF generation, and output-file writing.
- One uncounted warm-up was followed by seven measured runs; the table reports
  the median and range. Each run overwrote the same destination.
- Inputs were deterministic local files with no remote resources.
- Every generated output was checked for a PDF header, EOF marker, and
  non-trivial size.
- RayoMD implements a focused native Markdown subset. Pandoc/XeLaTeX is a much
  broader publishing system, while `md-to-pdf` includes browser/CSS rendering
  and syntax highlighting. This is not a visual-equivalence comparison.
- Remote-image timing is excluded because network latency is not reproducible.

## Reproduce

Install the Node comparator into ignored benchmark output if needed:

```powershell
npm install --prefix benchmark-output\competitor-tooling md-to-pdf@5.2.5
```

Build RayoMD and run the maintained comparator:

```powershell
cmake -S . -B build\windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build\windows --config Release
python tools\benchmark.py competitors -- `
  --rayomd build\windows\rayomd.exe `
  --node-modules benchmark-output\competitor-tooling\node_modules `
  --root benchmark-output\competitors-2026-07-13 `
  --runs 7 `
  --timeout 180
```

The ignored output contains the generated corpus, PDFs, `record.json` with
version and min/median/max data, and `summary.md`.