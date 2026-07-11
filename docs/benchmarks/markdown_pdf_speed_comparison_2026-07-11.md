# Markdown-to-PDF Speed Comparison

Run date: 2026-07-11.

## Result

RayoMD was the fastest converter in every case in this local comparison. Its
median end-to-end time was **74.2x to 113.4x faster than the next-fastest tool**.

Marketing-safe claim:

> RayoMD is the fastest Markdown-to-PDF converter in our July 2026 Windows
> tests, beating Pandoc/XeLaTeX and the popular Node `md-to-pdf` tool by at
> least 74x across three synthetic small-to-medium Markdown workloads.

This does not prove that RayoMD is universally the fastest converter in the
world. It proves that it is the fastest of these tested tools, on this machine,
for these inputs and settings. Use the scoped wording above rather than an
unqualified "fastest in the world" claim.

## Results

| Case | Input | RayoMD | Pandoc + XeLaTeX | `md-to-pdf` | RayoMD vs next fastest |
|---|---:|---:|---:|---:|---:|
| Tiny README | 205 B | **17.93 ms** | 3,022.02 ms | 1,655.77 ms | **92.3x** |
| Medium feature mix | 15,102 B | **19.74 ms** | 3,324.44 ms | 2,238.22 ms | **113.4x** |
| 500-row table | 33,353 B | **30.64 ms** | 3,302.67 ms | 2,272.99 ms | **74.2x** |

Measured timing ranges were 17.30-31.32 ms for RayoMD, 2,986.24-3,492.52 ms
for Pandoc/XeLaTeX, and 1,590.58-2,282.82 ms for `md-to-pdf`.

## Method

- Gigabyte B550 AORUS ELITE system with an AMD Ryzen 5 5600X processor
  (6 cores, 12 threads), 128 GiB RAM, and Windows 11 Pro build 26200.
- RayoMD 2.0.0 native mode, Pandoc 3.9.0.1 with MiKTeX-XeTeX 4.16, and
  `md-to-pdf` 5.2.5 on Node 25.8.1.
- `md-to-pdf` is the actively maintained Marked + Puppeteer/Chromium pipeline;
  its npm page reported about 156,000 weekly downloads when this report was
  prepared: <https://www.npmjs.com/package/md-to-pdf>.
- Each measurement launches a fresh process and includes startup, Markdown
  parsing, PDF generation, and writing the output file.
- One uncounted warm-up was followed by seven measured runs. The table reports
  the median. Each run overwrote the same destination file.
- The corpus was deterministic and local, with no remote resources.
- Every generated file was checked for a PDF header, EOF marker, and a
  non-trivial size.

The three converters do not promise identical rendering or feature coverage.
Pandoc/XeLaTeX is a much more complete publishing system, while `md-to-pdf`
uses browser rendering and includes richer CSS and syntax highlighting.
RayoMD's native renderer deliberately supports a smaller Markdown subset. The
comparison answers how quickly each complete command produces a valid PDF when
that subset is sufficient; it is not a visual-equivalence benchmark.

The older Node `markdown-pdf` package was excluded from the primary comparison
because it uses PhantomJS and its npm release was four years old at the time of
testing. Choosing the newer, much more frequently downloaded `md-to-pdf` avoids
inflating the result with an obsolete comparator.

## Reproduce

Install the Node comparator into the ignored benchmark output directory:

```powershell
npm install --prefix benchmark-output\world-fastest-tooling md-to-pdf@5.2.5
```

Build RayoMD and run the comparison:

```powershell
cmake --build build\windows --config Release
python scripts\compare_markdown_pdf_tools.py `
  --rayomd build\windows\rayomd.exe `
  --node-modules benchmark-output\world-fastest-tooling\node_modules `
  --root benchmark-output\world-fastest-20260711 `
  --runs 7
```

The ignored output directory contains the generated corpus, final PDFs,
`record.json` with full min/median/max data, and a compact `summary.md`.
