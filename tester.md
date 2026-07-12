# RayoMD Test Report

## 1. Unicode And Typography

This section checks UTF-8 text, diacritics, and mixed writing systems:
resume, naive, cafe, facade, Krakow, Lodz, Tokyo, Москва, Δοκιμή, and Zażółć
gęślą jaźń.

Inline formatting should stay readable: **bold text**, *italic text*,
***bold italic text***, ~~strikethrough text~~, and `inline_code()`.

## 2. Technical Section

The fenced code block should preserve indentation and use a monospaced style.

```go
package main

import "fmt"

func main() {
    // Indentation should be preserved.
    fmt.Println("System Ready")
}
```

## 3. Math And Formula Boxes

Inline math markers should be cleaned up: $E = mc^2$.

A block formula should render as a formula box:

$$
\int_{-\infty}^{\infty} e^{-x^2} dx = \sqrt{\pi}
$$

## 4. Tables And Lists

| Service | Status | Latency |
| :--- | :---: | ---: |
| API Gateway | :) Online | 45 ms |
| Database | :warning: Warning | 120 ms |
| Auth Service | Offline | Timeout |

- First bullet item
- Second bullet item
  - Nested item A
  - Nested item B
1. First numbered item
2. Second numbered item

> This is a block quote. It should be indented and visually distinct from the
> surrounding paragraph text.

---

\pagebreak

## 5. Explicit Page Break

This section should start on a new page after the `\pagebreak` marker.

<!-- pagebreak -->

## 6. Links And Images

![RayoMD mascot](docs/assets/branding/rayomd.png)

![Remote placeholder image](https://picsum.photos/seed/rayomd/200/300)

![Remote image fallback](https://example.com/fail.jpg)

![Local image fallback](nonexistent.png)

[OpenAI](https://www.openai.com)
