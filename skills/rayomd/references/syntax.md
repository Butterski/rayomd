# What RayoMD renders

RayoMD renders a subset of Markdown natively. It is not Pandoc, LaTeX or a browser: what is
outside the subset never fails the export but comes out as plain text, code or alt text. Write
within the lists below and the PDF looks as intended.

## Renders

### GitHub Markdown

- Headings (each a PDF bookmark and a `#anchor` link target), paragraphs, `**bold**`,
  `*italic*`, `~~strike~~`, `` `code` ``, links, reference links (full, collapsed, shortcut),
  `<…>` autolinks, bare URLs and e-mail addresses (clickable), rules, block quotes.
- Bullet and numbered lists, nested; task lists (`- [x] done`, `- [ ] open`).
- Pipe tables with column alignment (`|:--|--:|`); the header row repeats on every page the
  table reaches.
- GitHub alerts: `> [!NOTE]`, `> [!TIP]`, `> [!IMPORTANT]`, `> [!WARNING]`, `> [!CAUTION]`.
- Footnotes: `text[^1]` and `[^1]: note`; set after the text, linked both ways, or at the foot
  of the page with `--footnotes=page`.
- Page breaks: `\pagebreak`, `\newpage`, `<!-- pagebreak -->`, and the HTML ones other tools use
  (`<div style="page-break-after: always"></div>`, `break-before: page`, `class="page-break"`).
- A paragraph `[TOC]` or `[[_TOC_]]`: a table of contents there.
- `<br>`, character references (`&copy;`), HTML comments (hidden).
- YAML front matter (`title`, `author`, `subject` or `description`, `keywords`, `lang`): PDF
  metadata, not printed. Without `title` the first heading is the title.
- Unicode text in an embedded subset of a system font (or `RAYOMD_FONT`), with fallback fonts
  for other scripts; Noto Sans CJK first for Chinese, Japanese and Korean.

### Images

- `![alt](path)` with PNG, JPEG or SVG. SVG is drawn as vectors: paths, shapes, transforms,
  clipping, linear gradients, opacity, text, embedded PNG/JPEG. Charts from matplotlib,
  Vega-Lite, Plotly and Graphviz draw close to a browser.
- Relative paths resolve from the Markdown file's folder and must stay inside it (unless
  `--allow-unsafe-local-images`). HTTP/HTTPS images load only with `--allow-url-images`, on
  Windows and the Linux `-curl` build.
- A missing or unreadable image prints its alt text, so always write alt text.

### HTML inside Markdown

- Inline: `<b>`, `<i>`, `<s>`, `<code>`, `<kbd>`, `<u>`, `<mark>`, `<sup>`, `<sub>`, `<a href>`,
  `<q>`, `<br>`; `<img>` inline shows its alt text.
- Blocks: `align="center"`/`"right"` and `<center>` place text, headings and images (also
  Markdown between an opening and a closing `<div>`); `<img width height>` sizes an image; a
  paragraph of images shows them side by side; `id`, `<a id>`, `<a name>` are link targets;
  `<details>`/`<summary>`, `<pre>`, `<blockquote>`, lists, tables and `<dl>` become their
  Markdown counterparts.
- Other tags are removed and their text kept; `<script>`, `<style>` and the like are left out
  with their content.

### Code

Fenced code with a language name is coloured as GitHub's light theme colours it. The 29
languages and the fence names they answer to:

| Language | Fence names |
|---|---|
| Shell | `sh` `bash` `shell` `zsh` `ksh` `ash` `dash` |
| Shell session | `console` `shellsession` `terminal` (`$ ` prompts) |
| PowerShell | `powershell` `ps1` `pwsh` |
| Batch | `bat` `batch` `cmd` |
| JavaScript | `javascript` `js` `jsx` `mjs` `cjs` `node` |
| TypeScript | `typescript` `ts` `tsx` |
| Python | `python` `py` `py3` `python3` |
| Python console | `pycon` (`>>> ` prompts) |
| C | `c` `h` |
| C++ | `cpp` `c++` `cxx` `cc` `hpp` `cuda` |
| C# | `csharp` `cs` `c#` |
| Java | `java` `groovy` `gradle` |
| Kotlin | `kotlin` `kt` `kts` |
| Go | `go` `golang` |
| Rust | `rust` `rs` |
| Swift | `swift` |
| PHP | `php` |
| Ruby | `ruby` `rb` `rake` |
| SQL | `sql` `mysql` `postgresql` `psql` `sqlite` `tsql` `plsql` |
| HTML | `html` `htm` `xhtml` `vue` `svelte` |
| XML | `xml` `svg` `xsd` `xslt` `xaml` `csproj` `plist` |
| CSS | `css` |
| SCSS | `scss` `sass` `less` |
| JSON | `json` `jsonl` `ndjson` `jsonc` `json5` `geojson` |
| YAML | `yaml` `yml` |
| TOML | `toml` |
| INI | `ini` `cfg` `conf` `properties` `env` `dotenv` `gitconfig` `editorconfig` |
| Dockerfile | `dockerfile` `docker` `containerfile` |
| Diff | `diff` `patch` |

Any other name, or none, prints the block in one colour.

### Math (a TeX subset, typeset natively)

| Write | For |
|---|---|
| `$x^2$` | Inline. No space after the opening `$` or before the closing one, no digit right after the closing one. |
| `$$` … `$$` on lines of their own | Display. No blank line inside. |
| ```` ```math ```` fence | Display (GitHub style). |
| `\( x \)` (with the inner spaces) | Inline. |
| `\[` … `\]` on lines of their own | Display. |

- `\$` is a literal dollar; `$5 and $10` stays text. Code spans and code blocks never hold math.
- In a table cell write `\|` for a bar inside a formula.
- Supported: Greek letters, operators, relations, arrows, dots; sub- and superscripts;
  `\frac \dfrac \tfrac \cfrac \binom \over \choose \sqrt \sqrt[n]`; `\sum \prod \int \oint \iint
  \bigcup \bigcap` and the others, `\limits`; `\sin \log \lim \max`, `\operatorname`, `\bmod`,
  `\pmod`; `\left … \middle … \right` and `\big` sizes; accents (`\hat \bar \vec \tilde \dot
  \overline \underline \widehat \overrightarrow`), `\overset \underset \stackrel \substack`,
  `\underbrace \overbrace \boxed \cancel \xrightarrow`, `\phantom`; `\mathrm \mathbf \mathit
  \mathbb \mathcal \boldsymbol`, `\text \textbf \textit`; spacing `\, \: \; \! \quad \qquad
  \hspace`; environments `matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix smallmatrix cases
  dcases rcases array aligned align alignat gathered gather split equation multline eqnarray
  subarray` with `&`, `\\`, `\hline`, `\tag{…}`; Unicode symbols typed in the formula.

### Mermaid diagrams (drawn natively)

A ```` ```mermaid ```` block whose first statement is `flowchart`/`graph`, `sequenceDiagram` or
`pie` is drawn as a vector diagram in the document's font.

- Flowcharts: directions `TB`/`TD`, `BT`, `LR`, `RL`; shapes `[ ]` `( )` `([ ])` `[[ ]]` `[( )]`
  `(( ))` `{ }` `{{ }}` `[/ /]` `[\ \]` `[/ \]` `[\ /]`; quoted labels `id["text"]`; links
  `-->` `---` `-.->` `==>` `--x` `--o` `<-->` `~~~`, longer `--->`; link labels `-->|text|` or
  `-- text -->`; chains and `&`; `subgraph` … `end` (nested, with `direction`); `classDef`,
  `class`, `:::name`, `style` (`fill`, `stroke`, `stroke-width`, `color`, `stroke-dasharray`).
  `<br>` breaks a label's line.
- Sequence diagrams: `participant`/`actor` (`as` alias), `->` `-->` `->>` `-->>` `-x` `--x`
  `-)` `--)`, messages to oneself, `+`/`-` and `activate`/`deactivate`, `Note left of`/`right
  of`/`over`, `loop` `alt`/`else` `opt` `par`/`and` `critical`/`option` `break` … `end`,
  `rect COLOR`, `autonumber`, `title`.
- Pie charts: `pie`, `showData`, `title Text`, slices `"Label" : 42.5` (up to 64).

### Obsidian notes (`--dialect=obsidian`)

Callouts of every Obsidian type (folded ones print unfolded, nesting kept), `==highlights==`,
`%%comments%%` left out, `[[wiki links]]` (`[[Note#Heading|text]]`), `![[image.png|300]]`,
`![[Note]]`, `![[Note#Heading]]` and `![[Note#^id]]` alone on a line printed in place (from
the vault, `--vault=DIR`), `^block ids` left out, `#tags` left out with `--hide-tags`.

## Degrades

| Written | Printed |
|---|---|
| HTML/CSS layout: inline `style`, classes, style sheets, flex/grid, colours, fonts | The text, without the layout. Styles are read only for page breaks. |
| LaTeX packages, `\usepackage`, macros (`\newcommand`, `\def`), counters, `\label`/`\ref`, automatic equation numbers | Not supported. An unknown command prints as an upright word without its backslash (`\sgn` as "sgn"); an unknown environment as a centred matrix. |
| A formula over the limits (16 KB, nesting 24, 64 rows × 24 columns) or too wide for the page at 50 % size | Its TeX source, as code. Over-wide display formulas are not broken into lines: split them with `aligned`. |
| `\mathsf`, `\mathtt`, `\mathfrak` | The roman face. |
| Math with `--pdfua` | The TeX source as text. |
| Other Mermaid types: `classDiagram`, `stateDiagram-v2`, `erDiagram`, `gantt`, `xychart-beta`, `mindmap`, `timeline`, `flowchart-elk`, … | The source as a code block. So does a supported type with a statement outside the subset (`@{ }` shapes, `id>text]`, `(((…)))`, `create`/`destroy`/`box` in sequence diagrams) or over the limits (1,000 nodes, 2,000 edges, 64 participants). |
| An SVG that needs scripts or HTML (an SVG exported from Mermaid) | Its alt text. Use a ```` ```mermaid ```` block instead. |
| SVG radial gradients and patterns; markers, filters and masks | Approximated; not drawn. |
| Code in a language not in the table | One colour. |
| Arabic and Hebrew | Drawn left to right, unshaped. |
| Citations, Pandoc filters and templates | Not supported. |

Write diagrams of other types as a flowchart or a table, equations with the commands above,
and layout as Markdown (tables, `<div align="center">`, page breaks) rather than CSS.
