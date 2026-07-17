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

Classic Markdown Regression
===========================

This section is the native-renderer smoke suite for John Gruber's original
non-HTML Markdown syntax.

Setext Level Two Heading
------------------------

Reference links should resolve without showing their definitions: [Gruber syntax][gruber],
[Gruber syntax] [gruber], [Gruber syntax][], and [title on the next line][title-ref].
Automatic links should also be clickable: <https://daringfireball.net/projects/markdown/>
and <test@example.com>.

A reference-style standalone image should use native image layout:

![RayoMD reference image][logo-ref]

An image inside paragraph text has the deliberate fallback before ![inline mascot][logo-ref] after.

This line ends with the two spaces required for a hard break.  
This text must begin on the next rendered line.

    indented_code_block()
      preserved_inner_indent()

> First quoted paragraph.
>
> Second quoted paragraph after a blank quoted line.
>
> ## Heading inside a quote
>
> - Quoted list item
>   with a wrapped continuation.
>
>     quoted_indented_code()
>
> > Nested quote.

> Lazy quote continuation starts here
and continues without another quote marker.

- Loose list item, first paragraph.

    Loose list item, second paragraph.

    > Block quote inside a list item.

        list_item_indented_code()
          preserved_list_code_indent()

    - Nested list item.

Matching code delimiters preserve literal backticks: ``literal ` backtick``.
Classic emphasis renders with *asterisk emphasis*, **asterisk strong**,
***asterisk combined***, _underscore emphasis_, __underscore strong__, and
___underscore combined___. Nested emphasis renders as **strong with _inner emphasis_**.
Intraword_under_scores remain literal, as do unmatched * markers and non-escapable
\q and \> sequences. Escaped classic punctuation remains literal: \*not emphasis\*,
\_also literal\_, \[not a link\], and \`not code\`.

[gruber]: https://daringfireball.net/projects/markdown/syntax "Markdown: Syntax"
[Gruber syntax]: https://daringfireball.net/projects/markdown/syntax "Implicit reference label"
[title-ref]: <https://daringfireball.net/projects/markdown/syntax>
    "Title stored on the following line"
[logo-ref]: docs/assets/branding/rayomd.png
