# Plan: Markdown features after 5.0 (engine and GUI)

The work chosen from the wiki report
[Markdown-to-PDF Market Needs 2026-10-17](https://github.com/Butterski/rayomd/wiki/Markdown-to-PDF-Market-Needs-2026-10-17)
(the demand numbers and their sources are there): native Mermaid diagrams, heading numbering,
the HTML page-break `<div>`, Obsidian syntax and typing helpers in RayoMD GUI, then the rest of
the report's shortlist as a backlog. CSS styling and a VS Code extension are tracked as issues
of their own (see the end).

Line numbers are those of 5.0.0 (`5b78767`); they move, the names do not.

## Rules every item follows

- **Bytes.** A document that does not use the feature exports the same PDF: `check.sh` over the
  harness corpus (2,875 PDFs, dynamic and static) and the golden digests (`CheckNoMathGolden`,
  `parse_limits_tests.inc`, GUI `test_pdf_golden.cpp`). Syntax that GitHub shows as text
  (Obsidian forms) is opt-in; syntax whose authors plainly meant the feature (a page-break
  `<div>`, a `mermaid` fence) changes only the documents that contain it, and says so in its
  commit.
- **Bounds.** Every scan is linear and bounded, as AGENTS.md requires of the inline parser
  (`InlineScanner`, `ParseBudget`); every new reader of document text gets `Bounded(...)` checks
  in `tests/parse_limits_tests.inc` and seeds or a target in `tests/fuzz/`.
- **Speed.** New work lives in cold functions or its own translation unit (`-Os`), outside
  `tiny_pdf.cpp`'s inlining budget. Gate: warm A/B (`rayomd-lab/harness/ab.sh --instructions`)
  at most +0.3 % instructions on every fixture; if time moves with flat instructions,
  regenerate `src/hot.ld` (`scripts/hot_sections.py`) as in `0967d55`.
- **Both renderers and PDF/UA.** The standard-font and Unicode renderers both draw it; every new
  paint is tagged or marked as an artifact (`verapdf -f ua1`), and text painted in the
  document's font sets a fallback-probe flag (`NeedsRebuild`, AGENTS.md *Correctness
  Guardrails*).
- **Surfaces.** `PdfOptions` field, both CLIs (`main_cli.cpp`, `main_windows_cli.cpp`, help
  text), README *Features* with an example, AGENTS.md *Current Feature Surface*, the wiki page,
  and in the GUI: `ExportSettings` (load, store, `ToPdfOptions`), a row in `OptionsContent`,
  the submodule moved to the engine commit.

## Order

| Step | Item | Repo | Size | Release | Status (2026-10-17) |
|---:|---|---|---|---|---|
| 1 | HTML page breaks, `--page-break-before` | engine | S | 5.1 | done, d8b28d9 |
| 2 | Heading numbering | engine, GUI option | S | 5.1 | done, 775ee9c |
| 3 | Obsidian dialect | engine, GUI option | M | 5.2 | done, 121cd31 |
| 4 | GUI typing helpers | GUI | M | GUI 1.2 | done; GUI options for 2, 3 and 5 too |
| 5 | Native Mermaid (flowchart, then sequence, then more) | engine | L | 5.2 / 5.3 | flowchart, sequence, pie done, e884be7 |
| 6 | Backlog from the report | both | — | later | open |

Small engine items first: they ship value within days and touch the parts Mermaid and the
dialect build on (HTML block mapping, heading marks, `PdfOptions`). Mermaid is the largest and
gets its own contract document before code.

## 1. HTML page breaks

**Now.** Page breaks come only from a line `\pagebreak`, `\newpage` or `<!-- pagebreak -->`
(`IsPageBreakLine`, `markdown_parser.cpp:156-174`). `TagToken` keeps no `style` or `class`
(`html.h:67-75`), so `<div style="page-break-after: always"></div>`, the answer users are given
for Obsidian (79,783 views), Typora and mdbook-pdf, maps to no block at all
(`BlockMapper::Markup`, `html_blocks.cpp:449-458`).

**Do.**
- `TagToken` keeps `style` and `class` (6 bits of `seen` are free; both names pass
  `KeepAttribute`'s 2-6 length check, `html.cpp:140`).
- In `BlockMapper::Markup`, for `div`, `p`, `section`, `hr` and `br` blocks: a declaration
  `page-break-before|after: always` or `break-before|after: page|always` (case-insensitive,
  scanned once within the attribute's 4,096 bytes), or a class `page-break`, `pagebreak` or
  `new-page` (md-to-pdf's `page-break`), makes a `BlockType::PageBreak` before the element's
  content (`before`) or at its close (`after`). Nothing else of `style` is read.
- `--page-break-before=h1|h2` (`PdfOptions::pageBreakBeforeLevel`, 0 = off): a break before
  each heading of that level or above, except at the top of a page; Typora's "Page Break Between
  Top Headings". In `RenderBlocks` (cold branch), so the deferred-break logic
  (`pageBreakPending`, `tiny_pdf.cpp:5883`) collapses doubles.

**Tests.** Each accepted form, top level and in quotes and list items; negatives (`always`
missing, inline `<span>`, `style` on other elements); the option with and without a title page;
books; `Bounded` on 1 MB of `style` attributes. README *Page layout*, wiki *Native Markdown
Support*.

**GUI.** A checkbox "New page before each top heading" (Document section).

## 2. Heading numbering

**Now.** Headings are plain text (`Block::text`); anchors, bookmarks and the TOC all read it
(`HeadingMark::text`, `HeadingTargets::Index` `tiny_pdf.cpp:5044-5065`, `AddOutline`
4427-4520, `ContentsEntry::text`), and a TOC entry finds its heading by the text's address
(`ContentsHeadings` 12029-12043).

**Do.** `--number-headings[=DEPTH]` (`PdfOptions::numberHeadings`, depth 1-6, default 3).
- Numbers `1`, `1.1`, `1.1.1` by level, from the top level present; a document whose only
  level-1 heading comes first (the title, as the TOC already treats it,
  `contents.cpp` `HeadingCensus`) is numbered from level 2. A skipped level shows 0
  (`1.0.1`), as LaTeX numbers sections. *(Done in 5.1.)*
- `block.text` stays unnumbered, so anchors keep GitHub's slugs and `#links` keep working. The
  prefix lives in a side table built once after `PlaceContents` (cold), keyed by the heading's
  text address like the TOC match. It is painted before the text (`RenderHeading`,
  `RenderAlignedHeading`, `RenderMathTextLines`, `RenderQuoteHeading`, both renderers), added
  to `ContentsEntry::text`, and to the outline titles through a substituted mark list as
  `TitledHeadings` (4391-4402) does for footnotes. Info and XMP titles stay unnumbered.
- Books number across files: each file's top headings continue the count.
- Headings in footnotes and the inserted TOC heading are not numbered.

**Tests.** Golden PDFs of both renderers; TOC, bookmark and heading text carry the same number;
`#anchor` links unchanged; PDF/UA heading text includes the number; books. README *Tables of
contents*.

**GUI.** A checkbox and depth field next to "Table of contents" (Contents section).

## 3. Obsidian dialect

**Now.** Only the five GitHub alerts, with the marker alone on the first line of a top-level
quote (`TakeAlertMarker`, `markdown_parser.cpp:687-705`, titles fixed in `kAlertTitles`
`tiny_pdf.cpp:6119-6121`); `==text==`, `[[...]]` and `%%...%%` are literal text; `<mark>`
exists as `kDecorMark` (`PaintDecor`).

**Gate.** `--dialect=obsidian` (`PdfOptions::dialect`, default `github`): GitHub shows all of
these as text, and RayoMD's default follows GitHub. The parser gets the dialect through a
thread-local scope, as `ParseBudget` already passes per-parse state.

**Do, in this order.**
1. **Callouts.** `> [!type] Title` with a custom title; fold markers `-`/`+` (drawn expanded);
   Obsidian's types and aliases (note; abstract/summary/tldr; info; todo; tip/hint/important;
   success/check/done; question/help/faq; warning/caution/attention; failure/fail/missing;
   danger/error; bug; example; quote/cite) with their colours from the theme palette; unknown
   types drawn as note; nested callouts. `Block::alert` takes the wider type range (a padding
   byte); the title is kept in the quote block's own text, which quotes leave empty (check
   while implementing, `sizeof(Block) <= 136` must hold).
2. **Highlights** `==text==` as `kDecorMark`. Only under the dialect: `=` is not in
   `kByteInlineSyntax`, and adding it for every document would slow the fast path.
3. **Wiki links.** `[[#Heading]]` to an anchor of the document (`HeadingSlug`); `[[Note]]`,
   `[[Note#Heading]]` and `[[Note|alias]]` in a book through a name index of its files
   (basename without `.md`, as Obsidian resolves them) added to `BookTargets::FileOfLink`.
   `[[a\|b]]` in tables as in Obsidian.
4. **Embeds.** `![[image.png]]` and `![[image.png|300]]` (width) through the image path and
   `ApplySizeHints`, under the local-image policy. Note embeds `![[Note]]` (transclusion) come
   later, confined to the book's folder like chapters.
5. **Comments** `%%...%%`, inline and as blocks, are left out of the PDF.

*Done in 5.1, differently from the above in three points:* highlights, comments, wiki links and
embeds are rewritten into Markdown and HTML before the parse (`dialect.cpp`
`ObsidianToCommonMark`, line for line), so the inline parser has no second table and a wiki link
passes `Html::SafeUrl` as any link; a wiki link that resolves to no file of a book is a link to
`note.md`, as a Markdown link to the note would be, instead of plain text; and a sized embed is an
HTML `<img>`, so only one with a line of its own is sized.

**Later, same gate family.** MkDocs `!!!` and Docusaurus `:::` admonitions and Markdown
definition lists (backlog), as `--dialect=mkdocs` and an extended set.

**Tests.** Each form and its GitHub-default rendering (unchanged bytes); `Bounded` on a
megabyte of `[[`, `==`, `%%` and `> [!`; fuzz: a third options byte in `fuzz_public.cpp`
`OptionsFrom` (bytes 0 and 1 are full) selects the dialect and the new options. README
*Markdown*, a wiki page *Obsidian Notes*.

**GUI.** A "Markdown" dropdown (GitHub, Obsidian) in the Document section; Omarchy users often
write in Obsidian.

## 4. GUI typing helpers

**Now.** The editor is plain text with grouped undo; insertion from outside is `HandleText`,
`PasteText` or `SetText` (`texteditor.cpp:514-537, 24-39`); no menus, palette or completion;
Enter inserts `\n`, Tab a `\t`; a dropped image only gets a warning (`MainWindow::DropFile`,
`mainwindow.cpp:736-744`); the only popup is `Dropdown`, tied to its button
(`widgets.cpp:600-726`).

**Do.**
1. **Editor API.** `TextEditor::Replace(pos, len, text, caretOffset)` as one undo group with
   the caret placed inside the inserted text (today the caret always lands after it, and
   `Select` ends the group).
2. **Insert popup.** A footer button and `Ctrl+I` open a list anchored at the caret
   (`editorRect + CaretRect()`, as the IME rectangle): page break, table of contents, table,
   callout (with its type list), Mermaid flowchart and sequence templates, math block, front
   matter. A caret-anchored list built from the `Dropdown` pieces (`PushLayer`, `Block`,
   `State<T>`), taking Up, Down, Enter and Escape before `editor_.Frame`
   (`mainwindow.cpp:244`).
3. **Completion.** The same list opens by itself after ```` ``` ```` (fence languages: the
   highlighter's names, `mermaid`, `math`), after `> [!` (callout types) and, under the
   Obsidian dialect, after `[[` (the document's headings, from the last preview build on the
   worker, never a scan on the UI thread per keystroke, as GUI AGENTS.md requires).
4. **List and quote continuation** on Enter (`- `, `1. `, `- [ ] `, `> `), and Tab/Shift+Tab
   to indent and outdent list items; Enter on an empty item ends the list.
5. **Image drop.** A dropped PNG, JPEG or SVG on a saved document is copied into an `images/`
   folder beside it (`ReadRegularFile`, `WriteFileAtomically`, size-capped) and inserted as
   `![name](images/name.png)` at the drop's caret; the engine only shows local images inside
   the document's folder (`LocalImagePolicy`). Unsaved documents get a status message.

**Tests.** `tests/test_ui_editor.cpp` for replace and undo, continuation, popup keys; app tests
for drop and insert; the README keys table.

## 5. Native Mermaid diagrams

**Why native.** Every surveyed tool that puts Mermaid in a PDF runs a browser, and
Mermaid's own SVG keeps labels in `foreignObject`, which RayoMD's SVG converter rightly refuses
(report, *Diagrams*). A native subset is RayoMD's to own.

**Contract first.** `docs/development/native_mermaid.md`, as `native_math.md` is for math: the
supported subset, the limits, and the rule that an unsupported or malformed diagram is drawn as
the code block it is today (the same bytes), never a failed export.

**Subset.**
- **M1 flowchart:** `flowchart`/`graph` with `TD/TB/BT/LR/RL`; shapes `[]`, `()`, `([])`,
  `[[]]`, `[()]`, `(())`, `{}`, `{{}}`, `[/ /]`, `[\ \]`; edges `-->`, `---`, `-.->`, `==>`,
  `--x`, `--o`, labels `-- text -->` and `-->|text|`, chains and `&`; `%%` comments.
- **M2:** `subgraph ... end` (nested), `direction` in subgraphs, `classDef`/`class`/`style`
  limited to fill, stroke, stroke-width, color, stroke-dasharray.
- **M3 sequence:** participants and actors, messages `->`, `-->`, `->>`, `-->>`, `-x`, `-)`,
  activation, notes left/right/over, `loop`/`alt`/`else`/`opt`/`par`/`critical` boxes,
  `autonumber`.
- **M4:** `pie`, `xychart-beta` (the README uses one, `README.md:159-165`; supporting it changes
  the README's PDF, a measurement fixture: re-baseline the benchmarks in that commit).
- **M5:** `classDiagram`, `stateDiagram-v2`, `erDiagram`, `gantt`, `timeline`, `mindmap`, as
  demand shows.

**Design.**
- `src/core/mermaid.h/.cpp` (`-Os`, in `RAYOMD_CORE_SOURCES`): tokenizer and parser into a
  bounded model (caps on nodes, edges, label bytes, subgraph depth), layout, and an SVG string
  that `Internal::ConvertSvg` turns into a form XObject. One paint path for both renderers and
  their hosts, text in the document's font, the GUI preview draws forms already
  (`rayomd-gui/src/pdf/renderer.cpp:695-757`).
- Labels are measured with the same `SvgHost::TextWidth` the converter uses, so boxes fit their
  text exactly.
- Flowchart layout: layered (Sugiyama): cycles broken by DFS, layers by longest path, crossings
  reduced by a fixed number of barycentre sweeps, coordinates by a simple priority method,
  edges as polylines; subgraphs laid out as clusters. Every phase O(V+E) per sweep with fixed
  sweep counts. Sequence layout: lanes and rows, linear.
- The converter draws no `<marker>`: arrowheads are emitted as polygons. Only axis-aligned
  output and straight segments, so libm is never loaded for a diagram (`svg.cpp:62-110`).
- Colours from the theme palette (`headingColor`, `ruleColor`, `linkColor`), so a company
  theme recolours diagrams.

**Hooks.**
- Parser: a fence whose info string is `mermaid` becomes `BlockType::Diagram`, mirroring the
  `math` fence (`markdown_parser.cpp:1344, 1355-1363`); a new `BlockType` value does not grow
  `Block`. `CheckHighlighting`'s assertion that `"mermaid"` has no language
  (`tests/core_tests.cpp:2654`) moves to the diagram tests.
- Renderers: `RenderDiagram` (cold) in `RenderBlocks` (5874-5918) and both quote-child switches
  (8283-8289, 11313-11319) places the form as `RenderImage` does (scaled to the page,
  `Ensure`, PDF/UA `Figure` with the diagram's source as `/Alt`); on any failure it calls
  `RenderCode`, the bytes of today.
- `--no-diagrams` (`PdfOptions::diagrams`, default on) keeps every fence as code.
- Cancellation checks in the layout sweeps (`BuildCancelled()` through the host's `Cancelled`).

**Tests.** Parser units; layout goldens (digests of the generated SVG); PDF goldens of both
renderers; `Bounded` on 10,000 nodes, deep subgraphs, megabyte labels, dense edges; a fuzz
target `mermaid` with written seeds (no copied Mermaid examples); veraPDF UA-1. Wiki page
*Diagrams*, README *Features* with a flowchart example.

**GUI.** Diagrams appear in the preview with no GUI change; templates come with the insert
popup (step 4).

## 6. Backlog from the report

Ranked as the report ranks them; each takes the same rules.

**Engine and CLI.**
- Export options from front matter (`rayomd:` key: page size, margins, toc, numbering, page
  numbers, style, dialect); never paths or network options (theme, URL images, unsafe images),
  which stay on the command line.
- `data:` images in Markdown and HTML: export `DecodeBase64` from `svg.cpp` and take them in
  `ImageRegistry::ResolveSource` before the local policy (`tiny_pdf.cpp:3888-3899`), charged to
  `DecodeBudget` through `AddDecoded`. Today the fallback prints the whole URI when the alt text
  is empty (7846 / 10851).
- Wide tables: break cells at spaces before characters, shrink the font to a floor, else a
  landscape page for that table. Today a 25-column table cuts words mid-word.
- CJK soft line breaks: no space where a source line break falls between two CJK characters
  (`markdown_parser.cpp:1640`).
- Code line numbers and highlighted lines from fence attributes (`{.numberLines}`,
  `{hl_lines="2-4"}`).
- Captions, figure and table numbering, cross-references (pandoc-crossref syntax).
- PDF/UA formulas as `Formula` elements with their TeX as alternate text (today `--pdfua`
  shows the source); PDF/UA-2; PDF/A-2b.
- Emoji: per-glyph fallback to an installed emoji font, monochrome first.
- A diagram renderer hook (`--diagram=dot:CMD`) for Graphviz, PlantUML, D2 or Kroki, named only
  on the command line, never by a document.
- `--watch`; `--show-properties` (front matter as a key/value block); MkDocs `nav` as a book
  source; MkDocs and Docusaurus admonitions; definition lists; right-to-left text (bidi, then
  Arabic shaping; large); a container image and a CI action.

**GUI.**
- Print: the desktop portal's `Print` on Linux (the GUI has its own D-Bus client), GDI on
  Windows from the GUI's own rasterizer.
- Export a folder, several files or a book from the window (`BuildBookPdf` exists).
- Header, footer, cover and fonts edited in the app, writing a theme file.
- Named presets, remembered per document, optionally written to front matter.
- Markdown highlighting in the editor (the README lists its absence as a boundary).

**Not a fit** (report, *Not a fit or out of scope*): arbitrary CSS layout (see the CSS issue
for the bounded subset that does fit), citations and CSL, executing code blocks, PlantUML or
full Graphviz in the binary, full Mermaid parity, mobile apps, libcurl in the Linux GUI,
multi-column and per-section page styles, other output formats, complete LaTeX, a WYSIWYG
editor.

## Issues

- [#29](https://github.com/Butterski/rayomd/issues/29) Epic: CSS styling without a browser, a
  bounded stylesheet subset mapped onto RayoMD's own layout values.
- [#30](https://github.com/Butterski/rayomd/issues/30) VS Code extension on `rayomd --serve`
  (with a JSON-lines serve mode for per-request output paths and options).
