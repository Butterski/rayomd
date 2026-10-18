# Plan: note embeds, linked notes, page footnotes, long pages, print output

Five engine proposals of the wiki report
[Markdown-to-PDF Requests: Obsidian and Tools 2026-10-17](https://github.com/Butterski/rayomd/wiki/Markdown-to-PDF-Requests-Obsidian-and-Tools-2026-10-17)
(its demand numbers and sources are there), with their GUI options:

| Report # | Item | Need |
|---:|---|---|
| 1 | Note embeds: `![[Note]]` prints the note | One PDF composed of notes, as Obsidian's own export does |
| 3 | `--follow-links=N`, `--backlinks` | A note with the notes it depends on |
| 5 | Footnotes at the foot of the page | Book-style notes in long documents |
| 6 | One long page (`--page-size=210mmxauto`) | A PDF for screens, with no page breaks |
| 9 | `--black-text`, `--hide-tags` | Legible on a black-and-white printer, no working tags |

Line numbers are those of 5.1.0 (`a5a55b9`); they move, the names do not.

## Rules every item follows

The rules of `markdown_features_plan.md` (*Rules every item follows*: bytes, bounds, speed, both
renderers and PDF/UA, surfaces) hold for every item, plus one for files:

- **Files a document names.** Items 1 and 3 make RayoMD read Markdown files the document names.
  Each goes through one resolver (`Vault`, item 1): a regular file inside the root, by canonical
  path (`IsPathContainedInRoot`'s rule, `tiny_pdf.cpp:2582-2592`), on Windows with no reparse
  point on the way (`TryOpenDirect`'s rule, 2612-2630), read with `RayoMd::File::ReadAll` and its
  `FileIdentity`. Every count, depth and byte total is capped, `BuildCancelled` is asked between
  files, and past a cap the link stays a link: never a failed export. The fuzz targets already
  give builds a scratch `sourcePath` (`tests/fuzz/fuzz_public.cpp:66`, `:114`), whose seeds get
  note files.
- **Fuzz options.** Both bytes of `OptionsFrom` are taken (`fuzz_public.cpp:34-37`); a third
  byte carries the new options.

## Order

| Step | Item | Report # | Repo | Size | Release | Status (2026-10-18) |
|---:|---|---:|---|---|---|---|
| 1 | Print output | 9 | engine, GUI options | S | 5.2 | done, 714a5f6 |
| 2 | One long page | 6 | engine S, GUI preview M | M | 5.2 | done, f25dbc4; GUI strips |
| 3 | Note embeds and the vault resolver | 1 | engine, GUI options | M–L | 5.2 | done, 4e3e792, ea31d0c |
| 4 | Linked notes and backlinks | 3 | engine and CLIs, GUI export | M | 5.2 | done, with step 3 |
| 5 | Footnotes at the foot of the page | 5 | engine, GUI option | L | 5.2 | done, fefe395, c4f6013 |

Small, isolated items first. Step 4 reuses step 3's resolver. Step 5 changes page breaking in
both renderers and the structure tree; planned for 5.3, it was ready with the rest and ships in
5.2.

**Deviations, as built.** Embeds turn their lines into `<!---->` and splice each note's blocks
after the parse (`ParseWithEmbeds`), and images take their folder from a side table: a test in
`ParseMarkdownImpl` or a field in `ImageSource` moved GCC's inlining there (+0.4 % instructions on
notes.md). Embeds must start in the first column. Notes are opened with `openat2(RESOLVE_BENEATH)`
on Linux. Long pages keep their bottoms in `long_page.cpp` (a renderer member moved inlining).
Page footnotes measure each note once and replay its bytes under a translation (a second layout
cost +13 to +19 %); PDF/UA builds and notes with images lay out again. With a long page, the
notes follow the page's text. Print colours also run over a diagram's own form.

## 1. Print output (`--black-text`, `--hide-tags`)

**Now.** Only heading, link, rule and plain quote bar colours come from `ThemePalette`
(`tiny_pdf.cpp:6218-6221`). About 60 other sites in both renderers write literals or constexpr
tables:
- body text `0.08`, read on the hot paths through `kBodyRunColors` (7008, 10622);
- `kQuoteBars` for alerts and callouts (6198);
- `kMarkFill` (4987);
- table fills (6967, 10571);
- code tiles;
- the highlight table (`highlight.cpp:1934-1946`).

Diagrams start from `Mermaid::kDefaultPalette` (`mermaid.h:33`). Links print blue (`0.05 0.30 0.68`).
Obsidian `#tags` print as text: the dialect pass does not know them (`IsSyntaxByte`,
`dialect_rewrite.cpp:33-38`).

**Do.**
- `--black-text` (`PdfOptions::blackText`). A new `src/core/print_colors.{h,cpp}` (`-Os`)
  rewrites the colour operators of each finished page's content range in place, before
  `CompressPageContents` (12438, 13096).
  - Mapping:
    - `r g b rg|RG` and `v g|G` become DeviceGray of luminance Y = 0.299r + 0.587g + 0.114b.
    - Y < 0.5 becomes 0: text, links, bars and code colours turn black.
    - Lighter values keep Y: fills stay light grey.
  - The replacement is written over the same bytes and padded with spaces. Offsets, stream
    lengths and `BuildInto`'s in-place payloads (`AddStreamInPlace`, 1766-1775) do not move.
  - Its tokenizer reads the content syntax RayoMD writes:
    - literal strings with escapes (standard renderer);
    - hex strings (Unicode renderer);
    - `<< >>` property lists of `BDC`;
    - names and numbers.
    Operands of other operators pass through unchanged.
  - Overlays (page numbers, theme bands, TOC numbers) go through the same function.
  - Diagrams take a grey `kPrintPalette`. The diagram cache already keys on the palette
    (`diagram_cache.cpp:38`).
  - Raster and SVG images are left alone.
- `--hide-tags` (`PdfOptions::hideTags`, Obsidian dialect only) uses Obsidian's tag token
  ([Tags](https://help.obsidian.md/tags),
  [source](https://github.com/obsidianmd/obsidian-help/blob/master/en/Editing%20and%20formatting/Tags.md)).
  - Token: `#` at a line start or after white space, then letters (any non-ASCII letter),
    digits, `_`, `-` or `/`, with at least one non-digit. So `#1984` stays and `#y1984` goes.
  - A separate pass runs only when the flag is set, outside fences, code spans, display math and
    comments. It drops the token and one space next to it.
  - A line left blank stays as a blank line, so source lines do not move. A line of tags inside a
    paragraph therefore ends that paragraph.
  - The pair-gated SIMD pass (`FindSyntaxPair`) stays as it is.

**Decision.** A pass over finished content, not a palette threaded through the paint sites.
- A palette would touch about 60 sites inside `tiny_pdf.cpp`'s inlining budget, including the
  hot `kBodyRunColors` reads.
- The pass leaves default output byte- and instruction-identical by construction, and it covers
  paints added later.
- Its cost is one linear scan per page, in this mode only.

**Tests.**
- Property: in both renderers, over the harness corpus and the dialect corpus, the
  `--black-text` PDF equals the default PDF once colour operators and their padding are removed.
- No `rg`/`RG` left in page content.
- Fills keep their luminance.
- Image XObjects are byte-identical.
- The diagram's grey palette is used, and the cache entries of the two palettes stay apart.
- veraPDF `-f 3b` and `-f ua1` with the flag.
- Tags: the cases above, plus `a#b`, `http://x/#frag`, `# Heading`, tags in code and in
  wiki-link aliases. The flag is ignored under GitHub. `Bounded` on a megabyte of `#a #a`.

**GUI.**
- "Black text for printing" in the Output section.
- "Hide #tags" in the Document section, enabled with the Obsidian dialect.

## 2. One long page (`--page-size=WIDTHxauto`)

**Now.**
- Each renderer has one `pageW`/`pageH` (6252-6263, 9813-9822).
- Every page gets `/MediaBox [0 0 W H]` (12160-12178, 12452-12459, 13110-13117).
- Sides are capped at 144–14,400 pt (`export_options.h:12-14`, `ParsePageSize` 85-119). 14,400 is
  also PDF/A-2/3's limit (veraPDF rule 6.1.13-11).
- Every coordinate is absolute, with the origin bottom-left: content, link rectangles, outline and
  link destinations, PDF/UA BBoxes.
- `/UserUnit` is not used.
- The GUI rasterizes a whole page into one image and refuses pages taller than 16,384 px or over
  2^26 px, which then show blank (`rayomd-gui/src/pdf/renderer.cpp:1144-1203`).

**Do.**
- `--page-size=210mmxauto` (any unit; presets and `-landscape` are not combined with it):
  `PdfPageSize::autoHeight`. The nominal height, width × √2, sizes whatever is fitted to "a page".
- The renderers lay out on 14,400 pt pages. That holds about 900 body lines [INFERENCE: (14,400 −
  108) / 15.5]. A longer document continues on a second long page. Explicit page breaks
  (`\pagebreak`, HTML breaks, `--page-break-before`) start a new one.
- A `fitH` member, equal to `pageH` for fixed sizes (the same doubles, so the same bytes), takes
  `pageH`'s place in what sizes content to a page:
  - image fit (7980, 8022, 11049, 11082);
  - diagram fit (8513, 11601);
  - display math (8818, 11842);
  - `MaxMathHeight` (6520, 9979);
  - book and part title pages (8707-8719, 11756-11766).

  The cover keeps the nominal size.
- Each page records its lowest point, the cursor when the page ends. Its MediaBox becomes
  `[0 y0 W 14400]` with `y0 = max(0, lowest − margin)`, at least 144 pt tall. No coordinate
  moves.
- Page numbers and theme bands sit at `y0 + margin × 0.5`: `AddPageNumber` (12031-12048) and a
  bottom offset in `ThemePage` (`theme.cpp:203-228`).
- The source map stays as it is: it measures from the page's top edge, which does not move, and
  the GUI divides by each page's own box height (`buildworker.cpp:243-244`).

**Decision.** Move the box's origin instead of laying out again with the measured height. That
keeps one layout pass and every coordinate.

**Tests.**
- `CheckPageSize` parse cases: `xauto` with each unit; `-landscape`, preset and out-of-range
  widths refused.
- One MediaBox per page; 1,000 lines give two pages; a page break gives two cropped pages.
- Images fit the nominal height.
- Page numbers and bands lie inside the box.
- Books.
- veraPDF `-f 3b`.
- `CheckSourceMap` positions.

**GUI.**
- "Auto height" next to the custom page size.
- Pages over the raster limits render in strips of at most 2,048 px rows: `RenderPage` gets a
  vertical window, and `PreviewPages` keys entries by strip. Only the strips in view are drawn.

## 3. Note embeds (`![[Note]]`)

**Now.**
- `![[Note]]` becomes `[Note](<Note.md>)` (`Converter::Embed`, `dialect_rewrite.cpp:396-444`, then
  `WikiLink` 449-490). `![[Note#^id]]` also loses its block id.
- Obsidian's `^id` markers print as text.
- No parser translation unit reads files.
- Local images are confined to the document's folder (`LocalImagePolicy`, `tiny_pdf.cpp:2594-2709`).
- Fonts and WinAnsi transcoding are chosen from the whole input before the parse (`ChooseFonts`,
  13185-13205).
- The README says embedded notes are linked, not copied in (`README.md:302-307`).

**Do.**
- **`Vault`** in a new `src/core/vault.{h,cpp}` (`-Os`), shared with step 4.
  - Root: `--vault=DIR` (`PdfOptions::vaultRoot`), else the document's folder. It is
    canonicalized as `LocalImagePolicy::SafeRoot` does.
  - Resolution order:
    1. the path relative to the linking note;
    2. the path relative to the root;
    3. the file name under the root, preferring the fewest folders, then byte order.

    `.md` is added when there is no extension. Names compare ASCII case-insensitively, as
    `BookTargets::FileByName` (6173-6185).
  - The name index is built once per build, only when a direct path misses. It walks as
    `Batch::CollectMarkdown` does and skips hidden folders (`.obsidian`, `.trash`, `.git`).
  - Index limits: at most 20,000 files and 32 levels. Past them, lookups by name fail.
- **`ResolveEmbeds`** in a new `src/core/embeds.{h,cpp}` (`-Os`).
  - `BuildPdf` calls it under the Obsidian dialect when `sourcePath` is set and the text holds
    `![[`. Nothing is read for other documents.
  - It finds standalone note embeds at the top level: a line of only `![[target]]` outside
    fences, display math, comments and front matter, which is `Convert`'s `alone` rule without a
    `>` prefix. Images (`IsImageName`), PDFs, audio and video stay as they are.
  - It reads each target through `Vault`.
    - `#Heading` takes that heading's section, up to the next heading of its level or higher,
      fence-aware.
    - `#^id` takes the paragraph or list line that ends in ` ^id`, or the block that a `^id`
      line follows ([Obsidian: link to a block](https://github.com/obsidianmd/obsidian-help/blob/master/en/Linking%20notes%20and%20files/Internal%20links.md)).
  - It recurses into what it read:
    - depth at most 8, as nesting elsewhere;
    - a note already on the chain stays a link (cycles);
    - at most 1,000 embeds and 32 MiB of embedded text per build.
  - The result is an `EmbedSet`. Each entry holds:
    - its text, with front matter cut;
    - its folder;
    - the lines of its own embeds, mapped to child entries.
  - Footnote labels in an entry get a per-entry suffix in references and definitions, outside
    code: `[^1]` becomes `[^1~e3]`. Two notes' `[^1]` stay two notes; today the first definition
    would win (`DefineFootnotes`, `footnotes.cpp:183-201`).
- **Fonts.** `ChooseFonts`, WinAnsi transcoding and the fallback guess see the main text plus the
  entries, as `BuildBookPdf` does with its `all` (13350-13386).
- **Parser.** `ParseObsidian` gets the `EmbedSet` through a thread-local scope, as
  `DialectScope` is passed.
  - At depth 0, the line loop of `ParseMarkdownImpl` compares the line with the next embed line.
    That is one pointer test per line, and the pointer is null for every other document.
  - In that line's place it parses the entry with its own `ParseMarkdownImpl` (its own reference
    definitions, the shared `ParseBudget` and footnote parse) and splices the blocks in.
  - Each spliced block is stamped with the embed's line (`StampSourceLines`). The GUI's source
    map merges equal ranges (`sourcemap.cpp:81`), so the whole embed maps to its line.
  - This happens before `PlaceContents` and `NumberDocument`, so the table of contents and
    heading numbers include embedded headings.
  - Gate: A/B on GitHub-dialect and Obsidian documents.
- **Images.** `ImageSource::base` is the entry's folder index (0 for the document).
  `LocalImagePolicy` resolves against that folder, inside the root, as `SetChapter` does
  (2605-2609). With `--vault`, the image root is the vault.
- **Block ids.** The dialect pass strips Obsidian block ids: ` ^id` at a line end and a lone
  `^id` line.
- **Embeds as links.** `--embeds=link` (`PdfOptions::embedsAsLinks`) keeps 5.1's links. So do
  stdin input and builds without `sourcePath`.
- **Reversible profile.** It embeds the note as written, since expansion happens at parse time.
  Recovery returns the note.

**Limits.**
- Embeds inside quotes, callouts and list items stay links, because the line side table works at
  depth 0.
- Relative Markdown links, as opposed to wiki links, inside an embedded note resolve from the
  main note's folder.

**Tests.**
- Resolution order.
- Section and block extraction.
- Bounds and failures: a cycle (a ↔ b), depth 9, 1,001 embeds, 32 MiB, `../x.md`, an absolute
  path, a symlink out of the root, the name-index cap.
- Label suffixes.
- Source-map stamping; TOC and numbering include embedded headings.
- An image of an embedded note in a subfolder.
- A WinAnsi note embedding a CJK note, which must switch fonts.
- `Bounded` on a megabyte of `![[`.
- Fuzz seeds with note files.
- Byte identity: `check.sh` and the 1,871-document dialect corpus are unchanged wherever no
  embed names an existing note; documents that do embed one change, and the commit says so.
- README *Obsidian notes*, wiki *Obsidian Notes*.

**GUI.**
- The preview gets embeds already, since `sourcePath` is passed.
- Embedded files' stamps are re-checked on every preview and on focus-in, as `ApplyTheme` does
  (`buildworker.cpp:167-187`). A changed stamp rebuilds the preview.
- `[[` and `![[` completion lists note names under the document's folder (popup `WikiLinks`).
- "Embedded notes: content / link" under the dialect dropdown.

## 4. Linked notes and backlinks

**Now.**
- One note plus others is possible only as `--book a.md b.md out.pdf` (`ReadBook`,
  `book.cpp:383-428`).
- Chapters are parsed alone, and links between them resolve (`BookTargets::FileOfLink`,
  `tiny_pdf.cpp:6156-6169`).
- The only link collector is `AddLinks` (`book.cpp:93-118`, `ParseInlineSpans`).

**Do.**
- **`--follow-links[=N]`** (N from 1 to 6, default 1) makes a book.
  - The book holds the note, then the notes it links to, breadth-first to depth N, in
    first-link order, each once by `FileIdentity`.
  - Links counted: wiki links under the dialect, and relative `.md`/`.markdown` links in both
    dialects.
  - Resolution and confinement go through `Vault`.
  - Bounds: `kMaxBookBytes` and at most 1,000 notes. Past them the walk stops, and the CLI says on
    stderr how many notes it left out. Exit code 0.
- **`CollectLinkedNotes`** in `book.cpp` parses each note under its dialect and walks:
  - paragraphs, list items, quotes;
  - tables without math;
  - image links.

  It reads URLs with `ParseInlineSpans`, skips URLs with a scheme, and strips fragments.
- **Chapter titles.** A linked note that does not begin with a heading gets its file stem
  (`BeginsWithTitle`, `contents.cpp:149`), as SUMMARY.md titles are inserted.
- **`--backlinks`** (any book, `--book` too). Each chapter ends with a paragraph
  "Linked from: [A](<…>), [B](<…>)": the chapters that link to it, in chapter order.
  - The paragraph is added to the chapter's Markdown before `ParseBookFiles`, so its links resolve
    like any other book link.
  - The label is English, as "Contents" is.
- **CLIs.** Both CLIs, next to the `--book` branch (`main_cli.cpp:1019-1030`,
  `main_windows_cli.cpp:1183-1204`). `--embed-source` is refused with it, as with `--book`:
  books drop the embedded source and the source map (`tiny_pdf.cpp:13356`).

**Tests.**
- A fixture vault:
  - depths 0, 1 and 2;
  - a cycle;
  - a missing target;
  - a link out of the root;
  - one file under two names.
- Order and titles.
- Backlinks and their `/Dest` counts, as in `CheckBook`.
- 1,001 notes.
- `verify_cli.py`.

**GUI.**
- "Linked notes" (off, 1–3) and "Backlinks" in the export options. Export goes through
  `BuildBookPdf` when either is on.
- The preview stays the note itself. The status line says how many notes the export adds.

## 5. Footnotes at the foot of the page (`--footnotes=page`)

**Now.**
- `RenderWithFootnotes` draws the text, then `RenderFootnoteSection` (`tiny_pdf.cpp:5983-6025`): a
  rule, then every note at 0.85 of the body size in the flow.
- Each reference is a `MathPool` box, so lines with references take the cold paths
  (`RenderMathLines`, `RenderMathTextLines`, `RenderMathTable`). `PaintNote` (6669-6686,
  10301-10311) records its mark.
- Page breaking is one test, `Ensure`: `y − needed < margin` (7510-7512, 10096-10098).
- `margin` is both the left indent and the bottom limit, and nested blocks raise it.
- Pages are written in place, one contiguous range each. Only the last page can still grow.
- Overlays are artifacts only (`MarkArtifacts`).
- `StructTree::Begin` adds kids at the top of the open stack, and MCIDs go to the current page
  (`tagged.cpp:176-184`, `292-322`).

**Do.** `--footnotes=page` (`PdfOptions::footnotes`, `FootnotePlacement::End` by default).
1. **Measure.** Before `Render`, each note is laid out once at the foot's width. A `footMode`
   flag makes `NewPage` return at once, so nothing breaks.
   - Afterwards the content and link lists are cut back, tags are off throughout, and note marks
     are restored.
   - Height = start y − end y.
   - A note referenced only from other notes adds its height to the first note that references
     it, forming a cluster (depth ≤ 8).
2. **Reserve.** The cold paths know a line's references before its `Ensure`.
   - For notes not yet placed, the line needs its own height, plus their heights, plus 18 pt for
     the rule when the page has none yet.
   - After `Ensure(need)`, the page's `footReserve` grows by their heights.
   - `Ensure` and the other bottom tests compare with `margin + footReserve`. Those tests:
     `RenderMathTable`'s room (5710, 5758), `RenderTable` (8913, 11934), `TallTableRowSlice`
     (8975-8986, 11995-12006).
   - `footReserve` is 0 outside this mode. Gate: A/B, since `Ensure` inlines at about 60 sites.
3. **Paint.** Before a page closes, its pending notes are painted: a rule at the top of the foot
   area, then the notes in reference order, down to the page margin. This happens in `NewPage`,
   at the end of the document, and at the end of each book file before `SetFootnotes(nullptr)`.
   - A `FootScope` swaps `math`, `paragraphRuns`, the table cell buffers, `bodySize`,
     `lineHeight`, `margin` and `y` with spares and back. A page that breaks inside a paragraph
     or table row therefore resumes it unchanged.
   - In `footMode`, `NewPage` does nothing. Nested `Ensure`s, whose children are indented by
     `kNoteIndent`, cannot recurse.
4. **Too tall.** A note whose cluster does not fit with its line on an empty page (text height
   minus one line) is set after the text as today. Its reference links there, and the end section
   holds only such notes.
5. **PDF/UA.**
   - `StructTree::BeginUnder(root)` opens a Note as a kid of Document without touching the open
     stack, so an open P or Link stays open.
   - `PageBreak` splits into closing the open marked-content sequence and starting the next
     page's parent-tree entry, so a note's MCIDs land on its page.
   - Each Note follows, in reading order, the block that was open when its page ended.
   - `NoteSourceBlock`'s "began on the next page" test (5920-5937) compares with the page's
     content end before its notes.
6. **Links.** `#^N` and `#^rN` resolve from `noteMarks` as today, which now records the foot
   positions.

**Bytes.** End placement keeps today's code path. `CheckReferenceLinks`
(`parse_limits_tests.inc:99-122`) and `CheckFootnotes` (`core_tests.cpp:2521-2574`) pin it.

**Tests.** Goldens in both renderers, plus:
- a reference on a page's last line moves to the next page with its note;
- two references on one line;
- a cluster;
- a table row with references, with its header repeated;
- a heading and a quote with references;
- a note taller than a page;
- books, where each file flushes its notes;
- `CheckTaggedPdf` counts and veraPDF `-f ua1`;
- the source map;
- with `xauto` pages.

**GUI.** "Footnotes: end of document / foot of page" in the Document section.

## Releases

- **Engine 5.2.0, with all five steps** (step 5 was planned for 5.3). `VERSION`, the README
  badge, *New in 5.2* and the download commands. The release workflow does the Arch PKGBUILDs.
- **GUI 1.3.0.** `VERSION`, the PKGBUILD's `pkgver`, the README badge and download commands, and
  the submodule at the engine's bump commit.
- Each step updates `README.md` *Features*, AGENTS.md *Current Feature Surface*, the wiki pages
  (*CLI Reference*, *Native Markdown Support*, *Obsidian Notes*) and its GUI options in the same
  change.
