// SPDX-License-Identifier: Apache-2.0

#pragma once

// Mermaid diagrams without a browser: a bounded subset of Mermaid's flowchart, sequence and pie
// syntax, laid out here and written as an SVG document that Internal::ConvertSvg draws as a form,
// so both renderers show a diagram as they show an SVG image, its text in the document's font.
// The contract (syntax, limits, what is drawn) is docs/development/native_mermaid.md. Built at
// -Os: a diagram is laid out once per document.

#include "svg.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal::Mermaid {

// The colours a diagram is drawn in, each 0xRRGGBB.
struct Palette {
    uint32_t text;            // labels, titles, message text
    uint32_t line;            // edges, messages, lifelines, pie slice borders
    uint32_t nodeFill;        // node and participant boxes
    uint32_t nodeStroke;
    uint32_t clusterFill;     // subgraphs, edge label backgrounds, activations, block labels
    uint32_t clusterStroke;   // subgraphs, block frames
    uint32_t noteFill;        // sequence notes
    uint32_t accent;          // autonumber marks, the first pie colour
};

// Mermaid's default theme, for callers without a theme of their own.
constexpr Palette kDefaultPalette = { 0x333333, 0x333333, 0xECECFF, 0x9370DB, 0xFFFFDE, 0xAAAA33, 0xFFF5AD, 0x9370DB };

// A Mermaid source (the fence's content) as an SVG document that Internal::ConvertSvg draws:
// sizes in px user units, text measured with host.TextWidth (the converter's own measure),
// arrowheads as polygons (the converter draws no <marker>), no foreignObject, no scripts,
// only presentation attributes. False when the type, the syntax or a limit is not supported.
bool ToSvg(std::string_view source, SvgHost& host, const Palette& palette, std::string& svg);

// Where ToSvg placed what it drew, in the SVG's user units before any scaling to fit: for the
// layout checks of tests/mermaid_tests.inc.
struct Geometry {
    struct Box {
        double x, y, width, height;
        int shape;   // a node's Shape, else -1
    };
    // Node shapes of flowcharts; participants of sequence diagrams are kRect boxes.
    enum Shape { kRect, kRound, kStadium, kSubroutine, kCylinder, kCircle, kRhombus, kHexagon,
                 kLeanRight, kLeanLeft, kTrapezoid, kTrapezoidAlt };
    struct End {
        double x, y;   // where the edge or message touches its node
        int node;      // the index into nodes, or -1 for a subgraph or a lifeline
    };
    std::vector<Box> nodes;      // flowchart nodes, the top row of participants
    std::vector<Box> clusters;   // subgraphs, sequence notes and blocks, the pie
    std::vector<Box> texts;      // each line of text as measured: baseline - 0.8 size to baseline + 0.2 size
    std::vector<int> textNode;   // per text, the node it labels, or -1
    std::vector<End> ends;       // each drawn edge's two ends, source first
};

// ToSvg that also fills `geometry`.
bool ToSvg(std::string_view source, SvgHost& host, const Palette& palette, std::string& svg, Geometry* geometry);

} // namespace TinyPdf::Internal::Mermaid
