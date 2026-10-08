# Native math (TeX subset)

RayoMD typesets a defined **TeX math subset** natively. It is not LaTeX: there are no
packages, macros, counters or cross-references, and anything outside the subset degrades
to readable output instead of failing. No browser, TeX installation, font file or new
dependency is involved.

This file is the engineering contract for the feature. Keep it accurate when the parser,
the layout or the Markdown rules change.

## Guarantees

- **A document without math syntax produces the same PDF bytes as before the feature.**
  `tests/core_tests.cpp` (`CheckNoMathGolden`) pins this with SHA-256 digests of an ASCII
  fixture; the standard-font path is platform independent, so the digests hold on Windows
  and Linux.
- An ASCII document with math stays on the standard-font path. Math never loads, embeds or
  subsets a font.
- Malformed, unsupported or hostile input never crashes, hangs or fails the export. Every
  limit has one defined outcome (see [Limits](#limits)).
- The module has no mutable global state; `Layout` and `Emit` are safe from several threads.

## Markdown syntax

| Form | Meaning |
|---|---|
| `$x^2$` | Inline formula. Pandoc's rule: the opening `$` is followed by a non-space, the closing `$` is preceded by a non-space and not followed by a digit. The first closing candidate decides. |
| `$$ … $$` on its own line(s) | Display formula (block). A blank line inside ends the attempt; an unterminated `$$` is literal text. |
| `text $$…$$ text` | Display formula inside a paragraph, on a line of its own. |
| ```` ```math ```` / `~~~math` fence | Display formula (GitHub style). An empty or unterminated one stays a code block. |
| `\( x \)` | Inline formula, **only with inner spaces**. `\(x\)` stays an escaped parenthesis pair. |
| `\[` … `\]` on their own lines, or `\[ x \]` as a whole line | Display formula. Inline `\[…\]` is not recognised (it is a common bracket escape). |

- `\$` is a literal dollar. `$5 and $10`, `$x$5` and an unterminated `$x` stay literal.
- Code spans and fenced code are opaque: a `$` inside them never starts a formula.
- Emphasis markers inside a formula are inert (`$a*b*c$`, `$a_i + b_j$`).
- Table rows are split at every unescaped `|` as before; write `\|` for a bar inside a
  formula in a table cell (`$\|x\|$` is the formula `|x|`). A table row that contains `$`
  or `\(` keeps its backslashes; other rows drop escape backslashes as before.
- An inline candidate longer than 1,024 bytes (8,192 for `$$…$$`) is not typeset: it is
  shown verbatim as one code span.
- Headings and table cells keep their formulas: the parser stores them between private
  sentinel bytes (`0x01 … 0x02`) and sets `Block::hasMath`. Sentinel bytes in user input
  are removed, and a block without `hasMath` takes the unchanged plain-text path.

## TeX subset

Supported (complete tables: `src/core/math_symbols.inc`, generated):

- Variables, digits, punctuation, primes; Greek letters (lower, upper, variants);
  ordinary symbols, binary operators, relations and negated relations (`\not` too),
  arrows, dots (`\ldots \cdots \vdots \ddots`).
- Superscripts and subscripts, nested and combined; `\limits` / `\nolimits`.
- `\frac \dfrac \tfrac \cfrac \binom`, infix `\over` / `\choose`, `\sqrt`, `\sqrt[n]`.
- Big operators (`\sum \prod \int \oint \iint \iiint \bigcup \bigcap \coprod …`), named
  operators (`\sin \log \lim \max …`), `\operatorname`, `\operatorname*`, `\bmod`, `\pmod`.
- Delimiters: `\left … \middle … \right` and the `\big` family for
  `( ) [ ] \{ \} | \| \langle \rangle \lfloor \rfloor \lceil \rceil` and `.`.
- Accents and decorations: `\hat \check \tilde \acute \grave \dot \ddot \breve \bar \vec
  \overline \underline \widehat \widetilde \overrightarrow \overleftarrow`,
  `\overset \underset \stackrel \substack`, `\underbrace \overbrace`, `\boxed`,
  `\cancel`, `\xrightarrow`, `\phantom \hphantom \vphantom`.
- Fonts and text: `\mathrm \mathbf \mathit \mathbb \mathcal \boldsymbol`,
  `\text \textbf \textit \textrm`; spacing `\, \: \; \! \quad \qquad \hspace`.
- Environments: `matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix` (and starred forms),
  `smallmatrix cases dcases rcases array aligned align alignat gathered gather split
  equation multline eqnarray subarray`; `&`, `\\`, `\hline`; `\tag{…}`.
- Unicode symbols typed directly in a formula (Greek, operators, arrows, super- and
  subscript digits) map to the same glyphs as their commands.

Deliberate deviations and degradations:

- An unknown control word is set as an upright operator name without its backslash
  (`\sgn`, `\tr` come out right); an unknown environment is set as a centred matrix.
- `%` starts a comment only in a multi-line formula, so `$50%$` prints `50%`.
- Only five fonts exist: `\mathsf`, `\mathtt` and `\mathfrak` are set in Times-Roman;
  `\mathbb` is outlined Times-Bold and `\mathcal` outlined Times-BoldItalic.
- `\mathit{diff}` is spaced like the product *d·i·f·f*.
- Symbols that neither Times nor Symbol contains are composed from available glyphs and
  strokes (for example `\mp \ll \gg \mapsto \hbar \circ \oint`); a few rare ones fall back
  to a similar glyph.
- `\text{…}` words with non-ASCII characters are painted with the document's embedded
  font (Unicode documents); the ASCII renderer cannot meet such text.
- Over-wide display formulas are not broken into lines automatically.

## Fonts and PDF output

- Math uses the PDF standard fonts **Times-Roman, Times-Italic, Times-Bold,
  Times-BoldItalic and Symbol**, never embedded, under the resource names `/M1`…`/M5`.
  A renderer that painted at least one formula adds six objects (a `/FontDescriptor`
  with `/Flags 4` for Symbol, then the five font dictionaries; Symbol also carries
  `/FirstChar /LastChar /Widths`) and lists the fonts in every page's `/Font` resources.
  A document without a painted formula adds nothing.
- Symbol glyphs are shown one per text operator, so a viewer that substitutes a font with
  different advances still places every glyph correctly.
- `MathFormula::Emit` is graphics-state neutral (one `q … Q`), writes 7-bit ASCII, and
  paints inside the box it reports, except for the documented 0.19 em left overhang of
  the first glyph.
- The four Times dictionaries are bare, like the existing Helvetica and Courier ones.
  With `--embed-source` the file is declared PDF 2.0, which no longer exempts the standard
  fonts from `/FirstChar`, `/LastChar`, `/Widths` and `/FontDescriptor`; no viewer problem
  is known.

## Layout rules in the renderers

- Math is laid out at **1.08 × the surrounding text size** (Times looks small next to
  Helvetica and Segoe UI).
- An inline formula is one unbreakable box. A line grows only by the amount its tallest
  or deepest formula leaves the normal line box by more than 0.10 × the text size; lines
  without formulas keep the old metrics exactly.
- Source whitespace decides spacing around a formula: `($x$)` and `$n$-th` stay tight.
- A display formula is centred in the available width (body, list item, quote strip).
- Fit: a formula wider or taller than the available space is re-laid out smaller (to
  75 %) and then scaled uniformly, never below **50 % of its nominal size**. If it still
  does not fit, the complete TeX source is shown instead (code style inline, a tinted box
  for a block), wrapped inside the page.
- In formula-bearing lines of ASCII documents, text is positioned with exact Helvetica
  and Courier advance widths; line breaks are still decided with the approximate width
  table used everywhere else.
- Headings and table header cells pass a bold flag: bold Times faces and heavy Symbol.

## Limits

One formula: 16,384 source bytes, 8,192 parse nodes, nesting depth 24, 64 rows × 24
columns, 8,192 draw items. A formula over any limit is not typeset: the renderers show
its complete source. Layout and emission of the worst accepted input take a few
milliseconds; `tests/math_layout_tests.inc` times a hostile set.

Known performance gap (not introduced by math, but math adds a trigger): the emphasis
scan in `inline_markdown.cpp` is quadratic for thousands of *unmatched* emphasis openers
in one paragraph, for example 20,000 repetitions of `*a $b*c$ `.

## Files

| File | Role |
|---|---|
| `src/core/math_parser.{h,cpp}` | Tokenizer and parser for the subset; node arena; error recovery; limits. |
| `src/core/math_layout.{h,cpp}` | TeX-like box layout, draw list, PDF emission, font objects, exact standard-font widths. |
| `src/core/math_symbols.inc`, `src/core/math_font_metrics.inc` | Generated tables (commands, Unicode map, composites, per-glyph metrics). Do not edit. |
| `scripts/generate_math_tables.py` | Regenerates both tables from the Adobe Core 14 AFM files. |
| `src/core/inline_markdown.*`, `src/core/markdown_parser.*` | Which bytes are formulas (`InlineSpan::math`, `BlockType::MathBlock`, `Block::hasMath`). |
| `src/core/tiny_pdf.cpp` | Renderer integration: the "Native math integration" section and the `RAYOMD_MATH_COLD` functions of both renderers. |
| `tests/math_*_tests.inc`, `tests/no_math_golden.inc` | Parser, layout, Markdown and golden tests, included by `tests/core_tests.cpp`. |

Both math translation units are compiled with `-Os`; they run only for documents with
formulas. Engine rules apply: no exceptions, RTTI, iostream, regex or locale.

## Regenerating the tables

The metrics come from the Adobe Core 14 AFM files (Times-Roman, Times-Italic, Times-Bold,
Times-BoldItalic, Symbol, Helvetica, Helvetica-Bold, Courier, plus their `readme.txt`).
They are not stored in the repository. The generator pins them by SHA-256 and prints the
digests into both generated files. Matplotlib ships an identical copy under
`mpl-data/fonts/pdfcorefonts`.

```sh
python scripts/generate_math_tables.py --afm-dir <directory with the AFM files>
python scripts/generate_math_tables.py --afm-dir <directory> --check   # exit 1 if the files would change
```

Adobe's notice travels at the top of both generated files and in `NOTICE`.

## Verifying a change

```sh
build/<dir>/rayomd-core-tests          # exit codes 55-61 are the math checks
python tests/verify_cli.py --binary build/<dir>/rayomd
build/<dir>/rayomd --export tester.md out.pdf native modern normal   # section 3 is the math showcase
```

Look at the rendered pages after any layout change: rasterise the PDF (Poppler's
`pdftoppm` or Ghostscript) and compare formulas against a LaTeX rendering.
