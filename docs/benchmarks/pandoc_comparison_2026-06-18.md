# RayoMD vs Pandoc Local Export Comparison

Run date: 2026-06-18.

This is a local end-to-end PDF export comparison, not a universal benchmark.
The goal is to show the cost difference between RayoMD's native lightweight
subset and a full Pandoc-to-XeLaTeX PDF pipeline on simple Markdown documents.

## Method

- Platform: Windows, `Microsoft Windows NT 10.0.26100.0`.
- CPU identifier: `AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD`.
- RayoMD: `rayomd 0.1.0`, Windows release binary, `2,364,928 bytes`.
- Pandoc: `pandoc 3.9.0.1`.
- PDF engine for Pandoc: `MiKTeX-XeTeX 4.16 (MiKTeX 25.12)`.
- RayoMD command shape:
  `rayomd.exe --export input.md output.pdf native modern normal`.
- Pandoc command shape:
  `pandoc input.md -o output.pdf --pdf-engine=xelatex --no-highlight -V mainfont=Arial -V monofont="Courier New"`.
- Each case uses one uncounted warm-up run per tool, then reports the median of
  5 measured runs.
- Timings include process startup, Markdown parsing, PDF generation, and file
  writing.
- Inputs are synthetic, deterministic Markdown files with no remote images or
  network-dependent content.

Pandoc is a much more complete document system than RayoMD. These numbers should
not be read as a claim that RayoMD replaces Pandoc. They show the expected
advantage of a narrow native renderer for simple documents where the lightweight
subset is enough.

## Results

| Case | Input | RayoMD median | Pandoc median | Ratio | RayoMD PDF | Pandoc PDF |
|---|---:|---:|---:|---:|---:|---:|
| `large_table_500_rows` | `45,437 bytes` | `38.98 ms` | `7113.89 ms` | `182.5x` | `500,973` | `94,515` |
| `nested_lists_code` | `30,685 bytes` | `20.21 ms` | `6504.84 ms` | `321.9x` | `123,612` | `91,043` |
| `mixed_100kb` | `102,476 bytes` | `52.45 ms` | `10614.53 ms` | `202.4x` | `1,100,237` | `215,143` |

## Reproduce

```powershell
python scripts/compare_pandoc.py `
  --rayomd build\release-check-windows-escalated\rayomd.exe `
  --root benchmark-output\pandoc-comparison-windows `
  --runs 5
```

The script writes the generated corpus, raw outputs, JSON record, and Markdown
summary under the ignored `benchmark-output/` tree.
