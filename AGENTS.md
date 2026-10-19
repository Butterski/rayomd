# RayoMD Agent Guide

Read this file before changing the project. It records the product goal,
architecture, release hygiene, and benchmark guardrails that are easy to lose
between sessions.

## Product Goal

RayoMD is a tiny native Markdown-to-PDF converter. The default value is:

- very small packaged binaries
- fast startup and conversion
- low memory use for normal documents
- no browser, LaTeX, or Pandoc dependency
- a compact cross-platform CLI; the desktop app lives in the separate rayomd-gui repository,
  which embeds the engine through `rayomd::engine`

Treat the native renderer as a fast Markdown subset, not a full Pandoc clone.

## Release Readiness Priorities

- Keep the repository root clean: source entry points, `README.md`, `LICENSE`,
  `CMakeLists.txt`, `CMakePresets.json`, `.editorconfig`, `AGENTS.md`,
  `CONTRIBUTING.md`, `VERSION`, and the main smoke document belong there.
- Keep source-coupled fixtures, specifications, and decision documents under
  docs/; keep longer user guides, dated reports, benchmark results, and research
  archives in the GitHub wiki. Release benchmark records are not kept in the
  repository.
- Keep generated build trees, generated PDFs, benchmark corpora, local binaries,
  caches, and one-off scratch files out of source.
- Prefer reproducible helper scripts under `scripts/` over ad hoc command blobs.
- Keep README claims accurate, conservative, and easy to reproduce.
- Do not advertise native mode as full CommonMark, Pandoc, LaTeX math, or
  syntax-highlight compatible unless those features are actually implemented.

## Current Feature Surface

Native PDF mode supports the core document features listed in `README.md`,
including headings, paragraphs, lists (with GitHub task lists; bullets are drawn as paths by
depth, `AppendBulletMarker`, except in quotes), block quotes and
GitHub alerts, fenced code blocks, pipe tables (whose header row repeats on every
page), rule lines, natively typeset math for a TeX subset, inline emphasis
cleanup, clickable Markdown links (also bare URLs, email addresses and shortcut
references), bookmarks and `#anchor` links from headings, HTML comments, `<br>`
and character references, document metadata from the front matter (`/Info` title,
author, subject and keywords, the catalog's `/Lang`; `FrontMatterMetadata` reads
the YAML subset), opt-in page numbers, page sizes (A4 by default, presets,
landscape and custom sizes; the renderers' `pageW`/`pageH`, never constants; one long page,
`--page-size=WIDTHxauto`, `PdfPageSize::autoHeight`: `ResolvePageSize` gives the nominal height,
width × √2, and what sizes content to a page (image, diagram and formula fits, `MaxMathHeight`,
title pages, the cover) reads it as `fitH`; a page lays out on `fullH`, `kMaxPageSide`, from its
top `pageH`, `kLongPageTop` (9,900), down to −4,500, so every coordinate stays under 10,000 and
takes `WriteFixed2`'s fast branch; what tests for room on a fresh page reads `fullH`, and the
bottom limit `textBottom` starts a margin above the page's bottom (`margin + (pageH - fullH)`,
so `margin` bit for bit on fixed sizes, where `fitH` and `fullH` are `pageH`). `NewPage` notes
each finished long page's box bottom (cold `EndLongPage`, a margin below the cursor; `PageBottoms`
is nullptr otherwise): its MediaBox, page number and footer band start there),
standalone local images (PNG, JPEG, and SVG drawn as vectors), and HTTP/HTTPS
images on Windows or curl-enabled Linux builds with fallback text.
Inline HTML (`WriteHtmlTag` in `inline_markdown.cpp`, on `html.h`'s `ScanTag`): emphasis,
code and link tags set counters in `RunWriter::html`, which only then (`html.active`) takes the
out-of-line `FlushHtml`/`CloseHtml`; `<u>`/`<ins>` and `<mark>` are `InlineRun::decor`, which
both renderers paint around a run's text (`PaintDecor`; the standard renderer's style bits 32
and 64); `<sup>`/`<sub>` are `InlineMath::Sup`/`Sub` runs that `InlineRuns::scripts` sends to
the general paths, which box them in the `MathPool` like footnote numbers (`PaintScript`), and
that table cells lay out as text. Elements dropped with their content and scripts skip to their
close tag, so each byte is read once. HTML blocks give `Block::align` (2 centre, 3 right take the
cold `RenderAlignedParagraph`/`RenderAlignedHeading`, each line moved by what it leaves; 0 and 1
keep the unchanged renderers), image sizes (`ImageSource::widthHint`/`heightHint`, applied before
the fit to the page by `ApplySizeHints`) and rows (`ImageSource::row`, `RenderImageRow`: 6 points
apart, one bottom per line, the alt texts joined by " · " when one image cannot be shown), and
`BlockType::Anchor`, a `#id` target where the next block starts (`MarkAnchor`, the renderers'
`ids`). Of `style` and `class` only page breaks are read (`PageBreaks` in `html_blocks.cpp`:
`page-break-*`/`break-*` that force one, the classes `page-break`, `pagebreak`, `new-page`),
as `BlockType::PageBreak` blocks before an element or at its close (`HtmlState::Open::breakAfter`).
`PdfOptions::pageBreakBeforeLevel` (`--page-break-before=hN`) inserts PageBreak blocks before the
top-level headings of those levels and their anchors (`BreakBeforeHeadings`, `contents.cpp`,
before `PlaceContents`); renderers break only a page with something on it.
Footnotes as on GitHub (`[^label]` and `[^label]:` definitions, also in quotes and
list items): numbered by first reference in reading order, drawn as raised link
numbers (one `MathPool` box each, so they take the cold formula paths and the fast
paragraph path stays as it was), and set after the text at 0.85 of the body size,
each note's number linking back. Only `ParseMarkdown(markdown, &footnotes)` reads
them; `footnotes.cpp` holds their numbering, kept out of the parser's translation
unit, where it moved GCC's inlining of the line classifier. A table with a reference
shows its cells as plain text, as one with a formula does. Without definitions the
PDF bytes stay as they are.
`--footnotes=page` (`PdfOptions::footnotes`, `FootnotePlacement::Page`) sets each note at the foot
of the page of its first reference. The steps are one copy for both renderers in `page_notes.cpp`
(`PageNoteCore`, at `-Os`, beside the account `PageNotes`), which sees a renderer through a
`NoteHost` (references to the fields both have, one `NoteCall` dispatch, `PageNoteLayout<R>::Call`,
for what each does its own way); `tiny_pdf.cpp` keeps only that dispatch, the line and row hooks
of the cold paths and `SetNotesAside`: every note is laid out once before the text (`Measure`, in
`footMode`: no page
breaks, the content, links, marks, images and glyph counts it made taken back) for its height and
the notes it refers to; the cold paths that paint references (`RenderMathLines`,
`RenderMathTextLines` with `EnsureNotes`, `RenderMathTable` rows) keep room below the line for its
notes not placed yet (and the 18-point rule), which then wait at the foot (`textBottom`, what
`Ensure` and the tables' bottom tests compare with: `margin` bit for bit outside this mode on fixed
sizes, a margin above a long page's bottom; nested
blocks move both with `Indent`/`RestoreMargin`, so `Ensure` keeps its size); `NewPage` (through the
`PageNotes::pageEnds` pointer: a direct call put it in a cycle with the paint paths and moved GCC's
inlining of `MarkHeading`), the end of
a document and of each book file paint them (`Paint`). The measuring layout writes into the
stash (`PageNotes::content`, kept by the thread from export to export, and `FootStash`'s links,
headings and anchors, swapped for the renderer's while it runs; one `PageNotes::Info` a note holds
its height, state and where its layout ended). A note whose blocks after its first paragraph refer
to no note (`MayCut`) is measured with its bottom at `PageNoteBase::cut`, half a page below where
it is too tall for a page: once a page would end there below it, `Break` sets `stop`, at which
`NoteHost::cancel` (the build's `g_cancel`) points meanwhile, so `BuildCancelled` ends its blocks
and a note of many pages, which the end section lays out again, is not laid out whole twice; the
half page is more than the room any layout keeps without filling it, so nothing changes in a note
that fits. `Paint` replays a note from the stash (`ReplayNote`)
under one `cm` translation, its links, marks and references moved by as much, instead
of laying it out again; only under PDF/UA and for a note whose layout changed the images is it laid
out a second time (`PageNotes::Replayed`), and only then is the renderer set aside (`FootScope`:
its shared buffers traded with `FootBuffers` by `SwapFootBuffers`, so a paragraph or table row
broken by the page goes on; the paragraph and table runs trade places with the thread's
`KeptBuffer` slots, which the renderer emptied when it started, as a second destructor of those
types moved GCC's inlining of the renderers' destructors). A note
only notes refer to (`Footnotes::referenced`) goes with the first that does, at most 8 deep; a
cluster too tall for a page with its line stays in the end section, which then holds only such
notes. Under PDF/UA a foot note is a Note of the Document opened over the open elements
(`StructTree::BeginUnder`/`EndUnder`, which keep the flow's label, table and artifact state),
painted before `PageBreak`, so its MCIDs are its page's. End placement keeps the code path and the
bytes it had.
Fenced code whose info string names a language GitHub knows (29 lexers: shell and
console, PowerShell, batch, JavaScript and TypeScript, Python, C, C++, C#, Java, Kotlin,
Go, Rust, Swift, PHP, Ruby, SQL, HTML and XML, CSS and SCSS, JSON, YAML, TOML, INI,
Dockerfile, diff) is drawn in the colours of GitHub's light theme; `--no-highlight`
(`PdfOptions::highlightCode`) and blocks in no known language keep the one-colour path
and its bytes. The lexers in `highlight.cpp` read every byte once in one forward pass;
every rule is bounded, so time stays linear for any input. Keep it that way: a new rule
that scans ahead must stop at the end of its line or mark what it scanned, never rescan.
A ```` ```mermaid ```` fence (`Block::codeLanguage == kMermaidCode`, set by the parser's
`FenceLanguage`) is drawn as its diagram: `RenderCode` asks `RenderDiagram` first, which takes it
through `ImageRegistry::AddDiagram` (`Mermaid::ToSvg` with the renderer's `SvgHost`, a theme's
accent colour, then `ConvertSvgImage`) and draws the form centred as an image, a Figure under
PDF/UA with the source as its alt text. With `--no-diagrams` (`PdfOptions::diagrams`), or for a
type, syntax or size the module refuses, the block takes the one-colour code path with the bytes
of a fence without a language; documents without such fences are unchanged. A drawn diagram is
kept for the process (`diagram_cache.cpp`, at most 16 MiB) by its renderer's fonts (their
`TtfFont::loadId`), palette and source, with the text runs its form shows: a later build, such as
the GUI's preview on every edit, takes the form and replays the runs into its own glyph sets
(`CidText`), so the PDF is the one an uncached build makes (`Mermaid::Layouts` counts the
layouts for the tests). Its containers live in that unit: inside `tiny_pdf.cpp` they changed what
GCC inlined into `PdfObjects::BuildInto`, moving `WrapAsciiInline` 144 bytes and essay.md 2 to 4 %
slower at equal instructions.
A table of contents comes at the first paragraph `[TOC]` or `[[_TOC_]]` (later ones draw
nothing), or with `--toc` (`PdfOptions::toc`) first or after a title heading; it lists
the top-level headings to `--toc-depth` levels, without a document's only level-1 heading
when that comes first and without headings of footnote references alone. The renderers
lay out its entries in the flow and link them to the headings; the builder adds the page
numbers and the dot leaders afterwards as a content stream of each table page's own,
beside a theme's header and footer, so one layout pass suffices. A TOC entry finds its
heading by the address of the heading's text, which `HeadingMark::text` keeps.
`--number-headings[=N]` (`PdfOptions::numberHeadings`) numbers the headings of the top level as
the table would list them (`NumberHeadings`, `NumberBookHeadings` in `contents.cpp`: a side table
of numbers by the heading's text address, never written into `Block::text`); the renderers draw
a numbered heading through `RenderAlignedHeading` with the number before the text, its mark
keeping the text alone so anchors and titles do not change, the table's entries carry the number
(`NumberContents`) and the bookmarks a numbered copy of the marks (`NumberedHeadings`).
Obsidian's Markdown (`--dialect=obsidian`, `PdfOptions::dialect`; `dialect_rewrite.cpp`, at `-O2`):
`BuildPdf` and `BuildBookPdf` keep the dialect for their thread (`DialectScope`), and
`ParseMarkdown` then turns Obsidian's syntax into RayoMD's before it parses
(`ObsidianToCommonMark`: one pass, every line kept where it was for the source map, fences,
code spans, formulas and the front matter left alone; each opener's search is cached per line, so
text of unclosed openers stays linear). The pass changes only lines with "[[", "==" or "%%" (or in
a comment): it jumps between those pairs (`FindSyntaxPair`, SSE2) and copies the text between
them in runs, reading the other lines only for the fences and formulas they open; within a line
it skips to the next byte that can start syntax (`NextSyntaxByte`). A document without a pair is
parsed as it is. Keep it so: a preview builds the note on every edit, and a byte-at-a-time pass
at `-Os` made README.md 32 % slower to build in this dialect, now 2 %. Highlights become
`<mark>`, comments go, wiki links become links to `note.md#heading` and so pass `Html::SafeUrl` as
any Markdown link, embeds Markdown images
or a sized `<img>`. Callouts are the parser's (`TakeCallout`): `Block::alert` 1 to 7 picks the bar
colour, `Block::text` holds the title, which the renderers draw (`RenderCallout`); GitHub alerts
keep an empty `text`. In a book `BookTargets::FileByName` resolves a link by the note's name. The
GitHub dialect never runs any of it: its bytes stay as they are. The pass also leaves out Obsidian's
block ids (` ^id` ending a line, `^id` alone): `FindSyntaxPair` takes a `^` after a blank or a line
start as a pair, so a note without one keeps the fast path.
Note embeds (`embeds.cpp`, `vault.cpp`, both `-Os`): under the dialect, with a `sourcePath` and
without `PdfOptions::embedsAsLinks` (`--embeds=link`), a line `![[note]]` from the first column at
the top level prints the note (`#heading` its section, `#^id` its block). `BuildPdf` tests
`MayEmbedNotes` (the dialect, then one `find("![[")`) and only then `BuildWithEmbeds` (cold):
`ResolveEmbeds` reads the notes through a `Vault` (root `PdfOptions::vaultRoot`, else the note's
folder, canonical; relative to the note, to the root, then by name in an index built once, at
most 20,000 entries and 32 levels; on Windows no reparse point below the root,
`DirectPathHasLink`), at most 1,000 embeds, 8 deep, 32 MiB, cycles by path and file identity,
each entry's footnote labels suffixed `~eN`; the fonts are chosen on all of the text
(`EmbedSet::all`, also for `GuessFallbackFont`). Paths are UTF-8 strings throughout (normalized in
place, `Normalize`, only when a name has a "." or empty component); on Linux a candidate is one
`openat2` beneath the root's descriptor (opened on the first note not in the cache), and a note
read costs that, an `fstat`, one `read` into the string kept and a `close`; each folder and name is
looked up once a build, and the name index is one `readdir` walk (`Walk`) into a single string
buffer, ordered by an 8-byte key of each name. What a vault read is kept for the process (`Kept` in
`vault.cpp`: notes as `shared_ptr<const std::string>`, at most `kMaxNoteCacheBytes` (64 MiB) and
4,096 notes, least recently used out first; the name indexes of the last four roots; resolved
roots), so a preview, `--serve` or a batch builds the same note again for one `stat` a note (one
`GetFileAttributesExW` on Windows): size, last write and status change to the nanosecond, device and
inode must be as they were, a note written within `SetNoteSettleTime` (2 s) of its read is never
kept, and only what `Check` accepted (beneath the root, through no link) enters; an index is kept
with the identity of every folder it read and every link's target, each checked by one `stat`.
Keep new caching of files on this rule: a file that changed, moved or became a link out of the
vault must be read again on the next build (`CheckNoteCache`). The parse does
not change for any other document: `ParseObsidian` takes the rewrite from `RewriteDocument`, which
in a build in an `EmbedScope` first turns the document's embed lines into `<!---->` (a comment ends
the block before it and gives none; the rewrite leaves it, so it turns no embed into a link), and
`ParseWithFootnotes` (footnotes.cpp) hands that build to `ParseWithEmbeds`, which splices each
entry's blocks after the parse at the first block past its line, stamped with that line
(`SpliceEmbeds`). A test for embeds in `ParseObsidian` (+0.35 % instructions on notes.md) or
`ParseMarkdownImpl`, or a field in `ImageSource`, moved what GCC inlined into `ParseMarkdownImpl`:
images of an entry take its folder from a side table by address (`EmbedImageBase`), which
`LocalImagePolicy` resolves inside the root; with a vault, local images may lie anywhere in it.
Books of linked notes (`--follow-links[=N]`, `RayoMd::Book::ReadLinkedBook` in `book.cpp`):
breadth first through the same `Vault`, at most 1,000 notes and `kMaxBookBytes`;
`PdfOptions::backlinks` makes `BuildBookPdf` append "Linked from:" paragraphs (`AddBacklinks`),
whose links resolve as any book link (`BookFileOfLink`, shared with `BookTargets`).
`PdfOptions::hideTags` (`--hide-tags`, Obsidian's dialect only; `DialectScope` keeps it for the
thread, `HiddenTags()`) drops Obsidian's `#tags` before that pass (`HideTags`, `TagHider` in
`dialect_rewrite.cpp`: a note with no possible tag, a '#' at a line's start or after a blank with
a tag's bytes after it (`NextTag`), is read once by `memchr` and left as it is; else lines read up
to the last possible tag, a line of text from the first column without further reading, only those
with a possible tag or "%%" scanned, fences, display formulas, indented code, code spans, formulas,
comments, wiki links and the front matter left alone; a tag goes with one space next to it and a
line keeps its place; the text without tags is written into `RewriteObsidian`'s `out`, which the
pair-gated pass then reads). Without the flag `RewriteObsidian` only asks `HiddenTags()`, and the
pair-gated pass is as it was.
Print output (`--black-text`, `PdfOptions::blackText`): after layout and before
`CompressPageContents`, both builders hand the page content, a theme's bands and cover and the
table of contents' overlays to `Internal::PrintColors` (`print_colors.cpp`), as `AddPageNumber`
its stream; every `rg`/`RG`/`g`/`G` becomes the grey of its luminance (black below 0.5) over the
same bytes, padded with spaces, so no offset or length moves. Diagrams take
`Mermaid::kPrintPalette` (the cache keys on the palette) and their form's colours the same pass;
images keep theirs. A new paint needs nothing: its colour operator is rewritten with the rest.
Without the flag nothing runs and the bytes stay as they are.
Books (`--book`, `BuildBookPdf`) make one PDF of the files a SUMMARY.md lists (mdBook,
GitBook), of a folder, or of the files given. Each file is parsed on its own (its link
definitions, footnotes and heading anchors are its own) and begins on a new page, so the
page of a link, a heading mark or a note mark tells its file: `BookTargets` resolves
`#fragment` within the file and `b.md#fragment` across files with no file field on the
renderers' structures. Single documents pass `book == nullptr` through the builders and
must keep their bytes; book-only work stays in `RAYOMD_COLD` code and `book.cpp`.
The files a SUMMARY.md lists or a book folder holds must lie in that folder once links and
`..` are resolved (`ChapterReader`'s root; `GetFinalPathNameByHandleW` on Windows, whose
standard library resolves no link): a book from elsewhere must not put the reader's files into
its PDF. Files named on the command line are the user's choice and are not confined.
Opt-in compression (`--compress`, `PdfOptions::compress`) writes page content, the
font program and both CMaps as FlateDecode where that makes the file smaller, with
RayoMD's own DEFLATE encoder; never the XMP metadata or an embedded source, which
the reversible profile reads uncompressed. Without it the PDF bytes stay as they
are.
Company themes (`--theme=FILE`, `PdfOptions::theme`): a TrueType or OpenType font for all
text (no fallback font then), with optional bold, italic and bold italic faces (`/F2`, `/F4`,
`/F5`; the Unicode renderer's `faces`, `ChooseFace`): with any of them `multiFace` is set,
paragraphs take the general `WrapStyled` path, and every width and paint of styled text goes
through `SpanFont`/`PaintSpanText`, so measure and paint use the same face; without them every
one of those calls is `font`/`PaintText` as before. Heading, link and accent colours (the renderers'
`headingColor`/`linkColor`/`ruleColor`/`plainQuoteBar`, from `ThemePalette`, never
literals), header and footer fields with placeholders and a logo (one overlay stream
per page, like the page numbers, whose glyphs join the subset before it is cut), and a
cover page before the first page. Without a theme the PDF bytes stay as they are.
PDF/A-3b (`--pdfa`, `PdfOptions::pdfa`) takes the Unicode renderer for all text, as
every font must be embedded: page numbers become a footer band in the document's
font, formulas are typeset in the embedded formula fonts as in any export, and without
a TrueType or OpenType font the export fails
(`PdfaFontUnavailable`) instead of falling back to the standard fonts. It adds an
sRGB output intent, XMP metadata that tells what the Info dictionary does
(`PdfaText` makes both hold the same text), a trailer `/ID` and `/F 4` on links,
and stays PDF 1.7 with an embedded source. Check changes with veraPDF
(`verapdf -f 3b`). Without it the PDF bytes stay as they are.
`PdfOptions::pdfua` (`--pdfua`) writes tagged PDF/UA-1: the fonts as with `pdfa`, formulas
as their TeX source (`MathPool::Disable`: they have no structure tags yet), and a structure
tree (`src/core/tagged.h`) that the Unicode renderer fills as it
paints. Every paint of that renderer goes through it: `PaintText` and `DrawImage` mark
content of the open element, `DrawRect`, `DrawStrokeRect` and rules mark artifacts, and
every block opens its element. A new paint call in the Unicode renderer must do the same,
or veraPDF fails with "content neither marked as Artifact nor tagged" (`verapdf -f ua1`),
and Debug builds assert. Untagged documents keep their bytes: each hook tests `tags` first,
and the tagged-only work is in `RAYOMD_COLD` functions and `tagged.cpp`.
`PdfOptions::sourceMap` (rayomd-gui keeps its editor and preview together with it) gets each
top-level block's source lines and where it is laid out (`SourceBlock`: pages counted with a
theme's cover, y down from the page's top edge). The parser stamps the lines at depth 0 only
(`Block::sourceLine`/`sourceLines`, in the padding after `align`; `StampSourceLines`), and
`RenderBlocks` notes each stamped block after drawing it (`NoteSourceBlock`, cold); inserted
blocks (the `--toc` table, a book's titles), nested ones and footnotes have no entry, and books
never fill it. The hooks only read layout state: the PDF bytes are the same with or without a
map (`CheckSourceMap`).
Native exports can opt into the `rayomd-source/1` reversible PDF profile.
Embedding is disabled by default because it exposes the complete source,
including content not visible on rendered pages. Recovery is byte-exact and
format-specific; it must never silently fall back to heuristic PDF conversion.
Keep ordinary non-reversible exports on their unchanged PDF 1.7 fast path.
The profile limits PDFs to 256 MiB and source to 10 MiB. The source cap is
based on the measured maximum exact-recovery case; reject larger inputs before
rendering to avoid excessive output and peak memory.

Important math details (contract: `docs/development/native_math.md`):

- Math is a TeX subset, not LaTeX. Unknown or malformed input degrades to readable
  output; every hard limit shows the formula's complete source. It must never
  crash, hang, or fail the export.
- Math must not change the PDF bytes of a document without math syntax;
  `CheckNoMathGolden` in `tests/core_tests.cpp` pins them. Re-pin it only for a
  deliberate plain-text output change and say why. Formula-bearing text
  takes separate wrap and paint functions (`RAYOMD_MATH_COLD`) so the plain paths
  stay untouched.
- Math is laid out on the Times and Symbol AFM metrics and painted in five embedded
  fonts (`/M1`..`/M5`, RayoMDMath, TeX Gyre Termes outlines at those advances), each
  added only when a formula painted one of its glyphs and holding only those
  (`MathGlyphUse`, `BuildMathFaceFont`). ASCII documents with math stay on the
  standard-font path.
- `src/core/math_symbols.inc`, `src/core/math_font_metrics.inc` and
  `src/core/math_glyphs.inc` are generated by `scripts/generate_math_tables.py`; never
  edit them by hand. Every code a formula can paint must have a glyph there
  (`paintable_slots` in the generator; `CheckMathLayout` checks all five faces).

Important image/link details:

- Local image paths are resolved relative to `TinyPdf::BuildOptions::sourcePath`
  when the caller provides an input file path.
- URL images are controlled by `BuildOptions::enableUrlImages`.
- Windows image support uses WinHTTP/WIC through the Win32 build. WIC sees only PNG and JPEG
  files (by their magic bytes) that RayoMD's own readers reject, once, through Microsoft's own
  decoder for that format (`CreateDecoder` with `GUID_VendorMicrosoft`, checked, metadata on
  demand); never `CreateDecoderFromStream`, which lets every installed codec parse the bytes.
- Linux URL images use libcurl when `RAYOMD_USE_CURL` is defined.
- URL images reach only public addresses (`IsUnsafeIpv4Address`/`IsUnsafeIpv6Address` in
  `access_policy.cpp`, the core tests' `CheckUrlAddressFilter`), checked on the address actually
  connected to as well (curl's open-socket callback, WinHTTP's `WINHTTP_OPTION_CONNECTION_INFO`)
  against DNS rebinding. A fetch stops at the first error or status other than 2xx/3xx, sends
  no cookies or credentials, and has a deadline: 20 s per URL, redirects included, and 60 s for
  all of a build's (`UrlFetchBudget`).
- Theme fonts and logos are local files. On Windows `ParseTheme` and the build refuse UNC,
  device and drive-relative paths and mapped network drives (`IsNetworkOrDevicePath`,
  `IsLocalThemeFile`) before anything opens them: a share would mean an SMB or WebDAV connection
  with the user's credentials on every build.
- The Windows direct local-image path (`TryOpenDirect`) checks containment by name before it
  opens anything and follows no symbolic link, junction or mount point (name-surrogate reparse
  points), in the file or a folder below the document's.
- PNGs with transparency are inflated by RayoMD's own inflater (`inflate.cpp`) and
  re-encoded by its own encoder (`flate.cpp`), on every platform; no zlib or libdeflate.
- Such a PNG of 32,768 pixels or more is kept between processes in
  `PdfOptions::imageCacheDirectory`, a file per SHA-256 of the PNG (SHA-NI where the
  CPU has it). The library default is off; both command lines use the user
  cache directory unless `--no-image-cache`; `--bench` never does. An entry holds the
  RayoMD version and, in `kMagic` of `src/core/image_cache.cpp`, the encoding's
  generation: raise it with any change to the bytes `BuildDecodedPngImage` writes (`kFastMagic`
  for `--fast-images`' mode, whose entries are `*.fast.rimg`), and compare image output between
  builds with `--no-image-cache`, or one build reads the other's streams.
- `--fast-images` (`PdfOptions::fastImages`) re-encodes such a PNG in the encoder's fast mode
  (`DeflateImageSegment(..., fast)`: one match candidate a position, incompressible data skipped
  sooner): its image streams a few percent larger, the same pixels. Without it the PDF bytes stay
  as they are; its streams are kept apart in both image caches.
- Failed images should degrade to useful fallback text instead of failing the
  whole conversion.
- An SVG image (a file or URL) is converted by `src/core/svg.cpp`, without a
  dependency, into a form XObject for each document, as its text uses the
  document's fonts; such forms never enter the cross-document image cache. Their `data:`
  images do: decoded once a process and kept by the SHA-256 of their bytes
  (`ImageRegistry::AddDecoded`), as a preview builds the same SVG on every edit. One
  that needs what the converter does not draw (HTML, scripts, style sheets it
  cannot match) or hits a limit shows its alt text. Keep every limit failing
  closed: SVG input is untrusted.
- The images of one build decode at most `kDecodeBudgetPixels` (2^30) pixels in all
  (`DecodeBudget`, one per `ImageRegistry`, so per pass of a build): a re-encoded PNG, a WIC
  decode and an SVG's `data:` image are charged before their pixels are read, and an image
  taken from a cache (the process's or the persistent one) what its decode took
  (`PdfImage::decodedPixels`), so a PDF never depends on what the caches hold. Past it an
  image is not decoded: it shows its alt text and counts in `failedImages`. A new way to
  decode pixels must charge it.
- Links are emitted as PDF annotations; keep visible text and annotation rects
  aligned when changing wrapping or text layout.
- A link's target is one `SharedUrl` (`shared_url.h`) for all its spans, words, lines and
  rectangles; the fast paths' runs hold views into their parse, which `RunLinkUrls` copies once a
  link (by view and `NextRunsParse` id). Never copy a target per piece: a 100 KB URL over 100 KB of
  link text took 580 MB (14.6 GB with styled text). Targets of `kSharedActionUrlBytes` (1 KiB) or
  more are written once, as a URI action object every annotation of the link names; shorter ones
  stay inside each annotation, so ordinary documents keep their bytes.
- Every link target goes through `Html::SafeUrl` before it can become a `/URI`: Markdown inline
  links and resolved references (`ParseInlineRuns`), linked image blocks (`ParseStandaloneImage`,
  `ParseImageAt`) and HTML `<a href>`. A refused one (`javascript:`, `vbscript:`, `data:`,
  `file:`, a drive path, a `\\host` share) keeps its text without an annotation. Bare URLs and
  `<…>` autolinks are `http(s)` or `mailto:` by construction. A new way to make a link must call
  it too. `SafeUrl` reads the scheme without the control bytes and DEL that `AppendUriLiteral`
  leaves out of the `/URI`, so what it checks is what a viewer gets (`java&#1;script:` was a
  `javascript:` link); keep the two in step when either changes.
- Headings make the PDF outline, and `#anchor` links go to them by GitHub's
  anchor rules (`HeadingTargets`) with explicit destinations; a link to a missing
  anchor is dropped. Renderers note a heading where its first line lands
  (`MarkHeading`, next to that line's `Ensure`): keep them together when
  changing heading layout. A heading keeps room for what follows it on its page
  (`HeadingKeep`; its sum over a run of headings stops past the page height, where the page
  breaks anyway, so a run is linear), and a table's header row repeats after a page break (the row
  loop goes back to row 0): keep link rectangles on the page they are drawn on.

## Architecture Map

- `include/rayomd/tiny_pdf.h`
  Public native exporter API. Keep this small and stable.

- `src/core/tiny_pdf.cpp`
  Native PDF assembly, font handling, image decoding/cache, link annotations, and the
  standard/Unicode renderers. This remains the performance-critical export facade.

- `src/core/markdown_parser.cpp`
  Internal Markdown block model and parser; keep renderer-independent document cleanup here.

- `src/core/footnotes.h` and `src/core/footnotes.cpp`
  Footnotes in the parser: reading a definition's lines, the numbering by first
  reference once the document is read, and the plain text with reference markers of
  headings and tables that waited for it (`Block::notesPending`). Built at `-Os`.

- `src/core/page_notes.h` and `src/core/page_notes.cpp`
  Footnotes at the foot of the page (`--footnotes=page`): the account the renderers keep, each
  note's height, the notes it refers to and where its measured layout ended (one `Info` a note),
  the measured layouts' content, the clusters a line's references bring, which notes wait at the
  foot of the page and which go after the text. Room for the clusters and the waiting notes is
  taken once for all notes, so placing a line's notes allocates nothing. Layout and paint stay in
  `tiny_pdf.cpp` (`PageNoteLayout`, `ReplayNote`); every walk meets a note once. Built at `-Os`.

- `src/core/highlight.h` and `src/core/highlight.cpp`
  Syntax highlighting: the language an info string names (a table made at compile
  time), the lexers that give each byte of a code block a token class, and the colours.
  Built at `-O2`: per-byte code, 1.5 times as fast as at `-Os`. The renderers split a
  line into runs of one colour (`SplitCodeRuns`) and draw them in one text object.

- `src/core/html.h`, `src/core/html.cpp` and `src/core/html_blocks.cpp`
  HTML inside Markdown: the elements RayoMD knows (`Html::Tag`: GitHub's sanitizer
  allowlist, `u`, `center`, CommonMark's block names and the elements dropped with their
  content; looked up by a 60-bit key in an open-addressed table made at compile time),
  CommonMark's raw HTML tags (`ScanTag`: at most 4,096 bytes and 64 attributes; it clears only
  the values the token had, `TagToken::seen`, so hot callers keep one token) and HTML block
  conditions, and which URLs a link or image may have (`SafeUrl`). `QuoteCache` answers each
  tag's closing quote, so text of unterminated quotes stays linear; a new scan must stay bounded
  the same way. HTML blocks: the parser classifies their first line (`LineKind::HtmlBlock`,
  the start condition in `LineInfo::level`; condition 7 does not interrupt a paragraph),
  collects them as CommonMark ends them (a view of the source when their lines are one piece of
  it) and hands them to `MapBlock` in `html_blocks.cpp`, which turns text and inline tags into
  paragraphs for the inline parser (white space collapsed, Markdown syntax escaped) and block
  elements into blocks; an element left open keeps aligning later blocks of its level
  (`HtmlState`, `ApplyAlignments`). The close tags of quotes, lists and tables are found in one
  pass per block, made at the first such element (`Containers`), never searched for once per
  open tag. `MapParagraph` turns a paragraph of only images into an Image block with a `row`,
  and one of only an `<a id>` into an Anchor block, whose id `HeadingTargets` resolves exactly.
  `html.cpp` is built at `-O2`, `html_blocks.cpp` at `-Os` (at `-O2` it is 25 KB larger for 3 %
  on documents of HTML paragraphs); checked by `tests/html_tests.inc`.

- `src/core/contents.h` and `src/core/contents.cpp`
  The table of contents: turning the marker paragraph into a `BlockType::Contents` block
  (or inserting one for `--toc`) and listing the headings it shows, also a book's, and the
  numbers of `--number-headings`. Built at `-Os`; the marker scan runs for every document, so
  it looks at a block's text length first.

- `src/core/dialect.h`, `src/core/dialect.cpp` and `src/core/dialect_rewrite.cpp`
  Obsidian's dialect: the thread's dialect (`DialectScope`, `ObsidianDialect`: `dialect.cpp`, at
  `-Os`, as every build reads it), its syntax turned into RayoMD's before the parse
  (`ObsidianToCommonMark`) and its callouts (`TakeCallout`: `dialect_rewrite.cpp`, at `-O2`, as the
  pass reads every note on every build of a preview). At `-O2` the flag's functions, which every
  build runs from `src/hot.ld`'s group, were aligned to 16 bytes and moved every hot function after
  them 32 bytes: essay.md 2.3 % slower at equal instructions. Checked by `tests/dialect_tests.inc`.

- `src/core/vault.h` and `src/core/vault.cpp`
  The notes a document embeds or links to: a root folder (`Vault`), resolution relative to the
  note, the root, then by name (an index built once, bounded), confinement by resolved path, and
  on Windows `DirectPathHasLink` (shared with `tiny_pdf.cpp`'s direct image path); the process's
  cache of notes, name indexes and resolved roots, and `ResolvedPath`/`IsWithinRoot` on UTF-8
  paths. Its paths are UTF-8 strings joined and normalized by `src/common/path.h`'s lexical
  functions; on Windows a path that comes from a directory listing stays a wide
  `RayoMd::File::Path` until it is opened (a name with an unpaired surrogate has no UTF-8 form);
  only containment tests read it as UTF-8. Built at `-Os`.

- `src/core/embeds.h` and `src/core/embeds.cpp`
  Obsidian's note embeds: reading them (`ResolveEmbeds`, bounded in count, depth, bytes read and
  scanned, lookups), the build's `EmbedScope`, and splicing their blocks into the parse
  (`ParseWithEmbeds`, `SpliceEmbeds`). Built at `-Os`; checked by `tests/embeds_tests.inc`
  (core test exits 130, embeds, and 131, linked notes and backlinks).

- `src/core/book.h` and `src/core/book.cpp`
  Books: reading `--book`'s inputs for both command lines (SUMMARY.md as mdBook and
  GitBook read it, a folder, files), a note and its linked notes for `--follow-links`
  (`ReadLinkedBook`), and parsing a book's files for `BuildBookPdf` (with `--backlinks`'
  paragraphs, `AddBacklinks`). Built at
  `-Os`, outside `tiny_pdf.cpp` and its inlining budget. A book's files total at most 128 MiB,
  what one document may have (`kMaxBookBytes`), and a SUMMARY.md that lists a file again under
  any name (a link, letter case, `x/../`) is refused: files are told apart by their identity
  (`RayoMd::File::FileIdentity`, device and inode or volume and file index), not their names.

- `src/core/print_colors.h` and `src/core/print_colors.cpp`
  `--black-text`'s rewrite of finished content (`PrintColors`): 64 bytes a step in AVX2 (two
  `pshufb` tables; with BMI, chosen once as in `png_filter.cpp`) or SSE2, a bit for each byte
  that opens or closes a string ('(' ')' '\\' '<' '>') or ends a colour operator ('g', 'G'), taken
  by the state of strings it is in (a loop for literal strings; an escape clears the next bit). An
  operator whose bytes, operands and next byte were met before becomes what they became then, one
  XOR of its last 32 bytes (`OperandMemo`: keyed by the last 8, two ways; one per thread, on the
  heap from the first use, as an outcome depends only on those bytes); only a new one is parsed
  (`Operator`, cold). `PrintColorsScalar`, byte by byte without the memo, is the reference the
  tests hold the vector scans to (`CheckRewriteScans`) and the scan of streams under 64 bytes.
  Built at `-O2`. README.md: 3.9 % more instructions with the flag (18.8 % in 5.2.1).

- `src/core/svg.h` and `src/core/svg.cpp`
  SVG to a PDF form XObject: a non-recursive XML reader, the CSS cascade for
  presentation attributes, `style` and simple selectors, paths, shapes, clips,
  linear gradients, `use`, text in the host's font and `data:` images through the
  host (`SvgHost`, implemented by `tiny_pdf.cpp` for each renderer). Built at `-Os`;
  every input-size, depth, work and output limit makes `ConvertSvg` return false.

- `src/core/mermaid.h` and `src/core/mermaid.cpp`
  Mermaid diagrams (flowcharts with subgraphs and styles, sequence diagrams, pie charts; contract:
  `docs/development/native_mermaid.md`): read, laid out (a layered layout per subgraph level with
  fixed sweep counts, lanes and rows) and written as an SVG that `ConvertSvg` draws without libm,
  labels measured with the converter's own `SvgHost::TextWidth`. Built at `-Os`; an unsupported,
  malformed or over-limit diagram makes `Mermaid::ToSvg` return false, and the caller draws the
  code block. Pie arcs are Béziers from the module's own sine; keep transcendental calls out.

- `src/core/diagram_cache.h` and `src/core/diagram_cache.cpp`
  The process's drawn diagrams for `ImageRegistry::AddDiagram`: key, bound and lock, and the
  containers whose instantiations stay out of `tiny_pdf.cpp`. Built at `-Os`.

- `src/core/cff.h` and `src/core/cff.cpp`
  OpenType fonts with PostScript outlines: the 'CFF ' table checked once when the font
  loads (INDEXes, DICTs, FDSelect, private DICTs and subroutines), each glyph's Type 2
  charstring with its subroutines inlined (`Desubroutinize`, bounded in depth, bytes and
  work), and the font programs written of it: a CID-keyed subset whose CIDs are the code
  points (FontFile3 /CIDFontType0C, no CIDToGIDMap) and simple fonts with a built-in
  encoding. Built at `-Os`; every read is bounds-checked, as font files are untrusted.

- `src/core/tagged.h` and `src/core/tagged.cpp`
  PDF/UA's structure tree: elements opened and closed as the renderer paints, marked
  content and artifacts in the page streams, lists, tables and links tracked across
  blocks and pages, and the tree's objects written into one buffer the PDF views
  (StructTreeRoot, elements, ParentTree, IDTree). Built at `-Os`; elements hold no
  heap memory of their own.

- `src/core/rayomd_pdf_source.h` and `src/core/rayomd_pdf_source.cpp`
  Bounded reversible-profile metadata, SHA-256 integrity (SHA-NI where the CPU has it,
  checked against the portable code by the core tests), hostile-input inspection, and
  byte-exact source recovery. Keep this limited to RayoMD's exact classic-xref profile
  rather than growing a general PDF parser.

- `src/core/image_cache.h` and `src/core/image_cache.cpp`
  The persistent cache of re-encoded PNGs: entry format, checks, atomic writes and
  least-recently-used trimming. Cold code, kept out of `tiny_pdf.cpp`, whose inline
  budget it would otherwise take from hot calls.

- `src/core/inline_markdown.cpp`
  Renderer-neutral inline span parsing for emphasis, code, images, links, and math
  delimiters. Both renderers consume this model so visible text and link annotations
  cannot drift. `ParseInlineRuns` is the parser (flat runs for the hot paths);
  `ParseInlineSpans` is the same result as strings, for tests and cold paths; the spans of one
  link share its target (`SharedUrl`, `shared_url.h`), never a copy each. The parser copies text
  up to the next byte that may start syntax (`FindStops`: AVX2 nibble tables where the CPU has them,
  checked against `kByteClasses` by a static_assert; else `FindByteClass`'s SSE2): a new syntax byte
  goes into `kByteInlineSyntax`, and the tables follow.

- `src/core/math_parser.h` and `src/core/math_parser.cpp`
  Tokenizer, parser, error recovery, and limits for the TeX math subset; produces a
  bounded node arena. Uses the generated `src/core/math_symbols.inc`.

- `src/core/math_layout.h` and `src/core/math_layout.cpp`
  Renderer-neutral TeX-like box layout, draw list, PDF operator emission, and the
  embedded formula fonts (`BuildMathFaceFont`). Uses the generated
  `src/core/math_font_metrics.inc` and `src/core/math_glyphs.inc`. Cold code,
  compiled with `-Os`. Also exports `kStandardWordAdvances`, the exact AFM widths the
  standard-font renderer wraps and paints with.

- `scripts/generate_math_tables.py`
  Regenerates the math tables from the Adobe Core 14 AFM files and, with `--glyph-dir`,
  the formula glyphs from TeX Gyre Termes (fontTools; inputs not stored in the
  repository, pinned by SHA-256). TeX Gyre's GUST Font License is in
  `third_party/tex-gyre/`.

- `src/core/export_options.cpp` and `src/common/text_utils.cpp`
  Shared typed style/margin conversion, CLI option parsing primitives (also theme
  files, `ParseTheme`), and non-public text helpers. `export_options.cpp` is built at
  `-Os`: it runs once per run.

- `src/core/access_policy.h` and `src/core/access_policy.cpp`
  What builds may reach: the address classes URL images may not connect to, and Windows theme
  paths that are no local file. Pure where it can be, so the core tests check it on every
  platform. Built at `-Os`.

- `src/core/png_filter.cpp`
  PNG row filters: undoing them when a PNG is decoded, choosing and applying them when
  its pixels are re-encoded for a PDF. Choosing and applying in AVX2 where the CPU has it
  (GCC/Clang `target("avx2")`, chosen once), else SSE2 on x86-64, with the scalar
  functions kept as the reference and the fallback; core check 99 compares the dispatched,
  the SSE2 and the scalar code with the textbook filters. Built at `-O2`: a row's last bytes take
  its last full vector step again (the costs of what the step before counted masked off,
  `kTailMask`), so scalar code runs only for the first pixel and rows shorter than a step; at
  `-O3` GCC vectorized those scalar loops into 45 KB of code, now 9 KB, for 3.5 % fewer
  instructions on a 1024x1024 RGBA PNG.

- `src/core/word_bounds.h` and `src/core/word_bounds.cpp`
  The standard renderer's word measuring (`MeasureWords`): every word's first byte and end as
  positions (a 64-byte step's white-space bits, their edges, flattened a mask byte at a time from a
  table) and the prefix sums of the AFM advances (a byte at a time: AVX2's gather costs more cycles
  on Zen 2, see performance.md), so a word's width is
  `prefix[end] - prefix[begin]`, the same integer as summing it byte by byte. AVX2 where the CPU has
  it (asked once), else SSE2, else bytes; texts under 16 bytes always bytes. `BreakAsciiWords` and
  `WrapAsciiInline` in `tiny_pdf.cpp` wrap from them in the thread's `WrapScratch` (page footnotes
  trade it in `SwapFootBuffers`); texts under `kShortAsciiText` (16) bytes, most table cells, are
  read a byte at a time there, as the pass costs more than it saves. `MeasureWideWords` does the
  same for the Unicode renderer's UTF-16 text (widths gathered from `TtfFont::widthCache`, which has
  an entry more for it; a surrogate or an uncached width a unit at a time through the font), whose
  wrapping (`BreakWideWords`, `PlaceStyledWords`) decides in integer units where they are farther
  from the width than the double sums' rounding (`WrapUnits`) and lays a paragraph out again in
  doubles otherwise: a change to how widths are summed or compared must keep both paths. Core check
  133 compares the dispatched, SSE2 and portable code with byte and unit loops. `SplitColorRuns`
  splits a line of highlighted code into runs of one colour sixteen characters a step (AVX2, else
  the loop `SplitColorRunsScalar`) for both renderers' `PaintCodeLine`; core check 134.

- `src/common/file_io.cpp`
  Whole-file reads and writes through C stdio (`RayoMd::File`), wide paths on Windows. On POSIX,
  `WriteAll` overwrites an existing file in place and cuts it to size (no `O_TRUNC`): truncating
  an ext4 file to zero made its close start writeback, 0.1 ms per export. Keep it so; the cost
  is that a crash can leave old and new bytes mixed.

- `src/common/path.h` and `src/common/path.cpp`
  The file-system layer in place of the C++17 filesystem library, which the engine and both
  command lines no longer use (statically linked on MinGW it pulled the locale facets and file
  streams: 779 KB of rayomd.exe; 65 KB on Linux). `RayoMd::File::Path` is the native string
  (`std::wstring` on Windows, `std::string` elsewhere); the lexical functions (`AppendPath`,
  `ParentOf`, `FileNameOf`, `StemOf`, `ExtensionOf`, `ReplaceExtension`, `Normalize`,
  `RelativePath`, `IsAbsolute`, `RelativePart`, `RootLength`) take either kind and give what
  libstdc++'s path members give on Linux, with Windows' separators and roots there (a share's
  `\\server` is a root name, which MinGW's libstdc++ got wrong); the operations (`StatusOf`,
  `CreateDirectories`, `Remove`, `Rename`, `SetWriteTimeNow`, `SetOwnerOnly`, `Absolute`,
  `Resolved`, `TempDirectory`, `ForEachEntry`, `Walk`) answer as the library's did, `Walk` in
  `recursive_directory_iterator`'s order with `skip_permission_denied`, never entering a link.
  Core check 135 (`tests/path_tests.inc`) holds both to the library. Built at `-Os`. Keep
  `<filesystem>` out of `src/` and `include/` (tests may use it).

- `src/common/exact_math.h` and `src/common/exact_math.cpp`
  `ceil`, `floor`, `trunc`, `lround`, `llround`, `sqrt` and `fmod` without libm, bit for bit as
  glibc's (the core tests compare them), so the dynamic Linux build needs no `libm.so`. Use them,
  never `<cmath>`'s or other libm calls, in RayoMD's code: one direct call puts libm back into
  every process (`readelf -d build/linux/rayomd` must list libc only). `svg.cpp`'s `atan2`,
  `sincos` and `tan` are loaded from libm.so.6 on first use; a new transcendental function goes
  there too.

- `src/common/compiler.h`
  The inlining hints every translation unit uses: `RAYOMD_HOT_INLINE`, `RAYOMD_HOT_LAMBDA`,
  `RAYOMD_NOINLINE`, `RAYOMD_COLD` and `RAYOMD_SHARED`. Define them here only, never per file.

- `src/hot.ld` and `scripts/hot_sections.py`
  The order in which GNU ld places the code fresh exports run (Release, Linux, dynamic and
  static). Generated, never edited by hand; regenerate with the script (its docstring has the
  commands) after larger changes to the code every export runs. Stale names are harmless.

- `src/core/inflate.h` and `src/core/inflate.cpp`
  RayoMD's zlib/DEFLATE decoder (`ZlibReader`): resumable output a piece at a time
  through a window, two-level decode tables, a fast loop that looks up the next code
  before its refill. It accepts exactly what zlib's `uncompress()` accepts for an output
  of known size; the core tests hold it to libdeflate on mutated streams.

- `src/core/flate.h` and `src/core/flate.cpp`
  The DEFLATE/zlib encoder behind `--compress` and every re-encoded image: greedy
  matching over two hash tables, per-block stored/fixed/dynamic choice by exact bit
  count, SSE2 or AVX2 Adler-32. Image rows go through `DeflateImageSegment` (the 8-byte table
  only, segments of `ImageSegmentSize` with `kImageSegmentHistory` bytes of history,
  optionally waiting for input another thread is still writing); `ZlibCompressImage` is
  the same stream made on one thread. Output depends only on the input. Its tables live
  per thread and are never cleared between streams. Built at `-O2`: within 1 % of `-O3`
  at half the code.

- `src/core/image_encoder.h` and `src/core/image_encoder.cpp`
  `ImagePlanesEncoder`: the colour and alpha planes of a re-encoded PNG compressed in
  segments while they are decoded, by a process-wide pool of up to three helper threads
  and the decoding thread; the bytes do not depend on how many threads take part.

- `src/core/theme.h` and `src/core/theme.cpp`
  The layout of a theme's header, footer and cover page (placeholders, ellipses,
  wrapping) on UTF-8 text, behind `ThemeTextFont`, which each renderer implements in
  `tiny_pdf.cpp`. Built at `-Os`, and kept out of `tiny_pdf.cpp` on purpose (see the
  inlining guardrail below). A field is expanded once per build and its text stops at
  `FieldBytes` of its line (more than normal text can show there); text too wide is cut by a
  search over its character ends, not a character at a time.

- `src/core/long_page.h` and `src/core/long_page.cpp`
  One long page (`PdfPageSize::autoHeight`): `kLongPageTop`, the box bottoms of a build's pages,
  kept for the thread so a renderer holds only a pointer (null for fixed sizes), each a margin
  below where its layout ended, and the MediaBox from there to the top. Built at `-Os`; inside
  `tiny_pdf.cpp`, the vector's code moved the renderers' constructors out of line and widened
  `ResolvePageSize`. The renderers' `fitH` and `fullH` have no default value: one store more in
  `StandardRenderer`'s constructor took it out of `BuildStandardPdfBytes` (+3.3 KB).

- `src/core/pdfa.h` and `src/core/pdfa.cpp`
  What PDF/A-3b adds: the 480-byte sRGB ICC profile (a v4.2 display profile written
  as data, SHA-256 in the source), the XMP packet with the PDF/A identification and,
  with an embedded source, the reversible profile's properties and their extension
  schema, and the file identifier, a 128-bit hash of the body. Built at `-Os`.

- `src/common/batch_report.cpp`
  What both command lines share for batch export: the JSON Lines report (one
  line per document, written by the worker that finished it), the up-to-date
  check behind `--skip-unchanged` (PDF newer than its Markdown and ending in
  `%%EOF`) and the recursive collection behind `--recursive` (sorted by name as indices: a sort
  of the files themselves, two paths each, was 20 KB of code).

- `src/cli/main_cli.cpp`
  Portable CLI entry point for Linux and non-GUI workflows. Supports single
  export, stdin Markdown export, folder batch, stdin batch, warm serve mode,
  books, and benchmarks. Built at `-Os` with `batch_report.cpp` and the Windows CLI: the glue
  runs once a run (`--bench` and `--serve` only call the engine in their loops).

- `src/cli/terminate.cpp`
  Both command lines' terminate handler in place of libstdc++'s (whose demangler is 61 KB):
  the same message with the exception's type as the ABI names it (`St9bad_alloc`), then
  `abort()`. Not part of the engine library.

- `src/cli/no_unwind.cpp` and `src/cli/no_unwind.ld`
  The dynamic Linux release CLI's exception runtime: `__cxa_throw` prints the terminate
  message and aborts, so libgcc's unwinder, the personality routine and the emergency exception
  pool (a 73 KB allocation at every start) are never linked, and the linker script drops the
  unwind and exception tables nothing reads then (58 KB in all). The nothrow `operator new`
  is its own (malloc and the new-handler, null when both fail; libstdc++'s caught a throw).
  Release builds only, as only they compile without exceptions; not the static build, whose
  libc.a links the unwinder for thread cancellation, nor the engine library.

- `src/win32/main_windows_cli.cpp`
  The Windows CLI: wide paths, `CommandLineToArgvW`, a console program. Its modes mirror
  `main_cli.cpp`; change both. A MinGW cross-build runs under Wine for local checks of the
  CLI glue.

- `src/win32/delay_load.cpp`, `delay_import_pointers.cpp`, `delay_image_load.cpp` and
  `delay_image_import_pointers.cpp`
  System DLLs loaded on first use in GNU and Clang builds: `delay_load.cpp` for
  `CommandLineToArgvW`; `delay_image_*.cpp` for the engine's own Windows imports (OLE,
  Winsock, WinHTTP), which are also linked into `rayomd_engine`.

- `src/win32/rayomd.rc`
  Windows manifest and app icon resources. Keep resource changes localized here.

- `CMakeLists.txt`
  Cross-platform build. Windows and non-Windows builds produce `rayomd`
  (`rayomd.exe` on Windows).
  `rayomd_engine` (alias `rayomd::engine`) is the exporter as a static library for other
  front ends: not built by default, compiled from the same core sources a second time so the
  CLI's link order and `src/hot.ld` placement stay, with `include` and `src` as public include
  directories. rayomd-gui adds this directory with `add_subdirectory`; `tests/engine_consumer`
  builds a program against it the same way. `RAYOMD_TOP_LEVEL` (this directory is the
  top-level project) gates the tests, and `RAYOMD_BUILD_CLI` (default: `RAYOMD_TOP_LEVEL`)
  the command-line executable.

- `VERSION`
  Single source for the release version. CMake compiles it into both command-line
  entry points; update it only for release/versioning work.

- `CONTRIBUTING.md`
  Short contributor guide with project priorities, build/verify commands,
  performance expectations, and versioning rules.

- `third_party/libdeflate/`
  libdeflate v1.26's decompressor, the files it needs and unchanged (MIT): the core
  tests' independent reference for RayoMD's inflater and encoder. No executable links
  it (`EXCLUDE_FROM_ALL`). Its compressor is not vendored.

- `tester.md`
  Main hand-written smoke/regression document. It covers Unicode text, inline
  styles, code, math, tables, nested lists, block quotes, links, local images,
  remote images, and failed image fallbacks.

- tests/verify_cli.py
  Cross-platform black-box correctness verification for an already-built binary.
  Keep building and timing outside this verifier.

- tests/core_tests.cpp
  Unit and PDF-level tests linked against the core sources. The math checks live in
  `tests/math_markdown_tests.inc`, `tests/math_parser_tests.inc`,
  `tests/math_layout_tests.inc`, and `tests/no_math_golden.inc`, which it includes;
  `tests/inline_lookahead_tests.inc` holds the checks of the inline parser's
  look-ahead tables, `tests/parse_limits_tests.inc` the parser's limits on hostile input
  (`CheckParseLimits`) and the golden digests of what they must leave alone, and
  `tests/hardening_tests.inc` those of hostile and abandoned builds (cancellation, files that
  are no regular files, fonts changed after loading, image containment, cache permissions, the
  decode budget and the SVG `data:` image cache); `tests/dialect_tests.inc` checks Obsidian's
  dialect, `tests/mermaid_tests.inc` the Mermaid diagrams (syntax, refusals, layout
  invariants, golden SVG digests, conversion, limits), `tests/long_page_tests.inc` the long
  pages of `--page-size=WIDTHxauto` (boxes, fits, page numbers and bands, books, the source map),
  `tests/page_notes_tests.inc` footnotes at the foot of the page in both renderers (each note
  on its reference's page, by the links' destinations; tables, books, PDF/UA, the source map),
  and `tests/print_tests.inc` print output (`--black-text` against the default PDF in both
  renderers, `--hide-tags`).

- `tests/fuzz/`
  libFuzzer targets of the engine's untrusted-input entry points (`fuzz_targets.h`): Markdown
  through `BuildPdf` (the first two bytes choose the options, the third, the text's first, more), `PdfSource::Inspect`, `ParseTheme`
  and a build with its theme, TrueType and OpenType fonts loaded and subset as a theme's font,
  bare CFF, image decoding, SVG through both hosts, and Mermaid diagrams through `Mermaid::ToSvg`
  and `ConvertSvg`. `fuzz_internal.cpp` compiles `tiny_pdf.cpp` inside itself to reach its internal names (and reads fonts into memory instead of
  mapping them, so ASan sees overreads); each target runs on a 2 MiB stack. `-DRAYOMD_FUZZ=ON` with
  clang builds `rayomd-fuzz-<target>` (ASan, UBSan); `tests/fuzz/run.sh BUILD SECONDS` runs them on
  the corpus `seeds.py collect` makes (-timeout=10, -rss_limit_mb=2048). Every test build on Linux
  builds `rayomd-fuzz-replay`, the ctest entry that runs `tests/fuzz/regressions/<target>/` and
  `tests/fuzz/seeds/<target>/` through the same targets without libFuzzer; a `.rep` regression
  describes a large input in a few lines (`replay_main.cpp`). Add each input a fuzzer finds there,
  minimized, with the fix. The seeds are synthetic (`seeds.py generate`: no third-party font data).

- tools/benchmark.py
  Maintained entry point for run, A/B (warm engine and fresh process),
  competitor, and reversible-profile performance workflows. Focused
  implementations remain under scripts/.
- `.github/workflows/`
  GitHub Actions for Linux/Windows CI, repository hygiene, fuzzing, and CodeQL. Keep
  README badges aligned with workflow filenames when adding or renaming checks.

- docs/assets/branding/ and docs/assets/demo/
  Project branding sources and demo media, kept separate.

- `docs/development/`
  Source-coupled performance workflow, format profiles, the native math and Mermaid contracts,
  and architecture decisions.

- [GitHub wiki](https://github.com/Butterski/rayomd/wiki)
  User guides, dated benchmark reports, and optimization research. Wiki history
  is context, not a mandate to keep old experiments alive.

## Build And Verify

Windows release build:

```sh
cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release
```

Linux release build:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --config Release
```

The default Linux build intentionally avoids a libcurl runtime dependency. Use
`-DRAYOMD_USE_CURL=ON` only when producing a Linux build that should fetch
HTTP/HTTPS images and can target a known distro/libcurl baseline. Release builds link
libstdc++ and libgcc statically and do without libm (`exact_math.h`), so the binary needs only libc.
`-DRAYOMD_STATIC=ON` links glibc in as well: the static release asset, about
0.95 MB larger and 0.3 ms faster to start, with no runtime dependency. It is linked
`-static-pie`, position-independent like the dynamic build so the kernel loads it at a random
address (up to 0.04 ms of a fresh start more than plain `-static`'s fixed-address ET_EXEC). Not with
`-static-libgcc`/`-static-libstdc++`, after which GCC 13 links libc dynamically under
`-static-pie`; CI checks the result with `readelf` (no INTERP or NEEDED, Type DYN), as `ldd`
calls a static-pie "statically linked" and succeeds.

CMake 3.21 or newer can use the presets of `CMakePresets.json` instead: `release`
(`build/linux`), `release-static`, `release-curl`, `asan` (AddressSanitizer and
UBSan), `tsan` and, on Windows, `windows-mingw`; each writes `compile_commands.json`.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Cross-platform CLI verification (after building):

```sh
python3 tests/verify_cli.py --binary build/linux/rayomd
python tests/verify_cli.py --binary build/windows/rayomd.exe
```

Native benchmark examples:

```sh
build/linux/rayomd --bench tester.md benchmark-output/manual 1000 modern normal
build/windows/rayomd.exe --bench tester.md benchmark-output/manual 1000 modern normal
```

Performance watcher examples:

```sh
python tools/benchmark.py run -- --binary build/windows/rayomd.exe --platform windows --suite watch --label local
python3 tools/benchmark.py run -- --binary build/linux/rayomd --platform linux-wsl --suite watch --label local
```

When performance is part of the task, record before/after numbers. Do not keep a
change that improves one tiny case but clearly regresses larger documents,
batching, or memory without calling out the tradeoff.

GitHub CI entry points:

- `.github/workflows/ci.yml`
  Builds and smokes the Linux native CLI plus the Windows MinGW CLI.

- `.github/workflows/hygiene.yml`
  Checks release files, script syntax, local README targets, and generated
  output exclusions.

- `.github/workflows/codeql.yml`
  Runs C++ CodeQL analysis on the Linux native build path.

- `.github/workflows/fuzz.yml`
  Builds the libFuzzer targets with clang on Arch, replays the regressions and seeds under ASan
  and UBSan, and runs each target 60 s on the seed corpus; any crash, timeout or OOM fails it.

- `.github/workflows/release.yml`
  Packages default Linux, curl-enabled Linux, and Windows artifacts for matching version
  bumps, `v<VERSION>` tags, and manual dispatch; keep `VERSION` and package contents aligned.

## Benchmark And Documentation Rules

- `README.md` describes every user-visible feature in its Features section, each with a
  short, working example; keep that section in step when a feature is added or changes.
  Complete flag tables, exit codes, build variants, API details and benchmark methodology
  stay in the GitHub wiki.
- When user-visible behavior changes, update the relevant wiki page and the README's
  Features section.
- Update `VERSION` and the README version badge together when preparing a
  release.
- Keep benchmark claims dated and scoped.
- Distinguish warm `--bench` timings from end-to-end export and batch timings.
- Mention Linux storage location when reporting WSL/Linux batch numbers.
- Keep raw generated reports under ignored `benchmark-output/`, not source.
- The wiki commercial benchmark campaign is archival
  marketing-safe wording and caveats. Do not copy raw headline numbers without
  the caveats about synthetic corpora, warm `--bench` versus end-to-end I/O,
  and Linux storage location.

## Performance Guardrails

- Keep the default native path dependency-light.
- Linux release builds link the C++ runtime statically with `--gc-sections`: a process
  starts 0.6 ms sooner and warm builds run 5 % faster than with `libstdc++.so`. Keep
  iostreams (`<iostream>`, `<fstream>`, `<sstream>`) out of the CLI and the core: they
  pull about 460 KB of locale and stream code into that build. Use `RayoMd::File` and
  C stdio.
- Release builds compile without exceptions and RTTI on Windows and Linux, on Linux also without
  unwind tables (221 KB of the 1.74 MB binary): never write `try`, `catch`, `throw`,
  `dynamic_cast` or `typeid`. An
  exception the C++ runtime throws (`std::bad_alloc`) ends the process, as it did before: in the
  dynamic Linux release at the throw, without unwinding (`src/cli/no_unwind.cpp`), elsewhere
  through the command lines' own terminate handler (`src/cli/terminate.cpp`, the only
  `-fexceptions` file; libstdc++'s links the C++ demangler, 61 KB). A call-graph profile with
  `perf --call-graph=dwarf` needs a build with `-fasynchronous-unwind-tables`, and without
  `src/cli/no_unwind.ld`, which drops the unwind tables of the dynamic release.
- Fonts are mapped read-only (`FileBytes`); read font data in place instead of copying
  the file.
- `TtfFont` is allocated by its own `operator new` (calloc) or lives in static storage:
  its 128 KB width cache relies on starting zeroed. Never put one on the stack. The cache
  is read by code point (`CodepointWidth`) and by CID (`WidthForCid`); both must keep the
  rule of `CidForCodepoint`.
- A PNG with transparency is decoded a row at a time: `ZlibReader` hands out each row
  from a window of the last 32 KB and a chunk, and the filtered colour and alpha rows go
  straight into `ImagePlanesEncoder`'s segment buffers, which are reused once compressed,
  so no plane is held whole. The encoder, its segment schedule and history, and the filter
  heuristic all decide the image bytes; change them only on purpose. A segment's bytes are
  published with release order after they are written; on a decode error the encoder's
  destructor abandons the planes and waits for its helpers. From `kImageThreadPixels`,
  helpers join in for each usable CPU (the affinity mask) that no other decode of the
  process holds, within the decode's share of the pool's three, so a busy batch asks for
  none. The first encoders of a process carve their buffers from 2 MB pages
  (`MADV_HUGEPAGE` on Linux), which saves most of a fresh process's page faults.
- `WriteFixed2` may overwrite all `kFixed2MaxChars` bytes from where it starts (an operand under
  10,000 writes ".dd" after its digits whether it has decimals or not), within the room every
  caller must leave; never call it where bytes after the number must survive.
- Avoid adding large runtimes, browser engines, TeX stacks, or heavy document
  libraries to the native package.
- Reuse output buffers in batch/server paths where practical.
- A per-thread object with a destructor is `RAYOMD_THREAD_OBJECT(Type, name)`
  (`src/common/thread_object.h`), never a plain `static thread_local`: MinGW's emulated TLS frees
  a thread's storage before the C++ thread_local destructors run (GCC PR 80816), which crashed
  every batch worker and build thread as it ended. Elsewhere the macro is the same `static
  thread_local`; on MinGW the object lives on the heap and a fiber-local storage callback destroys
  it. Trivially destructible thread_locals (flags, pointers) need nothing. Core check 136.
- Be careful with per-line/per-span heap allocation in `tiny_pdf.cpp`.
- Page content is rendered straight into the caller's output buffer and
  `PdfObjects::BuildInto` assembles the file in place around it. Do not go back
  to one string per page: allocating and freeing those buffers cost 7-15 % of a
  warm build on Windows.
- Paragraph text is parsed and wrapped as flat runs (`InlineRuns`, `AsciiRuns`,
  `StyledRuns`): one text buffer per paragraph, reused from paragraph to
  paragraph. Keep a string or vector per span, word, or line off these paths.
- Every look-ahead of the inline parser goes through `InlineScanner`, which
  answers from tables once a paragraph has spent its byte budget. A new scan
  that runs "to the end for every opener" must go there too, or text with
  thousands of unterminated openers becomes quadratic again. Matchers outside it
  (bare URLs, shortcut references) must fail in constant time or never rescan
  text: `CheckInlineLookahead` holds a megabyte of `http://(` and 120,000 `[`.
- What the parser adds to a document beyond its text is bounded for the whole document by
  `ParseBudget` (`inline_markdown.h`), which `ParseMarkdown` keeps for its thread and every
  nested level and footnote shares, so nesting cannot multiply it: the destination bytes resolved
  references copy in (twice the document and 1 MiB; a definition's destination has at most
  `kMaxReferenceDestinationBytes`) and the empty cells that complete short table rows (2^19, as
  in cmark-gfm; a table header has at most `kMaxTableColumns` cells, and a data row is split up
  to its header's cells only, so a line of pipes makes no more; HTML tables take theirs from the
  same budget, `TakePaddedCells`, and one past it is read as text). Past a budget the input
  stays text. A new expansion that copies or pads must draw on it. Rewrites of text go into a new
  buffer in one pass (`NormalizeSymbols`), never `std::string::replace` in a loop.
- Prefer measured changes over plausible micro-optimizations.
- `src/core/tiny_pdf.cpp` sits at GCC's `-O3` inline-unit-growth limit, so any edit
  can move which hot calls get inlined. Profile the hot paths after a change; mark
  hot helpers `RAYOMD_HOT_INLINE` and hot lambdas `RAYOMD_HOT_LAMBDA`, and keep rare
  paths in `RAYOMD_COLD` functions that do not call small hot helpers directly.
  `TailWriter::Lit` and `Bytes` are always inlined: left to GCC, a call for each
  one-byte separator cost 1.6 % of a warm build; so is `PdfObjects::WriteTail`'s `text` (page
  footnotes' code had left one more of its calls out of line). A new cold feature goes into a
  translation unit of its own behind a small interface, as `theme.cpp` does: inside
  `tiny_pdf.cpp`, the theme layout moved `std::string::push_back` and appends of the
  link annotations out of line (+2.6 % instructions on `baseline.md`). Write output
  that grows by pieces through `TailWriter`, not per-byte `push_back`. A `TailWriter` writes
  past the string's size until it ends: nothing else may append to that string meanwhile. Grow
  a string that is then written whole with `ResizeForOverwrite`, not `resize` (which fills it).
- Keep image caches bounded. Image support can dominate memory on large or many
  remote images.
- Linux benchmarks are much faster on native/ext4 storage than on `/mnt/*` WSL
  mounts. State the storage location when reporting Linux benchmark numbers.

## Correctness Guardrails

- Native mode should fail gracefully. Missing images, unsupported image formats,
  and unavailable URLs should produce fallback text, not crash.
- Keep ASCII and Latin documents on the standard-font path when possible. A document whose
  characters all have a WinAnsiEncoding code is transcoded once in `BuildPdf`
  (`TranscodeToWinAnsi`); link targets, image paths and formula sources go back to UTF-8
  where they leave the renderer (`WinAnsiToUtf8`).
- Preserve Unicode output by loading/subsetting a system font when needed, TrueType or
  OpenType/CFF (`RAYOMD_FONT`, then known per-OS paths, then a bounded scan of the font
  directories). Without any font, the document degrades to the standard fonts
  and `BuildResult::missingCharacters` counts what they could not show; never
  fail the export for a missing font.
- A document the system font lacks characters of is laid out in it only up to the first block
  that drew one (`Renderer::ProbeFallback`); the fallback font `GuessFallbackFont` picks from the
  source then draws it, and `NeedsRebuild` keeps that build only when the system font's choice
  could not differ (`kLayoutChars`, `FallbackChoice::stable`). Anything new that layout paints in
  the document's font besides the text (numbers, marks, substitutes) must be in `kLayoutChars`,
  or set a flag that makes the guess unstable, as formulas (`MathPool::LaidOut`) and SVG text
  (`ImageRegistry::SvgAttempted`) do; a theme's bands and cover, PDF/A and PDF/UA page numbers
  and URL images turn the probe off (`FallbackGuessAllowed`). Compare such changes on CJK
  documents in both fallback fonts (`docs/development/performance.md`, *Comparing whole PDFs*).
- Keep PDF syntax valid: object ids, xrefs, page resources, image XObjects, and
  link annotations must remain consistent.
- `BuildOptions::sourcePath` matters for relative images; pass it from every
  file-based caller.
- A caller abandons a build by setting `PdfOptions::cancel` (rayomd-gui does, for previews no
  longer wanted): `BuildPdf` and `BuildBookPdf` then return `BuildError::Cancelled` without a
  PDF. `BuildCancelled()` in `tiny_pdf.cpp` reads the flag of the build running on its thread
  (`CancelScope`); the block loop, table rows, code lines, a book's files, PNG rows, URL fetches
  and SVG conversion (`SvgHost::Cancelled`) ask it. A new loop that can run long asks it too. It
  is out of line: inlined at its call sites it grew `tiny_pdf.cpp` past GCC's inlining budget
  (+0.54 % instructions on README.md). Work a cancel cut short is never cached: a cancelled
  image decode is no failed image (`StoreFailure` is skipped). The parser itself does not stop,
  about 7 ms per MB of Markdown.
- Keep visible link text and annotation rectangles aligned when changing text
  wrapping, painting, or page splitting.
- A word is the text between two white spaces of the source, also across emphasis,
  code, and link boundaries. Never insert a space or break a line inside it; only a
  word wider than the line is cut by character.
- Standard-font text is measured with the exact AFM advances of the font that shows
  it (`kStandardWordAdvances`); keep wrapping, painting, and link rectangles on them.
- A performance change must not change PDF bytes unless that is its stated
  purpose. Line breaks and coordinates come from floating-point sums, so keep
  the order of those additions when restructuring wrapping or measuring code,
  and compare whole PDFs before and after (see `docs/development/performance.md`).

## Packaging And Licensing

- Default Windows release should be a single `rayomd.exe` where
  possible.
- Default Linux release should be a compact `rayomd` CLI binary.
- `packaging/arch/` holds the Arch Linux PKGBUILDs (`rayomd` from the source tag, with
  curl; `rayomd-bin` from the static release asset) and their `.SRCINFO`. Do not bump them by hand:
  the release workflow's `arch` job runs `packaging/arch/update.sh <version>` (pkgver,
  pkgrel, sha256sums, `.SRCINFO`), builds, checks and smoke-tests both packages on Arch,
  commits the result to the default branch and, when the `AUR_SSH_PRIVATE_KEY` secret is
  set, pushes both to the AUR. `publish` and `arch` run only for `master`/`main` or a tag and in
  the `release` environment, which holds that secret (with its protection rules in the
  repository settings); a manual run on another branch builds but publishes nothing.
- RayoMD's own code is licensed under Apache-2.0. Keep `LICENSE`,
  `NOTICE`, README license wording, and SPDX headers aligned when changing
  licensing metadata.
- Keep generated build trees, benchmark outputs, PDFs, and temporary corpora out
  of source unless the user explicitly asks to track a specific artifact.

## Change Style

- Match the existing C++17 style.
- Keep changes scoped to the touched subsystem.
- Prefer standard library and platform APIs already in use over new
  dependencies.
- Add small helper functions when they reduce repeated PDF/string/layout logic.
- Avoid broad refactors unless needed for a measured performance or correctness
  issue.
- Use structured parsers/APIs when available instead of ad hoc string surgery.
- Protect user changes in the working tree. Check `git status --short` before
  editing and do not revert unrelated work.
