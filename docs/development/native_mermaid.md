# Native Mermaid diagrams

RayoMD draws a defined **subset of Mermaid** natively: flowcharts, sequence diagrams and pie
charts, laid out by RayoMD and written as an SVG document that its own SVG converter
(`src/core/svg.cpp`) turns into a form XObject. It is not Mermaid: there is no browser, no
JavaScript, no theme engine and no new dependency, and the pictures follow Mermaid's default
proportions without copying its pixels.

This file is the engineering contract for the feature. Keep it accurate when the reader, the
layout or the drawing change.

## Guarantees

- **A diagram RayoMD does not draw stays the code block it is today, byte for byte.**
  `Mermaid::ToSvg` returns false for an unsupported diagram type, a statement outside the
  subset, malformed syntax and any input past a [limit](#limits); the caller then draws the
  fence's content as code, exactly as without the feature. A diagram never fails an export.
- Every SVG `ToSvg` writes converts: `ConvertSvg` accepts it and draws something
  (`tests/mermaid_tests.inc` converts every accepted diagram of the tests; the `mermaid` fuzz
  target aborts when one does not convert).
- Malformed, unsupported or hostile input never crashes or hangs. Every phase is linear in the
  input or runs a fixed number of sweeps over a graph whose size the limits bound.
- The SVG uses only what the converter draws without help: `rect`, `circle`, `polygon`, `path`
  (`M`, `L`, `C`, `Z` only) and `text`, presentation attributes, no `<marker>` (arrowheads are
  polygons), no `transform`, no `A` arcs, no `foreignObject`, no scripts, no style sheets. The
  converter loads libm only for rotations, skews and arcs, so a diagram never loads it; the
  module itself calls no libm function (pie slices are cubic Béziers from RayoMD's own sine).
- The same source, host and palette give the same SVG bytes. The module has no mutable global
  state; `ToSvg` is safe from several threads with a host each.

## Interface

`src/core/mermaid.h`, namespace `TinyPdf::Internal::Mermaid`:

```cpp
struct Palette { uint32_t text, line, nodeFill, nodeStroke, clusterFill, clusterStroke, noteFill, accent; }; // 0xRRGGBB
constexpr Palette kDefaultPalette = { 0x333333, 0x333333, 0xECECFF, 0x9370DB, 0xFFFFDE, 0xAAAA33, 0xFFF5AD, 0x9370DB };
bool ToSvg(std::string_view source, SvgHost& host, const Palette& palette, std::string& svg);
```

- `source` is the fence's content, `host` the `SvgHost` that will convert the SVG: labels are
  measured with `host.TextWidth`, the converter's own measure, so boxes fit their text exactly.
- Sizes are px user units (the converter draws 1 px as 0.75 pt). A diagram larger than 32,767
  units in either direction keeps its layout and gets a smaller `width`/`height` with a
  `viewBox` of its full size, so no number of the form passes the converter's `kMaxReal`.
- Colours come from the palette only; a theme recolours every diagram. The default palette is
  Mermaid's default theme. Pie slices take colours derived from `accent` and `clusterStroke`
  (their channels rotated, then mixed towards `nodeFill`).
- `ToSvg(source, host, palette, svg, &geometry)` also reports where the layout put nodes, texts
  and edge ends, for the tests' layout checks.

## The source

- UTF-8 that XML can hold: no control characters but tab, line feed and carriage return, no
  surrogates, U+FFFE or U+FFFF. Anything else refuses the diagram.
- Blank lines and lines starting with `%%` (comments, `%%{init: ...}%%` directives) are skipped,
  also before the header. A front matter block (`---` ... `---`) at the very start is read past;
  its title and configuration are not drawn.
- The first statement names the type: `flowchart` or `graph`, `sequenceDiagram`, `pie`. Every
  other type (`classDiagram`, `stateDiagram-v2`, `erDiagram`, `gantt`, `xychart-beta`,
  `mindmap`, `timeline`, `flowchart-elk`, ...) is refused.
- Labels: runs of white space become one space; `<br>`, `<br/>` and `<br />` (any case) break
  lines; other HTML and Markdown in labels is shown as written. Quoted labels (`"..."`) may hold
  the brackets and characters that would end an unquoted one; neither may span lines.

## Flowcharts

Header: `flowchart` or `graph`, optionally a direction `TB`/`TD` (default), `BT`, `LR`, `RL`.
Statements end at a line feed or `;`.

| Form | Meaning |
|---|---|
| `id` | A node, its label the id. Ids: letters, digits, `_`, bytes past ASCII, `-` between them. |
| `id[text]` `id(text)` `id([text])` `id[[text]]` `id[(text)]` `id((text))` `id{text}` `id{{text}}` | Rectangle, rounded rectangle, stadium, subroutine, cylinder, circle, rhombus, hexagon. |
| `id[/text/]` `id[\text\]` `id[/text\]` `id[\text/]` | Parallelogram, parallelogram leaning left, trapezoid, inverted trapezoid. |
| `id["text"]` | A quoted label, in any of the shapes. A later definition replaces an earlier one. |
| `a --> b` `a --- b` | Arrow, open link. |
| `a -.-> b` `a -.- b` `a ==> b` `a === b` | Dotted, thick. |
| `a --x b` `a --o b` `a <--> b` `a x--x b` `a o--o b` | Cross and circle ends, both ends. |
| `a ~~~ b` | An invisible link: laid out, not drawn. |
| `a ---> b` `a -..-> b` `a ===> b` | Longer links span more layers (up to 8). |
| `a -->|text| b` `a -- text --> b` `a == text ==> b` `a -. text .-> b` | Link labels. |
| `a --> b --> c` `a & b --> c & d` | Chains; `&` links every node on one side with every node on the other. |
| `a --> a` | A loop beside the node. |
| `subgraph id [title]` `subgraph id` `subgraph "title"` `subgraph title words` ... `end` | A subgraph, nested up to 12 deep. A node belongs to the first subgraph it is mentioned in. Its id can be linked to and styled. |
| `direction TB` (and the others) | In a subgraph, its direction; outside, the chart's. |
| `classDef name[,name] props` `class id[,id] name` `id:::name` `style id props` | Styles; `classDef default` applies to every node. Later declarations override earlier ones; `style` overrides classes. |
| `linkStyle ...` `click ...` `accTitle: ...` `accDescr: ...` `accDescr { ... }` | Read past; nothing drawn changes. |

- Style properties drawn: `fill`, `stroke`, `stroke-width` (px, at most 20), `color` (the
  label) and `stroke-dasharray` (at most eight lengths). Colours are names, `#hex`,
  `rgb()`/`rgba()`/`hsl()`/`hsla()` as the converter reads them. Other properties and values
  that do not check are left out.
- Refused: `id>text]` (asymmetric), `(((...)))`, `@{ ... }` shapes, a start end without an
  end (`<--`), two labels on one link, an edge between a subgraph and a node or
  subgraph inside it, a loop on a subgraph, a subgraph declared twice, a shape on a subgraph's id,
  `end` without a subgraph, a subgraph left open, and any other statement.

**Layout.** Each level (the chart and the inside of each subgraph) is a layered (Sugiyama)
layout of its items, the subgraphs among them as boxes laid out first:

1. Cycles broken by a depth-first search in source order, the edges back reversed.
2. Layers by the longest path from the sources, then each source moved down next to its
   nearest successor. Layers are doubled: nodes take even layers, and every edge has a bend on
   the odd layer between, where its label sits as a node of the label's size (labels never
   overlap nodes or each other).
3. Crossings reduced by 8 barycentre sweeps (down and up); the order with the fewest crossings
   (counted with a Fenwick tree) stays.
4. Coordinates by 8 sweeps that place each layer as near the nodes it links to as its order and
   spacing allow (weighted least squares by pooling adjacent violators), long edges weighted to
   stay straight.
5. The level turned to its direction; edges drawn through their bends as Catmull-Rom splines
   (cubic Béziers), clipped to the node's outline, arrowheads as triangles.

An edge into a subgraph crosses its border at a port that the subgraph's own layout routes from
its top or bottom side, so the edge reaches its node around the others. A subgraph that edges
cross takes its parent's direction, as Mermaid's does; one they do not cross, its own.

Proportions: labels 14 px (edge labels 12 px), lines 1.3 em apart; nodes 15 px of padding
across, 10 down; 50 px between nodes of a layer, 50 px between layers; subgraphs 10 px of
padding under a title.

## Sequence diagrams

Header: `sequenceDiagram`. Statements end at a line feed or `;`. Names may hold spaces but none
of `< = > - : , ;`.

| Form | Meaning |
|---|---|
| `participant A` `participant A as Alice` `actor B as Bob` | A box or a stick figure, in order of first mention. |
| `A->B: text` `A-->B: text` | Solid or dotted line without a head. |
| `A->>B: text` `A-->>B: text` | Solid or dotted arrow. |
| `A-xB: text` `A--xB: text` | A cross at the end. |
| `A-)B: text` `A--)B: text` | An open arrowhead (asynchronous). |
| `A->>A: text` | A message to oneself, a loop on the right. |
| `A->>+B: text` `B-->>-A: text` | Activates the target; deactivates the source. |
| `activate A` `deactivate A` | Activation boxes, nested, offset 5 px a level. |
| `Note left of A: text` `Note right of A: text` `Note over A: text` `Note over A,B: text` | Notes. |
| `loop` `alt`/`else` `opt` `par`/`and` `critical`/`option` `break` + text ... `end` | Frames with their keyword and condition, nested up to 16 deep. |
| `rect COLOR` ... `end` | A background in that colour (checked as styles are). |
| `autonumber` | Numbers each message from here. |
| `title text` | A title above the diagram. |

- Participants are drawn again under the diagram, as Mermaid does.
- Refused: `create`, `destroy`, `box`, `links`, `link`, `properties`, `details`, `<<->>`
  arrows, `else` outside `alt`, `and` outside `par`, `option` outside `critical`, `end` without a
  frame, a frame left open, deactivating a participant that is not active, and any other
  statement.

**Layout.** Lanes and rows in one pass each: the space between neighbouring lanes is the widest
of their boxes' needs, the messages between them (the widest needs taken by span, narrowest
first), self messages and notes beside them; then the statements go down a row each. Frames
wrap what they hold.

## Pie charts

| Form | Meaning |
|---|---|
| `pie` | The header; `pie showData`, `pie title Text` and `pie showData title Text` too. |
| `title Text` | The title, above the chart. |
| `"Label" : 42.5` | A slice: a label in double quotes and a non-negative decimal number. |
| `showData` | The values in the legend (`Label [42.5]`, as written). |

- Slices go clockwise from the top in source order; each percentage is drawn inside its slice
  where the slice is wide enough for it. A slice of 0 has no wedge and keeps its legend entry.
- Refused: a negative or malformed value, no slice above 0, more than 64 slices.

## Limits

One diagram: 128 KiB of source, labels of 2,048 bytes and 32 lines; flowcharts of 1,000 nodes,
256 subgraphs nested 12 deep, 2,000 edges (after chains and `&` are expanded), 256 class names,
20,000 nodes of all levels' layered graphs (items, ports and bends); sequence diagrams of 64
participants, 2,000 statements and frames nested 16 deep; pies of 64 slices; 4 MiB of SVG.
Past any limit the diagram is refused and shown as code. A 200-node flowchart lays out in about
3 ms.

These keep the SVG within the converter's limits: 1 MiB of text, 64 KiB a text element,
500,000 elements and 64 MiB of content.

## Files

| File | Role |
|---|---|
| `src/core/mermaid.{h,cpp}` | Reading, layout and SVG writing for the three types. Built at `-Os`. |
| `src/core/svg.{h,cpp}` | Converts the SVG into a form (`ConvertSvg`). |
| `tests/mermaid_tests.inc` | Syntax, refusals, layout invariants, golden SVG digests, conversion and limits; core check 125. |
| `tests/fuzz/` | The `mermaid` target: a fixed-width host, then `ConvertSvg` on what it drew. |

## Verifying a change

```sh
build/<dir>/rayomd-core-tests          # exit code 125 is the Mermaid check
build/<dir>/tests/fuzz/rayomd-fuzz-replay tests/fuzz/regressions tests/fuzz/seeds
```

The golden digests in `tests/mermaid_tests.inc` pin the SVG of a few diagrams under a fixed-width
host: a change of what diagrams look like moves them; re-pin them on purpose and say why. Look
at the pictures after a layout change: `rsvg-convert` draws the SVGs much as RayoMD does.
