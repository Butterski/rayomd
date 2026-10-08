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

## 3. Math

Inline math sits on the text baseline: $E = mc^2$, $a^2 + b^2 = c^2$, tight forms such as
($x_i$), $n$-th and $2^{10}$, Greek $\alpha + \beta \le \gamma$, and a tall inline fraction
$\frac{\partial f}{\partial x}$ that makes only its own line taller. Currency stays literal:
$5 and $10, and so does an escaped \$x\$.

A display formula is centred and is never shown as source:

$$
\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$

Display math may sit inside a sentence, $$\sum_{k=1}^{n} k = \frac{n(n+1)}{2},$$ and the
paragraph continues on the next line.

```math
x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}
```

\[
\begin{pmatrix} a & b \\ c & d \end{pmatrix} \begin{pmatrix} x \\ y \end{pmatrix}
= \begin{pmatrix} ax + by \\ cx + dy \end{pmatrix}
\]

$$
f(x) = \begin{cases} x^2 & x \ge 0 \\ -x & x < 0 \end{cases}
\qquad \lim_{n \to \infty} \left(1 + \frac{1}{n}\right)^n = e
$$

- List item with $\vec{v} \cdot \hat{n} = \|v\| \cos\theta$ and a display child:

    $$\prod_{i=1}^{n} \mathbf{A}_i \in \mathbb{R}^{m \times m}$$

> Quoted math: $\forall \varepsilon > 0\ \exists \delta > 0$ and \( \overline{z} = x - iy \).

| Quantity | Formula |
| :--- | :---: |
| Area | $\pi r^2$ |
| Absolute value | $\|x\|$ |
| Text | $\text{zażółć} \to \infty$ |

### Heading with $\nabla \cdot \mathbf{E} = \rho / \varepsilon_0$

Unsupported input degrades gracefully: $\notacommand{x} + 1$ stays readable, and an
unterminated $x stays literal.

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
