# RayoMD No-Network Benchmark Smoke

This fixture is used by CI and release benchmark smokes. It intentionally uses
only deterministic local inputs so benchmark timing does not depend on DNS,
proxies, remote services, or redirect behavior.

## Text And Inline Cleanup

Zażółć gęślą jaźń appears next to resume, naive cafe, Αθήνα, 東京, Москва,
**bold spans**, *italic spans*, ~~strike markers~~, `inline_code()`, and
[stable links](https://example.com/rayomd-smoke).

> Block quotes should wrap consistently while keeping Unicode text on the
> embedded-font path.

- Local rendering stays dependency-light.
- Missing resources should degrade to fallback text.
- The smoke document should remain small enough for fast CI runs.

## Tables And Code

| Feature | Value | Status |
| :--- | ---: | :---: |
| headings | 3 | ok |
| tables | 1 | ok |
| code blocks | 1 | ok |

```cpp
auto pdf = build_markdown_pdf(input, style, margin);
if (pdf.empty()) return smoke_failure;
```

## Local Images

![RayoMD mascot](assets/rayomd.png)

![Missing local fallback](assets/missing-smoke-image.png)

## Formula Box

$$
x^2 + y^2 = z^2
$$
