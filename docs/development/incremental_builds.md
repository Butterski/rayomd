# Incremental builds (BuildSession)

rayomd-gui builds the whole document after every keystroke (`BuildWorker::RunPreview`, with
`PdfOptions::sourceMap`): for 700 copies of `tester.md`, about 160 ms of the 172 ms from a key to
the preview are the engine's. `TinyPdf::BuildSession` (`include/rayomd/tiny_pdf.h`) gives the same
bytes, result and source map as `BuildPdf` and lays out again only the top-level blocks an edit
changed or moved on the page; the layout of the others comes from the session's last build. This
note records where a build's time goes, the design, what it reuses and what it does not, and how
it was checked. Code: `src/core/build_session.h`, `src/core/build_session.cpp` (the session, `-Os`)
and the renderers' side in `src/core/tiny_pdf.cpp` (`SessionAccess`, `RenderBlockRange`).

## Where a build's time goes

Warm builds of the base (5.2.4) by callgrind, `--bench FILE DIR N modern normal`, instructions of
one build (the difference between 3 and 1 iterations, halved), DejaVu Sans; `tN` is N copies of
`tester.md`, `eN` N copies of the warm fixture `essay.md`. Phases are inclusive costs; the lines
under layout are parts of it (paragraphs include their formulas and pages).

| phase | t1 | t70 | t700 | e1 | e10 | e50 |
|---|---:|---:|---:|---:|---:|---:|
| build (BuildDocument) | 1.69M | 92.1M | 920M | 2.19M | 22.2M | 111M |
| WinAnsi transcoding (the font choice) | 0 % | 0 % | 0 % | 2.7 % | 2.8 % | 2.8 % |
| parse (ParseMarkdown) | 14.0 % | 17.8 % | 17.8 % | 10.3 % | 10.7 % | 10.7 % |
| table of contents, footnote numbers | 0.0 % | 0.0 % | 0.0 % | 0.1 % | 0.1 % | 0.1 % |
| layout (RenderBlocks) | 58.1 % | 72.6 % | 72.6 % | 75.5 % | 74.6 % | 74.4 % |
| &nbsp;&nbsp;paragraphs | 27.6 % | 35.1 % | 35.1 % | 70.7 % | 69.8 % | 69.6 % |
| &nbsp;&nbsp;formulas (LayoutMathToFit) | 22.6 % | 29.0 % | 29.1 % | - | - | - |
| &nbsp;&nbsp;tables with formulas | 5.6 % | 6.9 % | 6.9 % | - | - | - |
| &nbsp;&nbsp;quotes | 4.7 % | 6.1 % | 6.1 % | 2.3 % | 2.3 % | 2.3 % |
| &nbsp;&nbsp;tables, code, images | 4.4 % | 4.3 % | 4.2 % | - | - | - |
| &nbsp;&nbsp;page content numbers (WriteFixed2) | 4.2 % | 5.4 % | 5.4 % | 3.9 % | 3.8 % | 3.8 % |
| &nbsp;&nbsp;CID strings (WriteCidText) | 3.3 % | 3.6 % | 3.6 % | - | - | - |
| assembly: link annotations, outline | 1.5 % | 2.0 % | 2.3 % | 3.7 % | 3.8 % | 3.9 % |
| assembly: font subsets, formula fonts | 2.9 % | 0.1 % | 0.0 % | - | - | - |
| assembly: BuildInto | 18.8 % | 2.8 % | 2.6 % | 2.0 % | 2.0 % | 2.1 % |
| the blocks destroyed | 0.9 % | 1.7 % | 1.7 % | 1.7 % | 2.4 % | 2.4 % |

A long document is three quarters layout, a sixth to a tenth parse, and a tenth everything else.
The font subsets cost nothing on a large document (one subset, cached by its CIDs), the image
registry about 1 % (t700's 3,500 images resolve from the process's cache), the outline and the link
annotations 2 to 4 %, `BuildInto` 2 to 3 % (it moves the page content; a small document's image
stream makes most of t1's). For t700 a build is 920 M instructions, 122 ms on a quiet host.

## What the page content depends on

A top-level block's page content is a function of the block, the renderer's state where it
starts, and, for a heading, the blocks after it that its keep reads (`HeadingKeep`: the headings
and anchors right after it and whether a block follows them). It does not depend on:

- page numbers, the table of contents' page numbers and dots, a theme's bands and cover: streams
  of their own the builder adds afterwards;
- the font subsets: the content's CIDs are code points (`WriteCidText`); the subset, the CID-to-GID
  map and the widths are made from the set of CIDs after the layout;
- link targets, `#anchor` resolution, bookmarks: objects the builder makes from the renderer's
  rectangles and marks;
- `--compress` and `--black-text`: they rewrite the content after the layout.

It does depend on, and a session checks or lays out again:

- footnote numbers (drawn from `Footnotes::numbers`, numbered over the whole document) and page
  footnotes: documents with footnotes are laid out whole;
- heading numbers (`--number-headings`): a heading block is reused only with the number it had;
- the table of contents' entries: a `Contents` block is reused only when every entry is the same;
- image XObject names (the order the image registry met the images in) and the image files
  themselves, which may change between builds: image and diagram blocks are always laid out;
- PDF/UA's marked content and structure tree: tagged builds are laid out whole.

## Design

A `BuildSession` holds a tape of its last successful build. A build in a session is a `BuildPdf`
with the session set for the thread (`Internal::CurrentLayoutSession`), so every other step of a
build (the embeds, the font choice, a fallback font's probe and second build, the cancel flag,
the assembly) is the one `BuildPdf` takes. The builders lay a session's blocks out through
`SessionCalls` (`SessionRendererCalls`, which only `BuildSession` asks for). All of it is compiled only
with `RAYOMD_BUILD_SESSION`, which the `rayomd_engine` library and the core tests set: the command lines
make no session and carry none of it (unused, MinGW's linker kept 24 KB of it in `rayomd.exe`).

1. **Parse** (`ParseInSession`, `session_parse.h`): every line is split and classified as in a
   fresh parse (with `ResolveMathOpeners` and the definitions' pass, so the link definitions and
   each line's class are those of the whole text); then the block loop goes on from the last
   build's parse (see "Parse reuse" below) and the blocks it did not make are the last build's,
   kept in the session's vector (`SessionBlocks`), which the builders parse into.
2. **Compare** the blocks with the tape's (`SessionLayout::Begin`, `Same`): the common prefix of
   equal blocks (`SameBlock`: every field the renderers read, not the source lines; a table of
   contents only with equal entries, a numbered heading only with its number). The blocks after it
   map to the tape's in place with as many blocks as before, else shifted by the blocks added or
   removed, and are compared only when the layout reaches one in the state the tape had there
   (`RunFrom`). A run never crosses from the blocks before the change to those after it (they may
   have different shifts), and never holds an image or a Mermaid block (`TouchesImages`).
3. **Lay out a block at a time** (`SessionAccess::Render`): before each block the renderer's
   state is noted (`LayoutMark`: y, page, content length, the links, headings, anchors and table
   of contents lines so far, the source map's entries, the CIDs and formula glyphs recorded, the
   characters drawn as '?', the next heading number, a pending page break, whether the page has
   content, the formula pool's flags, and whether the settings blocks change and set back are as
   they began). Where the block was laid out by the last build from the same state (the same y,
   the same flags, the CID and glyph sets holding what the last build had there), the run of
   blocks from it is **replayed** (`SessionLayout::Replay`): its page content is copied from the
   tape, the page starts shifted by the content's length, its links, headings, anchors, table of
   contents lines, long pages' bottoms and source map entries copied (pages shifted; source lines
   from this build's blocks; heading and anchor marks moved to this build's equal blocks by the
   offsets the tape noted), the CIDs and glyphs the run recorded added in their order, the counts
   advanced, and the renderer's state set to the tape's after the run. A run ends before a block
   whose keep would read past it (a heading or anchor last) unless both documents end there.
   Other blocks are laid out by `RenderBlockRange`, the loop `RenderBlocks` itself runs. A replayed
   run's content is copied page by page, each new page after the gap the builder leaves before it
   (`StartPageContent`, `LayoutTape::pageGaps`), so `BuildInto` moves nothing.
4. **Keep the content** after the layout (`EndLayout`), before `--compress` or `--black-text`
   change it, with the CIDs in the order they were recorded (the font sorts them later).
5. **Assemble** the whole PDF as `BuildPdf` does, then **commit** the tape (`Commit`): it takes the
   blocks, the table of contents and heading numbers, the renderer's links, headings, anchors and
   contents lines, and where each heading and anchor mark lies in its block.

Why the bytes are those of a fresh build: a replayed run starts where the fresh layout would be in
the same state as the last build was (every field of that state the blocks after it read is in the
mark and compared, or is a cache whose value never shows in the output: the size memo, the run
link cache, the widest code advance), the run's blocks are equal, and a block's content is a
function of these. After the run the renderer is where the last build's layout was, except for
the sets, which hold at least what the last build's did (they were compared as supersets) and so
end as the fresh layout's would; the formula pool keeps the previous block's boxes, of which only
whether there are any is read (`MathPool::Active`), and that is set as after the run.

What a session lays out whole (as `BuildPdf`, and keeps no tape): documents with footnotes,
PDF/UA, books (`BuildBookPdf` has no session), empty documents. What it never replays: image and
diagram blocks, a block that drew a character the font lacks while a fallback font is probed. What
starts without a tape: other options (all of `PdfOptions` but `cancel` and `sourceMap` compared),
another renderer or font (ASCII, WinAnsi, a TrueType font, a theme's faces, a fallback font).
A cancelled or failed build leaves the last tape.

Memory: the tape holds the page content (t700: 25 MB), the blocks, and 88 bytes a block of marks.

## Checks

`tests/session_tests.inc` (core check 137, 0.14 s): documents of every kind of block in ASCII,
Latin and Unicode text, edited at random places (snippets of Markdown structure, deleted bytes and
lines, doubled lines, replaced ranges, letters), each version built by one session and by
`BuildPdf` and compared: bytes, result and source map; with the table of contents and heading
numbers, page numbers, long pages with `--black-text` and `--compress`, a theme's bands with page
breaks before headings, footnotes, PDF/UA and Obsidian's dialect; a build cancelled part way every
fifth step; and that a letter typed in a long document lays out at most a tenth of its blocks.

In the lab (2026-10-19), a harness of the same kind over 1, 70 and 700 copies of `tester.md`, the
warm fixtures, generated ASCII, Latin, Unicode, code, table and image documents, the SVG fixtures,
wiki pages, and a Hebrew document under `RAYOMD_FONT=DejaVuSansMono` with DejaVu Sans as the
fallback font (two builds a document), each with one of ten option profiles (themes with faces and
a cover, `--toc`, `--number-headings`, page numbers, long pages, `--compress`, PDF/A, PDF/UA,
footnotes at the end and at the foot of the page, Obsidian's dialect with `--hide-tags` and page
breaks before headings, `--black-text`, `--embed-source`) and cancelled builds between: 1,652
sequences of 25 to 40 edits, 58,762 edited versions each built both ways, all identical.

## Measurements

Per build, 2026-10-19, g++ 13.3 Release, the engine linked into a lab harness, alternate builds of
the text before and after one edit (the GUI's case: the session's last build is the text before
the edit), `perf stat -e cycles:u,instructions:u`, CPU 3 of a host shared with other jobs; "near
the start" is 2 % into the text.

| document, edit | fresh Mcycles | session Mcycles | cycles | instructions |
|---|---:|---:|---:|---:|
| t700, a letter typed near the start / middle / end | 504 / 502 / 500 | 187 / 188 / 188 | 2.7x | 3.2x |
| t700, a word typed (start, middle, end) | 510-514 | 205-215 | 2.4-2.5x | 3.2x |
| t700, a line deleted in the middle | 504 | 190 | 2.6x | 3.2x |
| t700, a paragraph inserted in the middle | 501 | 194 | 2.6x | 3.2x |
| t70, any of these edits anywhere | 50-51 | 20-22 | 2.3-2.6x | 3.1-3.2x |
| e50, a letter or word typed (start, middle, end) | 48-57 | 30-39 | 1.4-1.6x | 3.3x |
| e50, a line deleted or inserted | 55-56 | 32-40 | 1.4-1.6x | 3.2x |
| e10, a letter typed in the middle | 13.0 | 6.9 | 1.9x | 3.3x |
| e10, a paragraph deleted near the start (never in step again) | 13.1 | 15.1 | 0.87x | 0.96x |

On a quiet host a t700 build is 122 ms (`--bench`); by the cycles' ratio a keystroke's engine build
would be about 45 ms [estimate, not timed quietly]. Small documents gain as well (min of 400 builds:
`tester.md` 0.26 -> 0.13 ms, README 0.43 ->
0.23 ms, `essay.md` 0.22 -> 0.08 ms; the 680-byte Polish note the same). The worst case is an edit
that moves flowing text on the page for the rest of the document: paragraphs split across pages a
line at a time, so the layout after it is never in the last build's state again (the essay has few
headings or images that start a page where they did); everything after it is laid out, a block at
a time, at 4 % more instructions than `BuildPdf`. The blocks after such an edit are compared only
when a layout reaches one in the state the last build had there, so that case does not pay for
comparing them.

Where an incremental build of t700 spends its 313 M instructions (callgrind, a letter typed in the
middle): parse 52 %, the layout 18 % (replays 10 %, comparing the blocks 6 %, the blocks laid out:
images and the headings before them, 4 %), `BuildInto` 8 %, the old blocks destroyed 5 %, the
outline and link annotations 7 %. For e50's 36 M: parse 33 %, WinAnsi transcoding 9 %, comparing
the blocks 12 %, replays 10 %, old blocks 8 %, outline and annotations 12 %, `BuildInto` 6 %. In
cycles the copies weigh more than in instructions: the replayed content, the content kept and
`BuildInto`'s move are three passes over 7 MB (e50) to 25 MB (t700), memory-bound.

Ordinary builds: `ab.sh --instructions` against 5.2.4 with the session hooks compiled in (as in the
engine library), every fixture within +0.06 %. The command line, built without them: code +80 bytes
(`RenderBlocks`' loop is `RenderBlockRange`, which a session calls a block at a time), the file the
same size.

## Parse reuse

The block loop of `ParseMarkdownImpl` reads lines only forward: a pass reads at most the line after
the one it ends at, and nothing after an Empty line that ends it. What it carries from one pass to
the next is the line, the HTML elements left open (`Html::HtmlState`, and with it the alignments of
the blocks after them) and the `ParseBudget`; the link definitions, the lines' classes and the
footnotes come from passes over the whole text before it. So a session's parse (`ParsePass` noted
at every pass start, `ParseRecord`) starts its loop at the last pass the last parse began at an
Empty line before the first line whose bytes, class or definition mark differ, with no element
open, and stops at the first pass it begins on a line of the unchanged end (same bytes, class and
mark to the end of the text) where the last parse began one with no element open and the same
padded cells used; the blocks before and after are the last parse's (their source lines moved by
the lines the edit added). It parses whole (and keeps nothing for the next) when the definitions,
the front matter's end, `WinAnsiReferences`, the padded cells at the loop's start or "]:" differ,
the last text had footnotes or a paragraph that may be a contents marker, the build has `--toc`,
`--page-break-before` or embeds; and when a reference could have been refused (less than
`kMaxReferenceDestinationBytes` of the reference budget left at either parse's end, so that every
pass used what it would in a whole parse). A cancelled or failed build puts the last build's
blocks back (`UndoParse`). An opened fence changes every line's class after it until the fences
pair as before: the loop runs to there.

## Next

- **Classify only the changed lines**: now about a quarter of t700's session build; the line
  classes, `ResolveMathOpeners`' and the definitions pass' states would have to be noted per line
  and resumed.
- **Fewer copies**: the tape could keep the replayed content where it lies and splice in the new
  bytes (`EndLayout` copies all of it), and a first edit's fresh tape could reserve the last's sizes.
- **Footnotes**: the body could be replayed when the numbers are the same, with the references'
  first places (`NoteMarks::references`) noted per block; the notes section laid out again.
- **Images**: an image block could be replayed when the registry, asked again, gives the same image
  at the same index.
