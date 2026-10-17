// SPDX-License-Identifier: Apache-2.0

#include "mermaid.h"

#include "../common/exact_math.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace TinyPdf::Internal::Mermaid {
namespace {

using RayoMd::ExactMath::LlRound;
using RayoMd::ExactMath::Sqrt;

// ---------------------------------------------------------------------------------------------
// Limits. Each one fails the diagram (ToSvg returns false), never cuts it short. They keep the
// layout's work and the SVG within what svg.cpp accepts: 1 MiB of text (kMaxTextTotal) and 64 KiB
// a text element, 500,000 elements, 64 MiB of content.

constexpr size_t kMaxSourceBytes = 128u << 10;   // the fence's content
constexpr size_t kMaxLabelBytes = 2048;          // one label, white space collapsed
constexpr size_t kMaxLabelLines = 32;
constexpr size_t kMaxNodes = 1000;               // flowchart nodes
constexpr size_t kMaxSubgraphs = 256;
constexpr size_t kMaxNesting = 12;               // subgraphs inside subgraphs
constexpr size_t kMaxEdges = 2000;               // flowchart edges, chains and & expanded
constexpr size_t kMaxLayoutNodes = 20000;        // nodes, subgraphs, ports and bends of all levels
constexpr int kMaxEdgeLength = 8;                // extra dashes of a longer edge
constexpr size_t kMaxClasses = 256;              // classDef names
constexpr size_t kMaxSvgBytes = 4u << 20;
constexpr size_t kMaxTextBytes = 768u << 10;     // all text drawn, under svg.cpp's 1 MiB
constexpr double kMaxExtent = 32767.0;           // the SVG's width and height in px, scaled to fit

// ---------------------------------------------------------------------------------------------
// Proportions (px), close to Mermaid's default theme.

constexpr double kFontSize = 14.0;         // node, participant, title and note text
constexpr double kEdgeFontSize = 12.0;     // edge labels and message numbers
constexpr double kLineFactor = 1.3;        // line height / font size
constexpr double kMargin = 8.0;            // around the whole diagram
constexpr double kNodePadX = 15.0;
constexpr double kNodePadY = 10.0;
constexpr double kNodeGap = 25.0;          // half the space between two nodes of a layer
constexpr double kBendGap = 5.0;           // half the space a bend of an edge keeps
constexpr double kLabelGap = 8.0;          // half the space an edge label keeps
constexpr double kRankGap = 25.0;          // between two layers: twice this between two node rows
constexpr double kClusterPad = 10.0;
constexpr double kArrowLength = 9.0;
constexpr double kArrowHalfWidth = 4.5;
constexpr double kLoopReach = 30.0;        // how far a self loop reaches out of its node
constexpr int kOrderSweeps = 8;            // barycentre sweeps of the crossing reduction
constexpr int kPlaceSweeps = 8;            // sweeps of the coordinate assignment

struct Point {
    double x, y;
};

// ---------------------------------------------------------------------------------------------
// Characters

double Abs(double value) { return value < 0.0 ? -value : value; }
bool IsBlank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }
bool IsDigit(char ch) { return ch >= '0' && ch <= '9'; }
bool IsAlpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
bool IsIdChar(char ch) { return IsAlpha(ch) || IsDigit(ch) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80; }
char Lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch | 0x20) : ch; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t at = 0; at < a.size(); at++) {
        if (Lower(a[at]) != Lower(b[at])) return false;
    }
    return true;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (IsBlank(text.front()) || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (IsBlank(text.back()) || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

bool AllIdChars(std::string_view text) {
    if (text.empty()) return false;
    for (char ch : text) {
        if (!IsIdChar(ch)) return false;
    }
    return true;
}

// Whether `text` is UTF-8 that XML 1.0 can hold: no control characters but tab, line feed and
// carriage return, no surrogates, U+FFFE or U+FFFF. Anything else could not be written into the SVG.
bool IsXmlText(std::string_view text) {
    for (size_t at = 0; at < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80) {
            if (lead < 0x20 && lead != '\t' && lead != '\n' && lead != '\r') return false;
            at++;
            continue;
        }
        size_t length;
        uint32_t code;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (text.size() - at < length) return false;
        for (size_t k = 1; k < length; k++) {
            const unsigned char next = static_cast<unsigned char>(text[at + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = code << 6 | (next & 0x3Fu);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF))) return false;
        if ((code >= 0xD800 && code <= 0xDFFF) || code == 0xFFFE || code == 0xFFFF) return false;
        at += length;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Text

double LineHeight(double size) { return size * kLineFactor; }

// A label as drawn: its lines ('\n' between them), white space collapsed, and their box.
struct Label {
    std::string text;
    double width = 0.0;
    double height = 0.0;
    double size = kFontSize;
};

// The length of a <br>, <br/> or <br /> (any case) at `at`, else 0.
size_t BreakLength(std::string_view text, size_t at) {
    if (text.size() - at < 4 || Lower(text[at + 1]) != 'b' || Lower(text[at + 2]) != 'r') return 0;
    size_t end = at + 3;
    while (end < text.size() && IsBlank(text[end])) end++;
    if (end < text.size() && text[end] == '/') end++;
    while (end < text.size() && IsBlank(text[end])) end++;
    return end < text.size() && text[end] == '>' ? end + 1 - at : 0;
}

// Sets a label's text from its source: runs of white space become one space, <br> a line break,
// each line trimmed, as the SVG converter collapses white space. Measures it in `host`'s font.
bool SetLabel(std::string_view raw, double size, SvgHost& host, Label& label) {
    label.text.clear();
    label.size = size;
    bool space = false;
    size_t lines = 1;
    for (size_t at = 0; at < raw.size(); at++) {
        const char ch = raw[at];
        if (ch == '<') {
            if (const size_t length = BreakLength(raw, at)) {
                label.text += '\n';
                space = false;
                at += length - 1;
                if (++lines > kMaxLabelLines) return false;
                continue;
            }
        }
        if (IsBlank(ch) || ch == '\n') {
            space = true;
            continue;
        }
        if (space && !label.text.empty() && label.text.back() != '\n') label.text += ' ';
        space = false;
        label.text += ch;
        if (label.text.size() > kMaxLabelBytes) return false;
    }
    label.width = 0.0;
    label.height = 0.0;
    if (label.text.empty()) return true;
    size_t begin = 0;
    for (;;) {
        const size_t end = std::min(label.text.find('\n', begin), label.text.size());
        if (end > begin) {
            const double width = host.TextWidth(std::string_view(label.text).substr(begin, end - begin), size, false);
            if (!(width >= 0.0 && width < 1e6)) return false;
            label.width = std::max(label.width, width);
        }
        if (end == label.text.size()) break;
        begin = end + 1;
    }
    label.height = static_cast<double>(lines) * LineHeight(size);
    return true;
}

// ---------------------------------------------------------------------------------------------
// SVG output

// A number with at most two decimals, trailing zeros dropped.
void Num(std::string& out, double value) {
    long long hundredths = LlRound(value * 100.0);
    if (hundredths < 0) {
        out += '-';
        hundredths = -hundredths;
    }
    char digits[24];
    int count = 0;
    long long whole = hundredths / 100;
    do {
        digits[count++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole > 0);
    while (count > 0) out += digits[--count];
    const int fraction = static_cast<int>(hundredths % 100);
    if (fraction != 0) {
        out += '.';
        out += static_cast<char>('0' + fraction / 10);
        if (fraction % 10 != 0) out += static_cast<char>('0' + fraction % 10);
    }
}

void NumAttr(std::string& out, const char* name, double value) {
    out += ' ';
    out += name;
    out += "=\"";
    Num(out, value);
    out += '"';
}

void PointText(std::string& out, Point p) {
    Num(out, p.x);
    out += ' ';
    Num(out, p.y);
}

// A colour of the palette, or a value a diagram's style gave (checked by StyleValue), which
// takes its place.
void PaintAttr(std::string& out, const char* name, uint32_t rgb, std::string_view css = {}) {
    out += ' ';
    out += name;
    out += "=\"";
    if (!css.empty()) {
        out += css;
    } else {
        static const char kHex[] = "0123456789abcdef";
        out += '#';
        for (int shift = 20; shift >= 0; shift -= 4) out += kHex[(rgb >> shift) & 15];
    }
    out += '"';
}

void Escaped(std::string& out, std::string_view text) {
    for (char ch : text) {
        if (ch == '&') out += "&amp;";
        else if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '"') out += "&quot;";
        else out += ch;
    }
}

// What a diagram draws into: the SVG's body and, for the tests, the geometry.
struct Canvas {
    SvgHost& host;
    const Palette& palette;
    std::string body;
    Geometry* geometry;
    size_t textBytes = 0;   // the text drawn: labels repeat when '&' expands a labelled link
    bool painted = false;   // something drawn in a palette colour, which ConvertSvg surely paints
};

enum Anchor { kStart, kMiddle, kEnd };

// One line of text with its baseline at `baseline`.
void TextLine(Canvas& canvas, std::string_view line, double x, double baseline, double size, Anchor anchor,
              uint32_t rgb, std::string_view css, int node) {
    if (line.empty()) return;
    canvas.textBytes += line.size();
    if (css.empty()) canvas.painted = true;
    std::string& out = canvas.body;
    out += "<text";
    NumAttr(out, "x", x);
    NumAttr(out, "y", baseline);
    NumAttr(out, "font-size", size);
    if (anchor == kMiddle) out += " text-anchor=\"middle\"";
    else if (anchor == kEnd) out += " text-anchor=\"end\"";
    PaintAttr(out, "fill", rgb, css);
    out += '>';
    Escaped(out, line);
    out += "</text>\n";
    if (Geometry* geometry = canvas.geometry) {
        const double width = canvas.host.TextWidth(line, size, false);
        const double left = anchor == kMiddle ? x - width / 2.0 : anchor == kEnd ? x - width : x;
        geometry->texts.push_back({ left, baseline - 0.8 * size, width, size, -1 });
        geometry->textNode.push_back(node);
    }
}

// The lines of a label in a box of its height whose top is `top`: each centred on `x` (or
// starting or ending there), its glyphs' box (baseline - 0.8 size to + 0.2) in the middle of its line.
void LabelLines(Canvas& canvas, const Label& label, double x, double top, Anchor anchor, uint32_t rgb,
                std::string_view css = {}, int node = -1) {
    const double lineHeight = LineHeight(label.size);
    double baseline = top + (lineHeight - label.size) / 2.0 + 0.8 * label.size;
    size_t begin = 0;
    for (;;) {
        const size_t end = std::min(label.text.find('\n', begin), label.text.size());
        TextLine(canvas, std::string_view(label.text).substr(begin, end - begin), x, baseline, label.size, anchor, rgb, css, node);
        if (end >= label.text.size()) break;
        begin = end + 1;
        baseline += lineHeight;
    }
}

// A label centred on (cx, cy).
void CentredLabel(Canvas& canvas, const Label& label, double cx, double cy, uint32_t rgb, std::string_view css = {}, int node = -1) {
    LabelLines(canvas, label, cx, cy - label.height / 2.0, kMiddle, rgb, css, node);
}

void Rect(std::string& out, double x, double y, double width, double height) {
    out += "<rect";
    NumAttr(out, "x", x);
    NumAttr(out, "y", y);
    NumAttr(out, "width", width);
    NumAttr(out, "height", height);
}

// A filled triangle pointing at `tip` from the direction (ux, uy), a unit vector.
void ArrowHead(std::string& out, Point tip, double ux, double uy, uint32_t rgb) {
    const Point base{ tip.x - ux * kArrowLength, tip.y - uy * kArrowLength };
    out += "<polygon points=\"";
    PointText(out, tip);
    out += ' ';
    PointText(out, { base.x - uy * kArrowHalfWidth, base.y + ux * kArrowHalfWidth });
    out += ' ';
    PointText(out, { base.x + uy * kArrowHalfWidth, base.y - ux * kArrowHalfWidth });
    out += '"';
    PaintAttr(out, "fill", rgb);
    out += "/>\n";
}

// The unit vector from `from` to `to`, (0, 1) when they meet.
void Direction(Point from, Point to, double& ux, double& uy) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = Sqrt(dx * dx + dy * dy);
    if (!(length > 1e-9)) {
        ux = 0.0;
        uy = 1.0;
        return;
    }
    ux = dx / length;
    uy = dy / length;
}

// ---------------------------------------------------------------------------------------------
// Styles (classDef, class, :::, style)

// The properties of a style RayoMD draws; values are views of the source, checked.
struct Style {
    std::string_view fill, stroke, color, dash;
    double width = -1.0;   // stroke-width, unset below 0
};

void MergeStyle(Style& to, const Style& from) {
    if (!from.fill.empty()) to.fill = from.fill;
    if (!from.stroke.empty()) to.stroke = from.stroke;
    if (!from.color.empty()) to.color = from.color;
    if (!from.dash.empty()) to.dash = from.dash;
    if (from.width >= 0.0) to.width = from.width;
}

// A paint value written as given: a colour name, #hex, rgb()/rgba()/hsl()/hsla() or none; the
// SVG converter reads it. Only the characters those use, so it cannot leave its attribute.
bool IsPaintValue(std::string_view value) {
    if (value.empty() || value.size() > 64 || (value.size() >= 4 && EqualsNoCase(value.substr(0, 4), "url("))) return false;
    for (char ch : value) {
        if (!(IsAlpha(ch) || IsDigit(ch) || ch == '#' || ch == '(' || ch == ')' || ch == ',' || ch == '.' ||
              ch == '%' || ch == ' ' || ch == '/' || ch == '-')) {
            return false;
        }
    }
    return true;
}

// A non-negative number, digits and at most one point, with an optional "px".
bool ParseLength(std::string_view text, double& value) {
    if (text.size() > 2 && EqualsNoCase(text.substr(text.size() - 2), "px")) text.remove_suffix(2);
    if (text.empty() || text.size() > 12) return false;
    double whole = 0.0, scale = 1.0;
    bool point = false, digit = false;
    for (char ch : text) {
        if (ch == '.' && !point) {
            point = true;
        } else if (IsDigit(ch)) {
            digit = true;
            if (point) {
                scale /= 10.0;
                whole += (ch - '0') * scale;
            } else {
                whole = whole * 10.0 + (ch - '0');
            }
        } else {
            return false;
        }
    }
    value = whole;
    return digit;
}

// Up to eight lengths separated by spaces or commas, each at most 1,000.
bool IsDashValue(std::string_view value) {
    int count = 0;
    size_t at = 0;
    while (at < value.size()) {
        while (at < value.size() && (value[at] == ' ' || value[at] == ',')) at++;
        if (at == value.size()) break;
        size_t end = at;
        while (end < value.size() && value[end] != ' ' && value[end] != ',') end++;
        double length;
        if (!ParseLength(value.substr(at, end - at), length) || length > 1000.0 || ++count > 8) return false;
        at = end;
    }
    return count > 0;
}

// The declarations of a style, "fill:#f9f,stroke:#333,stroke-width:4px": the five properties
// RayoMD draws; others, and values it cannot check, are left out.
void ParseStyle(std::string_view text, Style& style) {
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = at;
        int depth = 0;
        while (end < text.size() && (text[end] != ',' || depth > 0)) {
            if (text[end] == '(') depth++;
            else if (text[end] == ')' && depth > 0) depth--;
            end++;
        }
        const std::string_view declaration = text.substr(at, end - at);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name = Trim(declaration.substr(0, colon));
            std::string_view value = Trim(declaration.substr(colon + 1));
            if (value.size() > 10 && EqualsNoCase(value.substr(value.size() - 10), "!important")) value = Trim(value.substr(0, value.size() - 10));
            double width;
            if (EqualsNoCase(name, "fill") && IsPaintValue(value)) style.fill = value;
            else if (EqualsNoCase(name, "stroke") && IsPaintValue(value)) style.stroke = value;
            else if (EqualsNoCase(name, "color") && IsPaintValue(value)) style.color = value;
            else if (EqualsNoCase(name, "stroke-width") && ParseLength(value, width)) style.width = std::min(width, 20.0);
            else if (EqualsNoCase(name, "stroke-dasharray") && IsDashValue(value)) style.dash = value;
        }
        at = end + 1;
    }
}

// Fill, stroke and stroke width of a shape: the style's where it has them, else the palette's.
void ShapePaint(Canvas& canvas, uint32_t fill, uint32_t stroke, double width, const Style& style) {
    std::string& out = canvas.body;
    if (style.fill.empty() || (style.stroke.empty() && style.width != 0.0)) canvas.painted = true;
    PaintAttr(out, "fill", fill, style.fill);
    PaintAttr(out, "stroke", stroke, style.stroke);
    NumAttr(out, "stroke-width", style.width >= 0.0 ? style.width : width);
    if (!style.dash.empty()) {
        out += " stroke-dasharray=\"";
        out += style.dash;
        out += '"';
    }
}

// ---------------------------------------------------------------------------------------------
// Reading

// The source a line at a time: statements end at a line feed, or at ';' where a diagram allows.
struct Cursor {
    const char* at;
    const char* end;

    bool Done() const { return at >= end; }
    char Peek(size_t ahead = 0) const { return static_cast<size_t>(end - at) > ahead ? at[ahead] : '\0'; }
    void SkipBlanks() {
        while (at < end && IsBlank(*at)) at++;
    }
    bool StartsWith(std::string_view text) const {
        return static_cast<size_t>(end - at) >= text.size() && std::memcmp(at, text.data(), text.size()) == 0;
    }
    // The rest of the line, without its line feed, which is skipped.
    std::string_view Line() {
        const char* start = at;
        while (at < end && *at != '\n') at++;
        const std::string_view line(start, static_cast<size_t>(at - start));
        if (at < end) at++;
        return line;
    }
    // A word of letters, digits, '-' and '_', or empty.
    std::string_view Word() const {
        const char* stop = at;
        while (stop < end && (IsAlpha(*stop) || IsDigit(*stop) || *stop == '-' || *stop == '_')) stop++;
        return std::string_view(at, static_cast<size_t>(stop - at));
    }
    // Whether the statement ends here: a line feed, ';' where allowed, or the end.
    bool AtStatementEnd(bool semicolon) const { return at >= end || *at == '\n' || (semicolon && *at == ';'); }
};

// Whether `word` comes next and is followed by a blank or the end of the statement.
bool TakeKeyword(Cursor& cursor, std::string_view word, bool semicolon) {
    if (!cursor.StartsWith(word)) return false;
    Cursor after{ cursor.at + word.size(), cursor.end };
    if (!after.AtStatementEnd(semicolon) && !IsBlank(after.Peek())) return false;
    cursor.at = after.at;
    cursor.SkipBlanks();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Flowcharts: model

enum Dir : uint8_t { kTB, kBT, kLR, kRL };
enum LineKind : uint8_t { kSolid, kDotted, kThick, kInvisible };
enum Head : uint8_t { kNoHead, kArrowHead, kCrossHead, kCircleHead };

bool ParseDir(std::string_view word, Dir& dir) {
    if (word == "TB" || word == "TD") dir = kTB;
    else if (word == "BT") dir = kBT;
    else if (word == "LR") dir = kLR;
    else if (word == "RL") dir = kRL;
    else return false;
    return true;
}

bool Horizontal(Dir dir) { return dir == kLR || dir == kRL; }

// A node or a subgraph: edges may join either.
struct Vertex {
    std::string_view id;
    std::string_view text;     // the label (a subgraph's title) as written
    bool hasText = false;
    bool cluster = false;
    bool placed = false;       // its subgraph is decided: mentioned inside one
    bool dirSet = false;
    bool hasPorts = false;     // a subgraph that edges cross
    int shape = Geometry::kRect;
    int parent = -1;           // the subgraph it is in
    Dir dir = kTB;             // a subgraph's direction statement
    Dir flow = kTB;            // the direction its contents are laid out in
    Style style;
    Label label;
    // Layout: the size, the top left corner in its parent's box, and on the page.
    double width = 0.0, height = 0.0;
    double x = 0.0, y = 0.0;
    double ax = 0.0, ay = 0.0;
    int local = -1;            // its node in its level's graph
    int loops = 0;             // edges from it to itself
    double loopLabelWidth = 0.0, loopLabelHeight = 0.0;
};

struct Edge {
    int from = 0, to = 0;
    LineKind line = kSolid;
    Head startHead = kNoHead, endHead = kNoHead;
    int length = 1;
    std::string_view text;
    bool hasText = false;
    Label label;
    int firstPiece = 0;
    int pieces = 0;
    std::vector<Point> points;   // the drawn line, source to target, on the page
    Point labelAt{ 0.0, 0.0 };
};

constexpr int kPort = -2;

// An edge's part in one level of the layout (the root or a subgraph's inside): between two of
// its items, or between an item and the level's own border ("port"), where the edge crosses it.
struct Piece {
    int edge = 0;
    int level = -1;     // the subgraph, -1 for the root
    int a = 0, b = 0;   // the source and target end: a vertex of the level, or kPort
    bool bottom = false;   // a port end on the side after the last layer
    bool label = false;    // carries the edge's label
    std::vector<Point> points;   // source to target, in the level's box
    Point port{ 0.0, 0.0 };      // where it crosses the level's border, in the level's box
    Point labelAt{ 0.0, 0.0 };
};

struct Flow {
    Dir dir = kTB;
    std::vector<Vertex> vertices;
    std::vector<Edge> edges;
    std::vector<Piece> pieces;
    std::unordered_map<std::string_view, int> names;
    std::unordered_map<std::string_view, Style> classes;
    std::vector<std::pair<int, std::string_view>> classUses;   // in source order
    size_t nodes = 0;
    size_t subgraphs = 0;
    std::vector<int> clusterOrder;   // subgraphs, each after its parent
    std::vector<int> nodeIndex;      // per vertex, its index among the nodes drawn
};

// ---------------------------------------------------------------------------------------------
// Flowcharts: reading

class FlowReader {
public:
    FlowReader(Flow& flow, Cursor cursor) : f_(flow), c_(cursor) {}

    bool Read() {
        for (;;) {
            c_.SkipBlanks();
            if (c_.Done()) break;
            if (*c_.at == '\n' || *c_.at == ';') {
                c_.at++;
                continue;
            }
            if (c_.StartsWith("%%")) {
                c_.Line();
                continue;
            }
            if (!Statement()) return false;
            c_.SkipBlanks();
            if (!c_.AtStatementEnd(true)) return false;
        }
        return open_.empty() && !f_.vertices.empty();
    }

private:
    bool Statement() {
        if (TakeKeyword(c_, "subgraph", true)) return Subgraph();
        if (TakeKeyword(c_, "end", true)) {
            if (open_.empty()) return false;
            open_.pop_back();
            return true;
        }
        if (TakeKeyword(c_, "direction", true)) {
            Dir dir;
            const std::string_view word = c_.Word();
            if (!ParseDir(word, dir)) return false;
            c_.at += word.size();
            if (open_.empty()) {
                f_.dir = dir;
            } else {
                f_.vertices[static_cast<size_t>(open_.back())].dir = dir;
                f_.vertices[static_cast<size_t>(open_.back())].dirSet = true;
            }
            return true;
        }
        if (TakeKeyword(c_, "classDef", true)) return ClassDef();
        if (TakeKeyword(c_, "class", true)) return Class();
        if (TakeKeyword(c_, "style", true)) {
            int vertex;
            if (!Name(vertex)) return false;
            c_.SkipBlanks();
            ParseStyle(StatementRest(), f_.vertices[static_cast<size_t>(vertex)].style);
            return true;
        }
        // Statements that change nothing drawn: link styles, click handlers, accessible titles.
        if (TakeKeyword(c_, "linkStyle", true) || TakeKeyword(c_, "click", true)) {
            StatementRest();
            return true;
        }
        if (c_.StartsWith("accTitle") || c_.StartsWith("accDescr")) {
            c_.at += 8;
            c_.SkipBlanks();
            if (c_.Peek() == '{') {
                while (!c_.Done() && *c_.at != '}') c_.at++;
                if (c_.Done()) return false;
                c_.at++;
            } else if (c_.Peek() == ':') {
                while (!c_.Done() && *c_.at != '\n') c_.at++;
            } else {
                return false;
            }
            return true;
        }
        return Chain();
    }

    // The rest of a statement, to a line feed or ';'.
    std::string_view StatementRest() {
        const char* start = c_.at;
        while (!c_.AtStatementEnd(true)) c_.at++;
        return Trim(std::string_view(start, static_cast<size_t>(c_.at - start)));
    }

    // The vertex an id names, made a node when it is new.
    bool Find(std::string_view id, int& vertex) {
        const auto found = f_.names.find(id);
        if (found != f_.names.end()) {
            vertex = found->second;
            return true;
        }
        if (++f_.nodes > kMaxNodes) return false;
        vertex = static_cast<int>(f_.vertices.size());
        f_.vertices.emplace_back();
        f_.vertices.back().id = id;
        f_.names.emplace(id, vertex);
        return true;
    }

    // An id: letters, digits, '_', bytes past ASCII, and '-' between them.
    std::string_view Id() {
        const char* start = c_.at;
        while (!c_.Done()) {
            const char ch = *c_.at;
            if (IsIdChar(ch) || (ch == '-' && c_.at > start && IsIdChar(c_.Peek(1)) && c_.Peek(1) != 'x' && c_.Peek(1) != 'o')) {
                c_.at++;
            } else if (ch == '-' && c_.at > start && (c_.Peek(1) == 'x' || c_.Peek(1) == 'o') && IsIdChar(c_.Peek(2))) {
                c_.at++;
            } else {
                break;
            }
        }
        return std::string_view(start, static_cast<size_t>(c_.at - start));
    }

    bool Name(int& vertex) {
        const std::string_view id = Id();
        return !id.empty() && Find(id, vertex);
    }

    bool Subgraph() {
        if (f_.subgraphs++ >= kMaxSubgraphs || open_.size() >= kMaxNesting) return false;
        const char* start = c_.at;
        while (!c_.Done() && *c_.at != '\n') c_.at++;
        std::string_view rest(start, static_cast<size_t>(c_.at - start));
        rest = Trim(rest);
        if (!rest.empty() && rest.back() == ';') rest = Trim(rest.substr(0, rest.size() - 1));
        std::string_view id, title;
        const size_t bracket = rest.find('[');
        if (bracket != std::string_view::npos && bracket > 0 && rest.back() == ']') {
            id = Trim(rest.substr(0, bracket));
            title = Unquote(Trim(rest.substr(bracket + 1, rest.size() - bracket - 2)));
            if (!AllIdChars(id)) return false;
        } else if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
            title = rest.substr(1, rest.size() - 2);
        } else {
            title = rest;
            if (AllIdChars(rest)) id = rest;
        }
        int vertex;
        const auto found = id.empty() ? f_.names.end() : f_.names.find(id);
        if (found != f_.names.end()) {
            vertex = found->second;
            Vertex& existing = f_.vertices[static_cast<size_t>(vertex)];
            if (existing.cluster) return false;   // declared twice
            f_.nodes--;
        } else {
            vertex = static_cast<int>(f_.vertices.size());
            f_.vertices.emplace_back();
            f_.vertices.back().id = id;
            if (!id.empty()) f_.names.emplace(id, vertex);
        }
        Vertex& cluster = f_.vertices[static_cast<size_t>(vertex)];
        cluster.cluster = true;
        cluster.placed = true;
        cluster.shape = Geometry::kRect;
        cluster.text = title;
        cluster.hasText = true;
        cluster.parent = open_.empty() ? -1 : open_.back();
        open_.push_back(vertex);
        return true;
    }

    static std::string_view Unquote(std::string_view text) {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
        return text;
    }

    bool ClassDef() {
        std::vector<std::string_view> names;
        for (;;) {
            const std::string_view name = c_.Word();
            if (name.empty()) return false;
            names.push_back(name);
            c_.at += name.size();
            if (c_.Peek() != ',') break;
            c_.at++;
            c_.SkipBlanks();
        }
        c_.SkipBlanks();
        Style style;
        ParseStyle(StatementRest(), style);
        for (std::string_view name : names) {
            auto found = f_.classes.find(name);
            if (found == f_.classes.end()) {
                if (f_.classes.size() >= kMaxClasses) return false;
                f_.classes.emplace(name, style);
            } else {
                MergeStyle(found->second, style);
            }
        }
        return true;
    }

    bool Class() {
        std::vector<int> targets;
        for (;;) {
            int vertex;
            if (!Name(vertex)) return false;
            targets.push_back(vertex);
            c_.SkipBlanks();
            if (c_.Peek() != ',') break;
            c_.at++;
            c_.SkipBlanks();
        }
        const std::string_view name = c_.Word();
        if (name.empty()) return false;
        c_.at += name.size();
        for (int vertex : targets) f_.classUses.emplace_back(vertex, name);
        return true;
    }

    // A node: its id, an optional shape with its label, an optional ":::class".
    bool Node(int& vertex) {
        if (!Name(vertex)) return false;
        Vertex* v = &f_.vertices[static_cast<size_t>(vertex)];
        if (!open_.empty() && !v->placed && !v->cluster) {
            v->parent = open_.back();
            v->placed = true;
        }
        const char ch = c_.Peek();
        if (ch == '[' || ch == '(' || ch == '{') {
            if (v->cluster) return false;
            int shape;
            std::string_view text;
            if (!Shape(shape, text)) return false;
            v = &f_.vertices[static_cast<size_t>(vertex)];
            v->shape = shape;
            v->text = text;
            v->hasText = true;
        }
        if (c_.StartsWith(":::")) {
            c_.at += 3;
            const std::string_view name = c_.Word();
            if (name.empty()) return false;
            c_.at += name.size();
            f_.classUses.emplace_back(vertex, name);
        }
        return true;
    }

    // A shape's brackets and the label between them.
    bool Shape(int& shape, std::string_view& text) {
        struct Form {
            const char* open;
            const char* close;
            int shape;
        };
        // Longer openers first; "[/" and "[\" each close two ways.
        static const Form kForms[] = {
            { "([", "])", Geometry::kStadium },    { "[[", "]]", Geometry::kSubroutine },
            { "[(", ")]", Geometry::kCylinder },   { "((", "))", Geometry::kCircle },
            { "{{", "}}", Geometry::kHexagon },    { "[/", "/]", Geometry::kLeanRight },
            { "[\\", "\\]", Geometry::kLeanLeft }, { "[", "]", Geometry::kRect },
            { "(", ")", Geometry::kRound },        { "{", "}", Geometry::kRhombus },
        };
        const Form* form = nullptr;
        for (const Form& candidate : kForms) {
            if (c_.StartsWith(candidate.open)) {
                form = &candidate;
                break;
            }
        }
        if (form == nullptr) return false;
        c_.at += std::strlen(form->open);
        const char* start = c_.at;
        const bool slanted = form->shape == Geometry::kLeanRight || form->shape == Geometry::kLeanLeft;
        // The closer: for the slanted forms, "/]" or "\]", which tells the shape.
        auto closesHere = [&](const char* at) -> int {
            const size_t left = static_cast<size_t>(c_.end - at);
            if (slanted) {
                if (left >= 2 && (at[0] == '/' || at[0] == '\\') && at[1] == ']') {
                    const bool lean = (at[0] == '/') == (form->shape == Geometry::kLeanRight);
                    if (lean) return form->shape;
                    return form->shape == Geometry::kLeanRight ? Geometry::kTrapezoid : Geometry::kTrapezoidAlt;
                }
                return -1;
            }
            const size_t length = std::strlen(form->close);
            return left >= length && std::memcmp(at, form->close, length) == 0 ? form->shape : -1;
        };
        Cursor inner{ start, c_.end };
        inner.SkipBlanks();
        if (inner.Peek() == '"') {
            const char* quoted = inner.at + 1;
            const char* close = quoted;
            while (close < c_.end && *close != '"' && *close != '\n') close++;
            if (close >= c_.end || *close != '"') return false;
            text = std::string_view(quoted, static_cast<size_t>(close - quoted));
            inner.at = close + 1;
            inner.SkipBlanks();
            shape = closesHere(inner.at);
            if (shape < 0) return false;
            c_.at = inner.at + (slanted ? 2 : std::strlen(form->close));
            return true;
        }
        const char* at = start;
        while (at < c_.end && *at != '\n') {
            shape = closesHere(at);
            if (shape >= 0) {
                text = std::string_view(start, static_cast<size_t>(at - start));
                c_.at = at + (slanted ? 2 : std::strlen(form->close));
                return true;
            }
            at++;
        }
        return false;
    }

    // Nodes joined by '&'.
    bool Group(std::vector<int>& group) {
        group.clear();
        for (;;) {
            int vertex;
            if (!Node(vertex)) return false;
            group.push_back(vertex);
            c_.SkipBlanks();
            if (c_.Peek() != '&') return true;
            c_.at++;
            c_.SkipBlanks();
        }
    }

    // Counts a run of `ch` from `at`.
    static size_t Run(const char* at, const char* end, char ch) {
        const char* stop = at;
        while (stop < end && *stop == ch) stop++;
        return static_cast<size_t>(stop - at);
    }

    // The end of a link after its line: '>', or 'x'/'o' that no id continues.
    Head EndHead(const char* at, bool forced) const {
        if (at >= c_.end) return kNoHead;
        if (*at == '>') return kArrowHead;
        if ((*at == 'x' || *at == 'o') && (forced || at + 1 >= c_.end || !IsIdChar(at[1]))) {
            return *at == 'x' ? kCrossHead : kCircleHead;
        }
        return kNoHead;
    }

    // A link's line and end from `at`: "-->", "---", "-.->", "-.-", "==>", "===", "--x", "--o",
    // their longer forms, and "~~~". Sets `next` past it; false when there is none.
    bool LinkBody(const char* at, Edge& edge, const char*& next, bool afterText) const {
        const char* end = c_.end;
        if (at >= end) return false;
        if (*at == '~') {
            const size_t count = Run(at, end, '~');
            if (count < 3) return false;
            edge.line = kInvisible;
            edge.length = static_cast<int>(count) - 2;
            next = at + count;
            return true;
        }
        if (*at == '-' && at + 1 < end && at[1] == '.') {
            const size_t dots = Run(at + 1, end, '.');
            const char* dash = at + 1 + dots;
            if (dash >= end || *dash != '-') return false;
            edge.line = kDotted;
            edge.endHead = EndHead(dash + 1, true);
            edge.length = static_cast<int>(dots);
            next = dash + 1 + (edge.endHead != kNoHead ? 1 : 0);
            return true;
        }
        if (afterText && *at == '.') {   // the end of "-. text .->"
            const size_t dots = Run(at, end, '.');
            const char* dash = at + dots;
            if (dash >= end || *dash != '-') return false;
            edge.line = kDotted;
            edge.endHead = EndHead(dash + 1, true);
            edge.length = static_cast<int>(dots);
            next = dash + 1 + (edge.endHead != kNoHead ? 1 : 0);
            return true;
        }
        if (*at == '-' || *at == '=') {
            const char ch = *at;
            const size_t count = Run(at, end, ch);
            if (count < 2) return false;
            edge.line = ch == '=' ? kThick : kSolid;
            edge.endHead = EndHead(at + count, count == 2);
            if (edge.endHead == kNoHead) {
                if (count < 3) return false;
                edge.length = static_cast<int>(count) - 2;
                next = at + count;
            } else {
                edge.length = static_cast<int>(count) - 1;
                next = at + count + 1;
            }
            return true;
        }
        return false;
    }

    // A link: its line, ends and label, "-->|text|" or "-- text -->".
    bool Link(Edge& edge) {
        const char* at = c_.at;
        const char* end = c_.end;
        if (*at == '<') {
            edge.startHead = kArrowHead;
            at++;
        } else if ((*at == 'x' || *at == 'o') && at + 1 < end && (at[1] == '-' || at[1] == '=')) {
            edge.startHead = *at == 'x' ? kCrossHead : kCircleHead;
            at++;
        }
        if (at >= end) return false;
        const char* next = nullptr;
        if (!LinkBody(at, edge, next, false)) {
            // "-- text -->", "== text ==>", "-. text .->": an opener, the text, a link.
            const bool dotted = at + 1 < end && at[0] == '-' && at[1] == '.';
            const char ch = *at;
            if (!dotted && !((ch == '-' || ch == '=') && Run(at, end, ch) == 2)) return false;
            const char* text = at + 2;
            const char* scan = text;
            while (scan < end && *scan != '\n') {
                const bool closes = dotted ? (*scan == '.' && Run(scan, end, '.') + scan < end && scan[Run(scan, end, '.')] == '-')
                                           : (*scan == ch && scan + 1 < end && scan[1] == ch);
                if (closes) break;
                scan++;
            }
            if (scan >= end || *scan == '\n') return false;
            edge.text = Trim(std::string_view(text, static_cast<size_t>(scan - text)));
            edge.hasText = true;
            if (!LinkBody(scan, edge, next, true)) return false;
            if (edge.line != (dotted ? kDotted : ch == '=' ? kThick : kSolid)) return false;
        }
        if (edge.startHead != kNoHead && edge.endHead == kNoHead) return false;
        edge.length = std::min(std::max(edge.length, 1), kMaxEdgeLength);
        c_.at = next;
        c_.SkipBlanks();
        if (c_.Peek() == '|') {
            if (edge.hasText) return false;
            const char* text = c_.at + 1;
            const char* close = text;
            while (close < end && IsBlank(*close)) close++;
            if (close < end && *close == '"') {
                // A quoted label may hold '|'.
                close++;
                while (close < end && *close != '"' && *close != '\n') close++;
                if (close >= end || *close != '"') return false;
                close++;
                while (close < end && IsBlank(*close)) close++;
                if (close >= end || *close != '|') return false;
            }
            while (close < end && *close != '|' && *close != '\n') close++;
            if (close >= end || *close != '|') return false;
            edge.text = Unquote(Trim(std::string_view(text, static_cast<size_t>(close - text))));
            edge.hasText = true;
            c_.at = close + 1;
        }
        return true;
    }

    // Nodes and links: "A --> B & C -.-> D".
    bool Chain() {
        std::vector<int> group;
        std::vector<int> next;
        if (!Group(group)) return false;
        for (;;) {
            c_.SkipBlanks();
            if (c_.AtStatementEnd(true)) return true;
            Edge edge{};
            if (!Link(edge)) return false;
            c_.SkipBlanks();
            if (!Group(next)) return false;
            if (f_.edges.size() + group.size() * next.size() > kMaxEdges) return false;
            for (int from : group) {
                for (int to : next) {
                    edge.from = from;
                    edge.to = to;
                    f_.edges.push_back(edge);
                }
            }
            group.swap(next);
        }
    }

    Flow& f_;
    Cursor c_;
    std::vector<int> open_;   // the subgraphs open, innermost last
};

// ---------------------------------------------------------------------------------------------
// Flowcharts: sizes and borders

// A node's size for its label.
void SizeNode(Vertex& v) {
    const double textWidth = v.label.width;
    const double textHeight = std::max(v.label.height, LineHeight(kFontSize));
    double& w = v.width;
    double& h = v.height;
    switch (v.shape) {
    case Geometry::kSubroutine:
        w = textWidth + 2.0 * kNodePadX + 16.0;
        h = textHeight + 2.0 * kNodePadY;
        break;
    case Geometry::kStadium:
        h = textHeight + 2.0 * kNodePadY;
        w = textWidth + h + 2.0 * 8.0;
        break;
    case Geometry::kCylinder: {
        w = textWidth + 2.0 * kNodePadX;
        const double ry = std::min(std::max(w * 0.08, 5.0), 14.0);
        h = textHeight + 2.0 * kNodePadY + 3.0 * ry;
        break;
    }
    case Geometry::kCircle:
        w = h = Sqrt(textWidth * textWidth + textHeight * textHeight) + 16.0;
        break;
    case Geometry::kRhombus:
        w = h = textWidth + textHeight + 24.0;
        break;
    case Geometry::kHexagon:
        h = textHeight + 2.0 * kNodePadY;
        w = textWidth + 2.0 * kNodePadX + h / 2.0;
        break;
    case Geometry::kLeanRight:
    case Geometry::kLeanLeft:
    case Geometry::kTrapezoid:
    case Geometry::kTrapezoidAlt: {
        h = textHeight + 2.0 * kNodePadY;
        const double pad = std::max(kNodePadX, textHeight / 4.0 + 2.0);
        w = textWidth + 2.0 * pad + h / 2.0;
        break;
    }
    default:
        w = textWidth + 2.0 * kNodePadX;
        h = textHeight + 2.0 * kNodePadY;
        break;
    }
}

// The cylinder's cap: the half height of its ellipses.
double CylinderCap(double width) { return std::min(std::max(width * 0.08, 5.0), 14.0); }

// The corners of a polygonal shape, clockwise from the top left; 0 for round shapes.
int Corners(int shape, double x, double y, double w, double h, Point (&p)[6]) {
    const double x1 = x + w, y1 = y + h, cx = x + w / 2.0, cy = y + h / 2.0;
    const double slant = h / 2.0;
    switch (shape) {
    case Geometry::kRhombus:
        p[0] = { cx, y };
        p[1] = { x1, cy };
        p[2] = { cx, y1 };
        p[3] = { x, cy };
        return 4;
    case Geometry::kHexagon: {
        const double m = h / 4.0;
        p[0] = { x + m, y };
        p[1] = { x1 - m, y };
        p[2] = { x1, cy };
        p[3] = { x1 - m, y1 };
        p[4] = { x + m, y1 };
        p[5] = { x, cy };
        return 6;
    }
    case Geometry::kLeanRight:
        p[0] = { x + slant, y };
        p[1] = { x1, y };
        p[2] = { x1 - slant, y1 };
        p[3] = { x, y1 };
        return 4;
    case Geometry::kLeanLeft:
        p[0] = { x, y };
        p[1] = { x1 - slant, y };
        p[2] = { x1, y1 };
        p[3] = { x + slant, y1 };
        return 4;
    case Geometry::kTrapezoid:
        p[0] = { x + slant / 2.0, y };
        p[1] = { x1 - slant / 2.0, y };
        p[2] = { x1, y1 };
        p[3] = { x, y1 };
        return 4;
    case Geometry::kTrapezoidAlt:
        p[0] = { x, y };
        p[1] = { x1, y };
        p[2] = { x1 - slant / 2.0, y1 };
        p[3] = { x + slant / 2.0, y1 };
        return 4;
    case Geometry::kCircle:
    case Geometry::kStadium:
        return 0;
    default:
        p[0] = { x, y };
        p[1] = { x1, y };
        p[2] = { x1, y1 };
        p[3] = { x, y1 };
        return 4;
    }
}

// Where the ray from the centre of a shape at (x, y, w, h) towards `toward` leaves it.
Point Border(int shape, double x, double y, double w, double h, Point toward) {
    const Point c{ x + w / 2.0, y + h / 2.0 };
    const double dx = toward.x - c.x, dy = toward.y - c.y;
    if (!(dx * dx + dy * dy > 1e-12)) return { c.x, y };
    if (shape == Geometry::kCircle) {
        const double length = Sqrt(dx * dx + dy * dy);
        const double r = w / 2.0;
        return { c.x + dx * r / length, c.y + dy * r / length };
    }
    if (shape == Geometry::kStadium) {
        // The rectangle between the caps, else the circle of the cap it leaves through.
        const double r = h / 2.0;
        const Point hit = Border(Geometry::kRect, x, y, w, h, toward);
        double capX = c.x;
        if (w > 2.0 * r) {
            if (hit.x >= x + r && hit.x <= x + w - r) return hit;
            capX = hit.x < x + r ? x + r : x + w - r;
        }
        // |c + t d - (capX, c.y)|^2 = r^2, the larger root.
        const double ox = c.x - capX;
        const double qa = dx * dx + dy * dy, qb = 2.0 * ox * dx, qc = ox * ox - r * r;
        const double disc = qb * qb - 4.0 * qa * qc;
        if (!(disc >= 0.0)) return hit;
        const double t = (-qb + Sqrt(disc)) / (2.0 * qa);
        return { c.x + t * dx, c.y + t * dy };
    }
    Point p[6];
    const int count = Corners(shape, x, y, w, h, p);
    double best = 1e300;
    for (int k = 0; k < count; k++) {
        const Point a = p[k], b = p[(k + 1) % count];
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double denom = dx * ey - dy * ex;
        if (denom > -1e-12 && denom < 1e-12) continue;
        const double t = ((a.x - c.x) * ey - (a.y - c.y) * ex) / denom;
        const double s = ((a.x - c.x) * dy - (a.y - c.y) * dx) / denom;
        if (t > 0.0 && s >= -1e-9 && s <= 1.0 + 1e-9 && t < best) best = t;
    }
    if (best > 1e299) return c;
    return { c.x + best * dx, c.y + best * dy };
}

// ---------------------------------------------------------------------------------------------
// Flowcharts: layout
//
// Each level, the root and every subgraph's inside, is laid out on its own as a layered graph of
// its items (its nodes and its subgraphs as boxes) in a canonical frame, layers going down,
// then turned to its direction. Layers are doubled, items on even ones, so every edge has a
// bend on an odd layer between, where its label sits as a node of the label's size. An edge into
// a subgraph enters through a port on the subgraph's border that the subgraph's own layout routes
// on: a subgraph that edges cross takes its parent's direction, as in Mermaid.

class FlowLayout {
public:
    FlowLayout(Flow& flow, SvgHost& host) : f_(flow), host_(host) {}

    bool Run(double& width, double& height) {
        const size_t count = f_.vertices.size();
        children_.assign(count + 1, {});
        levels_.assign(count + 1, {});
        for (size_t index = 0; index < count; index++) {
            const Vertex& v = f_.vertices[index];
            children_[static_cast<size_t>(v.parent + 1)].push_back(static_cast<int>(index));
        }
        if (!MakePieces()) return false;
        // Subgraphs outer ones first: a subgraph made from a node mentioned earlier can have a
        // lower index than its parent.
        f_.clusterOrder.clear();
        for (size_t index = 0; index < count; index++) {
            if (f_.vertices[index].cluster && f_.vertices[index].parent < 0) f_.clusterOrder.push_back(static_cast<int>(index));
        }
        for (size_t head = 0; head < f_.clusterOrder.size(); head++) {
            for (int child : children_[static_cast<size_t>(f_.clusterOrder[head] + 1)]) {
                if (f_.vertices[static_cast<size_t>(child)].cluster) f_.clusterOrder.push_back(child);
            }
        }
        for (int index : f_.clusterOrder) {
            Vertex& v = f_.vertices[static_cast<size_t>(index)];
            const Dir parent = v.parent < 0 ? f_.dir : f_.vertices[static_cast<size_t>(v.parent)].flow;
            v.flow = v.hasPorts || !v.dirSet ? parent : v.dir;
        }
        if (!Level(-1)) return false;
        width = rootWidth_;
        height = rootHeight_;
        // Page positions: subgraphs outer ones first, then nodes.
        for (int index : f_.clusterOrder) PagePosition(f_.vertices[static_cast<size_t>(index)]);
        for (Vertex& v : f_.vertices) {
            if (!v.cluster) PagePosition(v);
        }
        for (Edge& edge : f_.edges) {
            for (int k = 0; k < edge.pieces; k++) {
                const Piece& piece = f_.pieces[static_cast<size_t>(edge.firstPiece + k)];
                const double ox = piece.level < 0 ? kMargin : f_.vertices[static_cast<size_t>(piece.level)].ax;
                const double oy = piece.level < 0 ? kMargin : f_.vertices[static_cast<size_t>(piece.level)].ay;
                for (const Point& p : piece.points) {
                    const Point q{ ox + p.x, oy + p.y };
                    if (!edge.points.empty() && Abs(edge.points.back().x - q.x) < 0.01 && Abs(edge.points.back().y - q.y) < 0.01) {
                        continue;
                    }
                    edge.points.push_back(q);
                }
                if (piece.label) edge.labelAt = { ox + piece.labelAt.x, oy + piece.labelAt.y };
            }
        }
        return true;
    }

private:
    struct Node {
        int layer = 0;
        double along = 0.0;   // size on the order axis
        double across = 0.0;  // size on the rank axis
        double extra = 0.0;   // room after it on the order axis, for self loops
        double gap = kBendGap;
        double pos = 0.0;     // centre on the order axis
        int index = 0;        // position in its layer
        bool bend = true;     // a bend or a port, not an item
    };

    void PagePosition(Vertex& v) const {
        v.ax = (v.parent < 0 ? kMargin : f_.vertices[static_cast<size_t>(v.parent)].ax) + v.x;
        v.ay = (v.parent < 0 ? kMargin : f_.vertices[static_cast<size_t>(v.parent)].ay) + v.y;
    }

    // The vertices from `v` up to (not including) `stop`.
    int Below(int v, int stop) const {
        while (f_.vertices[static_cast<size_t>(v)].parent != stop) v = f_.vertices[static_cast<size_t>(v)].parent;
        return v;
    }

    int Depth(int v) const {
        int depth = 0;
        for (int at = f_.vertices[static_cast<size_t>(v)].parent; at >= 0; at = f_.vertices[static_cast<size_t>(at)].parent) depth++;
        return depth;
    }

    void AddPiece(int edge, int level, int a, int b, bool label) {
        Piece piece;
        piece.edge = edge;
        piece.level = level;
        piece.a = a;
        piece.b = b;
        piece.label = label;
        levels_[static_cast<size_t>(level + 1)].push_back(static_cast<int>(f_.pieces.size()));
        if (level >= 0 && (a == kPort || b == kPort)) f_.vertices[static_cast<size_t>(level)].hasPorts = true;
        f_.pieces.push_back(std::move(piece));
    }

    // Splits each edge into its pieces: from the source up to the subgraph holding both ends,
    // across it, and down to the target.
    bool MakePieces() {
        std::vector<int> down;
        for (size_t index = 0; index < f_.edges.size(); index++) {
            Edge& edge = f_.edges[index];
            const int edgeIndex = static_cast<int>(index);
            edge.firstPiece = static_cast<int>(f_.pieces.size());
            if (edge.from == edge.to) {
                Vertex& v = f_.vertices[static_cast<size_t>(edge.from)];
                if (v.cluster) return false;
                v.loops++;
                v.loopLabelWidth = std::max(v.loopLabelWidth, edge.label.width);
                v.loopLabelHeight += edge.label.height > 0.0 ? edge.label.height + 4.0 : 0.0;
                continue;
            }
            // The lowest subgraph holding both ends; an edge between a subgraph and what it holds
            // has none.
            int a = f_.vertices[static_cast<size_t>(edge.from)].parent;
            int b = f_.vertices[static_cast<size_t>(edge.to)].parent;
            int depthA = edge.from >= 0 ? Depth(edge.from) : 0;
            int depthB = Depth(edge.to);
            while (depthA > depthB) {
                a = f_.vertices[static_cast<size_t>(a)].parent;
                depthA--;
            }
            while (depthB > depthA) {
                b = f_.vertices[static_cast<size_t>(b)].parent;
                depthB--;
            }
            while (a != b) {
                a = f_.vertices[static_cast<size_t>(a)].parent;
                b = f_.vertices[static_cast<size_t>(b)].parent;
            }
            const int lca = a;
            for (int at = f_.vertices[static_cast<size_t>(edge.to)].parent; at >= 0; at = f_.vertices[static_cast<size_t>(at)].parent) {
                if (at == edge.from) return false;
            }
            for (int at = f_.vertices[static_cast<size_t>(edge.from)].parent; at >= 0; at = f_.vertices[static_cast<size_t>(at)].parent) {
                if (at == edge.to) return false;
            }
            // Source side, the innermost first.
            int prev = edge.from;
            for (int at = f_.vertices[static_cast<size_t>(edge.from)].parent; at != lca; at = f_.vertices[static_cast<size_t>(at)].parent) {
                AddPiece(edgeIndex, at, prev, kPort, false);
                prev = at;
            }
            AddPiece(edgeIndex, lca, prev, Below(edge.to, lca), edge.hasText && !edge.label.text.empty());
            down.clear();
            prev = edge.to;
            for (int at = f_.vertices[static_cast<size_t>(edge.to)].parent; at != lca; at = f_.vertices[static_cast<size_t>(at)].parent) {
                down.push_back(prev);
                down.push_back(at);
                prev = at;
            }
            for (size_t k = down.size(); k >= 2; k -= 2) AddPiece(edgeIndex, down[k - 1], kPort, down[k - 2], false);
            edge.pieces = static_cast<int>(f_.pieces.size()) - edge.firstPiece;
        }
        return true;
    }

    // The piece of `edge` laid out in `level`.
    Piece* PieceIn(int edge, int level) {
        const Edge& e = f_.edges[static_cast<size_t>(edge)];
        for (int k = 0; k < e.pieces; k++) {
            Piece& piece = f_.pieces[static_cast<size_t>(e.firstPiece + k)];
            if (piece.level == level) return &piece;
        }
        return nullptr;
    }

    // Whether the piece's end `end` (a or b) is a subgraph the edge goes into.
    bool EntersCluster(const Piece& piece, int end) const {
        if (end < 0) return false;
        const Edge& e = f_.edges[static_cast<size_t>(piece.edge)];
        return f_.vertices[static_cast<size_t>(end)].cluster && end != (end == piece.a ? e.from : e.to);
    }

    bool Level(int level);

    // Places the nodes of a layer, in their order, as near their targets as their separations
    // allow: weighted least squares under order constraints, by pooling adjacent violators.
    void Place(const std::vector<int>& layer, const std::vector<double>& target, const std::vector<double>& weight) {
        const size_t count = layer.size();
        offsets_.resize(count);
        blocks_.clear();
        double offset = 0.0;
        for (size_t k = 0; k < count; k++) {
            if (k > 0) offset += Separation(nodes_[static_cast<size_t>(layer[k - 1])], nodes_[static_cast<size_t>(layer[k])]);
            offsets_[k] = offset;
            Block block{ weight[k], weight[k] * (target[k] - offset), k, k + 1 };
            while (!blocks_.empty() && blocks_.back().sum * block.weight > block.sum * blocks_.back().weight) {
                const Block& last = blocks_.back();
                block.weight += last.weight;
                block.sum += last.sum;
                block.begin = last.begin;
                blocks_.pop_back();
            }
            blocks_.push_back(block);
        }
        for (const Block& block : blocks_) {
            const double mean = block.sum / block.weight;
            for (size_t k = block.begin; k < block.end; k++) nodes_[static_cast<size_t>(layer[k])].pos = mean + offsets_[k];
        }
    }

    static double Separation(const Node& left, const Node& right) {
        return left.along / 2.0 + left.extra + left.gap + right.gap + right.along / 2.0;
    }

    struct Block {
        double weight, sum;
        size_t begin, end;
    };

    // Crossings between the layers, counted with a Fenwick tree per pair of layers.
    long long Crossings() {
        long long total = 0;
        for (size_t l = 0; l + 1 < layers_.size(); l++) {
            const size_t size = layers_[l + 1].size();
            tree_.assign(size + 1, 0);
            long long inserted = 0;
            for (int x : layers_[l]) {
                scratch_.clear();
                for (int k = downStart_[static_cast<size_t>(x)]; k < downStart_[static_cast<size_t>(x) + 1]; k++) {
                    scratch_.push_back(nodes_[static_cast<size_t>(down_[static_cast<size_t>(k)])].index);
                }
                for (int index : scratch_) {
                    long long below = 0;
                    for (size_t i = static_cast<size_t>(index) + 1; i > 0; i -= i & (~i + 1)) below += tree_[i];
                    total += inserted - below;
                }
                for (int index : scratch_) {
                    for (size_t i = static_cast<size_t>(index) + 1; i <= size; i += i & (~i + 1)) tree_[i]++;
                    inserted++;
                }
            }
        }
        return total;
    }

    Flow& f_;
    SvgHost& host_;
    std::vector<std::vector<int>> children_;   // by parent + 1
    std::vector<std::vector<int>> levels_;     // pieces by level + 1
    size_t layoutNodes_ = 0;
    double rootWidth_ = 0.0, rootHeight_ = 0.0;
    // One level's graph, reused (a subgraph's level is done before its parent's needs them).
    std::vector<Node> nodes_;
    std::vector<std::vector<int>> layers_;
    std::vector<int> upStart_, up_, downStart_, down_;
    std::vector<double> upWeight_, downWeight_;
    std::vector<double> offsets_;
    std::vector<Block> blocks_;
    std::vector<long long> tree_;
    std::vector<int> scratch_;
};

bool FlowLayout::Level(int level) {
    const std::vector<int>& items = children_[static_cast<size_t>(level + 1)];
    const std::vector<int>& pieces = levels_[static_cast<size_t>(level + 1)];
    const Dir dir = level < 0 ? f_.dir : f_.vertices[static_cast<size_t>(level)].flow;
    const size_t count = items.size();
    if (count == 0) {
        if (level >= 0) {
            // An empty subgraph: its title alone.
            Vertex& cluster = f_.vertices[static_cast<size_t>(level)];
            cluster.width = std::max(cluster.label.width, 40.0) + 2.0 * kClusterPad;
            cluster.height = cluster.label.height + 2.0 * kClusterPad + 20.0;
        }
        return true;
    }
    layoutNodes_ += count;
    if (layoutNodes_ > kMaxLayoutNodes) return false;
    for (size_t k = 0; k < count; k++) f_.vertices[static_cast<size_t>(items[k])].local = static_cast<int>(k);

    // The ends of each piece in this level's graph: items, then a node per port.
    std::vector<int> endA(pieces.size()), endB(pieces.size());
    std::vector<int> layer(count, 0);
    size_t ports = 0;
    bool topPorts = false;
    for (size_t k = 0; k < pieces.size(); k++) {
        const Piece& piece = f_.pieces[static_cast<size_t>(pieces[k])];
        if (piece.a == kPort || piece.b == kPort) {
            ports++;
            if (!piece.bottom) topPorts = true;
        }
        endA[k] = piece.a == kPort ? -1 : f_.vertices[static_cast<size_t>(piece.a)].local;
        endB[k] = piece.b == kPort ? -1 : f_.vertices[static_cast<size_t>(piece.b)].local;
    }
    layoutNodes_ += ports;
    if (layoutNodes_ > kMaxLayoutNodes) return false;

    // Cycles broken by a depth-first search in source order; the edges back reversed.
    std::vector<int> outStart(count + 1, 0), out;
    std::vector<char> reversed(pieces.size(), 0);
    for (size_t k = 0; k < pieces.size(); k++) {
        if (endA[k] >= 0 && endB[k] >= 0) outStart[static_cast<size_t>(endA[k]) + 1]++;
    }
    for (size_t k = 0; k < count; k++) outStart[k + 1] += outStart[k];
    out.resize(static_cast<size_t>(outStart[count]));
    {
        std::vector<int> fill(outStart.begin(), outStart.end() - 1);
        for (size_t k = 0; k < pieces.size(); k++) {
            if (endA[k] >= 0 && endB[k] >= 0) out[static_cast<size_t>(fill[static_cast<size_t>(endA[k])]++)] = static_cast<int>(k);
        }
    }
    {
        std::vector<char> state(count, 0);
        std::vector<std::pair<int, int>> stack;
        for (size_t root = 0; root < count; root++) {
            if (state[root] != 0) continue;
            stack.emplace_back(static_cast<int>(root), outStart[root]);
            state[root] = 1;
            while (!stack.empty()) {
                auto& [node, next] = stack.back();
                if (next == outStart[static_cast<size_t>(node) + 1]) {
                    state[static_cast<size_t>(node)] = 2;
                    stack.pop_back();
                    continue;
                }
                const int k = out[static_cast<size_t>(next++)];
                const int target = endB[static_cast<size_t>(k)];
                if (state[static_cast<size_t>(target)] == 1) {
                    reversed[static_cast<size_t>(k)] = 1;
                } else if (state[static_cast<size_t>(target)] == 0) {
                    state[static_cast<size_t>(target)] = 1;
                    stack.emplace_back(target, outStart[static_cast<size_t>(target)]);
                }
            }
        }
    }

    // Layers by the longest path from the sources (Kahn's order), each edge two layers a step
    // of its length; then each source moved down to just above its nearest successor.
    {
        std::vector<int> succStart(count + 1, 0), succ, indegree(count, 0);
        for (size_t k = 0; k < pieces.size(); k++) {
            if (endA[k] < 0 || endB[k] < 0) continue;
            const int from = reversed[k] ? endB[k] : endA[k];
            succStart[static_cast<size_t>(from) + 1]++;
        }
        for (size_t k = 0; k < count; k++) succStart[k + 1] += succStart[k];
        succ.resize(static_cast<size_t>(succStart[count]));
        std::vector<int> fill(succStart.begin(), succStart.end() - 1);
        for (size_t k = 0; k < pieces.size(); k++) {
            if (endA[k] < 0 || endB[k] < 0) continue;
            const int from = reversed[k] ? endB[k] : endA[k];
            const int to = reversed[k] ? endA[k] : endB[k];
            succ[static_cast<size_t>(fill[static_cast<size_t>(from)]++)] = static_cast<int>(k);
            indegree[static_cast<size_t>(to)]++;
        }
        const int base = topPorts ? 2 : 0;
        std::vector<int> order;
        order.reserve(count);
        for (size_t k = 0; k < count; k++) {
            layer[k] = base;
            if (indegree[k] == 0) order.push_back(static_cast<int>(k));
        }
        const std::vector<int> sources(indegree);
        for (size_t head = 0; head < order.size(); head++) {
            const int from = order[head];
            for (int s = succStart[static_cast<size_t>(from)]; s < succStart[static_cast<size_t>(from) + 1]; s++) {
                const int k = succ[static_cast<size_t>(s)];
                const int to = reversed[static_cast<size_t>(k)] ? endA[static_cast<size_t>(k)] : endB[static_cast<size_t>(k)];
                const int span = 2 * f_.edges[static_cast<size_t>(f_.pieces[static_cast<size_t>(pieces[static_cast<size_t>(k)])].edge)].length;
                layer[static_cast<size_t>(to)] = std::max(layer[static_cast<size_t>(to)], layer[static_cast<size_t>(from)] + span);
                if (--indegree[static_cast<size_t>(to)] == 0) order.push_back(to);
            }
        }
        if (order.size() != count) return false;
        for (size_t head = order.size(); head-- > 0;) {
            const int from = order[head];
            if (sources[static_cast<size_t>(from)] != 0 || succStart[static_cast<size_t>(from)] == succStart[static_cast<size_t>(from) + 1]) continue;
            int best = 1 << 30;
            for (int s = succStart[static_cast<size_t>(from)]; s < succStart[static_cast<size_t>(from) + 1]; s++) {
                const int k = succ[static_cast<size_t>(s)];
                const int to = reversed[static_cast<size_t>(k)] ? endA[static_cast<size_t>(k)] : endB[static_cast<size_t>(k)];
                const int span = 2 * f_.edges[static_cast<size_t>(f_.pieces[static_cast<size_t>(pieces[static_cast<size_t>(k)])].edge)].length;
                best = std::min(best, layer[static_cast<size_t>(to)] - span);
            }
            layer[static_cast<size_t>(from)] = std::max(best, base);
        }
    }
    int lastLayer = 0;
    for (size_t k = 0; k < count; k++) lastLayer = std::max(lastLayer, layer[k]);
    const int bottomLayer = lastLayer + 2;

    // The sides where edges enter the subgraphs of this level: towards the other end.
    auto layerOf = [&](size_t k, bool a) -> int {
        const int end = a ? endA[k] : endB[k];
        if (end >= 0) return layer[static_cast<size_t>(end)];
        return f_.pieces[static_cast<size_t>(pieces[k])].bottom ? bottomLayer : 0;
    };
    for (size_t k = 0; k < pieces.size(); k++) {
        const Piece& piece = f_.pieces[static_cast<size_t>(pieces[k])];
        for (int side = 0; side < 2; side++) {
            const int end = side == 0 ? piece.a : piece.b;
            if (!EntersCluster(piece, end)) continue;
            Piece* inner = PieceIn(piece.edge, end);
            if (inner == nullptr) return false;
            inner->bottom = layerOf(k, side != 0) > layerOf(k, side == 0);
        }
    }
    for (int item : items) {
        if (f_.vertices[static_cast<size_t>(item)].cluster && !Level(item)) return false;
    }

    // This level's graph: items, ports, then the bends of every piece.
    nodes_.assign(count + ports, Node());
    for (size_t k = 0; k < count; k++) {
        const Vertex& v = f_.vertices[static_cast<size_t>(items[k])];
        Node& node = nodes_[k];
        node.layer = layer[k];
        node.along = Horizontal(dir) ? v.height : v.width;
        node.across = Horizontal(dir) ? v.width : v.height;
        node.gap = kNodeGap;
        node.bend = false;
        if (v.loops > 0) {
            node.extra = kLoopReach + 12.0 * (v.loops - 1) +
                (Horizontal(dir) ? v.loopLabelHeight : (v.loopLabelWidth > 0.0 ? v.loopLabelWidth + 8.0 : 0.0));
        }
    }
    std::vector<int> chainStart(pieces.size() + 1, 0), chain;
    std::vector<int> labelNode(pieces.size(), -1);
    {
        size_t port = count;
        for (size_t k = 0; k < pieces.size(); k++) {
            const Piece& piece = f_.pieces[static_cast<size_t>(pieces[k])];
            if (endA[k] < 0 || endB[k] < 0) {
                const size_t node = port++;
                nodes_[node].layer = piece.bottom ? bottomLayer : 0;
                (endA[k] < 0 ? endA[k] : endB[k]) = static_cast<int>(node);
            }
        }
    }
    for (size_t k = 0; k < pieces.size(); k++) {
        const Piece& piece = f_.pieces[static_cast<size_t>(pieces[k])];
        const int a = endA[k], b = endB[k];
        const bool forward = nodes_[static_cast<size_t>(a)].layer < nodes_[static_cast<size_t>(b)].layer;
        const int lo = forward ? a : b, hi = forward ? b : a;
        const int loLayer = nodes_[static_cast<size_t>(lo)].layer, hiLayer = nodes_[static_cast<size_t>(hi)].layer;
        if (hiLayer - loLayer > 1) {
            layoutNodes_ += static_cast<size_t>(hiLayer - loLayer - 1);
            if (layoutNodes_ > kMaxLayoutNodes) return false;
        }
        int labelLayer = -1;
        if (piece.label) {
            labelLayer = (loLayer + hiLayer) / 2;
            if (labelLayer % 2 == 0) labelLayer--;
            if (labelLayer <= loLayer) labelLayer = loLayer + 1;
        }
        chain.push_back(lo);
        for (int l = loLayer + 1; l < hiLayer; l++) {
            Node bend;
            bend.layer = l;
            if (l == labelLayer) {
                const Label& label = f_.edges[static_cast<size_t>(piece.edge)].label;
                bend.along = (Horizontal(dir) ? label.height : label.width) + 4.0;
                bend.across = (Horizontal(dir) ? label.width : label.height) + 4.0;
                bend.gap = kLabelGap;
                labelNode[k] = static_cast<int>(nodes_.size());
            }
            chain.push_back(static_cast<int>(nodes_.size()));
            nodes_.push_back(bend);
        }
        chain.push_back(hi);
        chainStart[k + 1] = static_cast<int>(chain.size());
    }

    // Links between neighbouring layers, both ways, with weights that keep long edges straight.
    const size_t total = nodes_.size();
    upStart_.assign(total + 1, 0);
    downStart_.assign(total + 1, 0);
    for (size_t k = 0; k < pieces.size(); k++) {
        for (int at = chainStart[k]; at + 1 < chainStart[k + 1]; at++) {
            downStart_[static_cast<size_t>(chain[static_cast<size_t>(at)]) + 1]++;
            upStart_[static_cast<size_t>(chain[static_cast<size_t>(at) + 1]) + 1]++;
        }
    }
    for (size_t k = 0; k < total; k++) {
        upStart_[k + 1] += upStart_[k];
        downStart_[k + 1] += downStart_[k];
    }
    up_.resize(static_cast<size_t>(upStart_[total]));
    down_.resize(static_cast<size_t>(downStart_[total]));
    upWeight_.resize(up_.size());
    downWeight_.resize(down_.size());
    {
        std::vector<int> upFill(upStart_.begin(), upStart_.end() - 1), downFill(downStart_.begin(), downStart_.end() - 1);
        for (size_t k = 0; k < pieces.size(); k++) {
            for (int at = chainStart[k]; at + 1 < chainStart[k + 1]; at++) {
                const int x = chain[static_cast<size_t>(at)], y = chain[static_cast<size_t>(at) + 1];
                const bool bx = nodes_[static_cast<size_t>(x)].bend, by = nodes_[static_cast<size_t>(y)].bend;
                const double weight = bx && by ? 8.0 : bx || by ? 2.0 : 1.0;
                const size_t d = static_cast<size_t>(downFill[static_cast<size_t>(x)]++);
                down_[d] = y;
                downWeight_[d] = weight;
                const size_t u = static_cast<size_t>(upFill[static_cast<size_t>(y)]++);
                up_[u] = x;
                upWeight_[u] = weight;
            }
        }
    }

    // Order: the barycentre heuristic, a fixed number of sweeps down and up; the order with the
    // fewest crossings stays.
    int layerCount = bottomLayer + 1;
    layers_.assign(static_cast<size_t>(layerCount), {});
    for (size_t k = 0; k < total; k++) {
        std::vector<int>& row = layers_[static_cast<size_t>(nodes_[k].layer)];
        nodes_[k].index = static_cast<int>(row.size());
        row.push_back(static_cast<int>(k));
    }
    std::vector<std::vector<int>> best = layers_;
    long long fewest = Crossings();
    std::vector<std::pair<double, int>> keyed;
    for (int sweep = 0; sweep < kOrderSweeps && fewest > 0; sweep++) {
        if (host_.Cancelled()) return false;
        const bool downward = sweep % 2 == 0;
        for (int step = 1; step < layerCount; step++) {
            const int l = downward ? step : layerCount - 1 - step;
            std::vector<int>& row = layers_[static_cast<size_t>(l)];
            keyed.clear();
            for (int x : row) {
                const std::vector<int>& list = downward ? up_ : down_;
                const std::vector<int>& start = downward ? upStart_ : downStart_;
                double sum = 0.0;
                int n = 0;
                for (int k = start[static_cast<size_t>(x)]; k < start[static_cast<size_t>(x) + 1]; k++) {
                    sum += nodes_[static_cast<size_t>(list[static_cast<size_t>(k)])].index;
                    n++;
                }
                keyed.emplace_back(n > 0 ? sum / n : nodes_[static_cast<size_t>(x)].index, x);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const std::pair<double, int>& p, const std::pair<double, int>& q) { return p.first < q.first; });
            for (size_t k = 0; k < keyed.size(); k++) {
                row[k] = keyed[k].second;
                nodes_[static_cast<size_t>(keyed[k].second)].index = static_cast<int>(k);
            }
        }
        const long long crossings = Crossings();
        if (crossings < fewest) {
            fewest = crossings;
            best = layers_;
        }
    }
    layers_ = std::move(best);
    for (const std::vector<int>& row : layers_) {
        for (size_t k = 0; k < row.size(); k++) nodes_[static_cast<size_t>(row[k])].index = static_cast<int>(k);
    }

    // Positions on the order axis: packed, then sweeps that pull each node towards the nodes it
    // links to (from above, from below, then both), its layer's order and spacing kept.
    for (const std::vector<int>& row : layers_) {
        for (size_t k = 0; k < row.size(); k++) {
            Node& node = nodes_[static_cast<size_t>(row[k])];
            node.pos = k == 0 ? node.along / 2.0 : nodes_[static_cast<size_t>(row[k - 1])].pos + Separation(nodes_[static_cast<size_t>(row[k - 1])], node);
        }
    }
    std::vector<double> target, weight;
    for (int sweep = 0; sweep < kPlaceSweeps; sweep++) {
        if (host_.Cancelled()) return false;
        const bool fromUp = sweep >= kPlaceSweeps - 2 || sweep % 2 == 0;
        const bool fromDown = sweep >= kPlaceSweeps - 2 || sweep % 2 == 1;
        for (int step = 0; step < layerCount; step++) {
            const int l = fromUp && !fromDown ? step : fromDown && !fromUp ? layerCount - 1 - step : step;
            const std::vector<int>& row = layers_[static_cast<size_t>(l)];
            if (row.empty()) continue;
            target.assign(row.size(), 0.0);
            weight.assign(row.size(), 0.0);
            for (size_t k = 0; k < row.size(); k++) {
                const int x = row[k];
                double sum = 0.0, mass = 0.0;
                if (fromUp) {
                    for (int i = upStart_[static_cast<size_t>(x)]; i < upStart_[static_cast<size_t>(x) + 1]; i++) {
                        sum += upWeight_[static_cast<size_t>(i)] * nodes_[static_cast<size_t>(up_[static_cast<size_t>(i)])].pos;
                        mass += upWeight_[static_cast<size_t>(i)];
                    }
                }
                if (fromDown) {
                    for (int i = downStart_[static_cast<size_t>(x)]; i < downStart_[static_cast<size_t>(x) + 1]; i++) {
                        sum += downWeight_[static_cast<size_t>(i)] * nodes_[static_cast<size_t>(down_[static_cast<size_t>(i)])].pos;
                        mass += downWeight_[static_cast<size_t>(i)];
                    }
                }
                if (mass > 0.0) {
                    target[k] = sum / mass;
                    weight[k] = mass;
                } else {
                    target[k] = nodes_[static_cast<size_t>(x)].pos;
                    weight[k] = 0.25;
                }
            }
            Place(row, target, weight);
        }
    }
    double left = 1e300, right = -1e300;
    for (const Node& node : nodes_) {
        left = std::min(left, node.pos - node.along / 2.0);
        right = std::max(right, node.pos + node.along / 2.0 + node.extra);
    }
    for (Node& node : nodes_) node.pos -= left;
    const double alongSize = right - left;

    // The rank axis: each layer as thick as its thickest node.
    std::vector<double> rank(static_cast<size_t>(layerCount), 0.0), thick(static_cast<size_t>(layerCount), 0.0);
    for (const Node& node : nodes_) thick[static_cast<size_t>(node.layer)] = std::max(thick[static_cast<size_t>(node.layer)], node.across);
    double cursor = 0.0;
    for (int l = 0; l < layerCount; l++) {
        if (l > 0) cursor += thick[static_cast<size_t>(l - 1)] / 2.0 + kRankGap + thick[static_cast<size_t>(l)] / 2.0;
        else cursor = thick[static_cast<size_t>(l)] / 2.0;
        rank[static_cast<size_t>(l)] = cursor;
    }
    bool bottomPorts = false;
    for (size_t k = 0; k < pieces.size(); k++) {
        if (f_.pieces[static_cast<size_t>(pieces[k])].bottom &&
            (f_.pieces[static_cast<size_t>(pieces[k])].a == kPort || f_.pieces[static_cast<size_t>(pieces[k])].b == kPort)) {
            bottomPorts = true;
        }
    }
    const int usedLast = bottomPorts ? bottomLayer : lastLayer;
    const double rankSize = rank[static_cast<size_t>(usedLast)] + thick[static_cast<size_t>(usedLast)] / 2.0;

    // Turned to the level's direction, inside its box.
    const double realWidth = Horizontal(dir) ? rankSize : alongSize;
    const double realHeight = Horizontal(dir) ? alongSize : rankSize;
    double ox = 0.0, oy = 0.0, boxWidth = realWidth, boxHeight = realHeight;
    if (level >= 0) {
        Vertex& cluster = f_.vertices[static_cast<size_t>(level)];
        const double title = cluster.label.text.empty() ? 0.0 : cluster.label.height + 4.0;
        boxWidth = std::max(realWidth, cluster.label.width) + 2.0 * kClusterPad;
        boxHeight = realHeight + 2.0 * kClusterPad + title;
        ox = (boxWidth - realWidth) / 2.0;
        oy = kClusterPad + title;
        cluster.width = boxWidth;
        cluster.height = boxHeight;
    } else {
        rootWidth_ = boxWidth;
        rootHeight_ = boxHeight;
    }
    auto place = [&](const Node& node) -> Point {
        const double along = node.pos, across = rank[static_cast<size_t>(node.layer)];
        switch (dir) {
        case kBT: return { ox + along, oy + rankSize - across };
        case kLR: return { ox + across, oy + along };
        case kRL: return { ox + rankSize - across, oy + along };
        default: return { ox + along, oy + across };
        }
    };
    for (size_t k = 0; k < count; k++) {
        Vertex& v = f_.vertices[static_cast<size_t>(items[k])];
        const Point centre = place(nodes_[k]);
        v.x = centre.x - v.width / 2.0;
        v.y = centre.y - v.height / 2.0;
    }

    // Each piece's points, source to target, its ends on borders and ports.
    for (size_t k = 0; k < pieces.size(); k++) {
        Piece& piece = f_.pieces[static_cast<size_t>(pieces[k])];
        std::vector<Point>& points = piece.points;
        points.clear();
        const bool forward = nodes_[static_cast<size_t>(endA[k])].layer < nodes_[static_cast<size_t>(endB[k])].layer;
        for (int at = chainStart[k]; at < chainStart[k + 1]; at++) points.push_back(place(nodes_[static_cast<size_t>(chain[static_cast<size_t>(at)])]));
        if (!forward) std::reverse(points.begin(), points.end());
        if (labelNode[k] >= 0) piece.labelAt = place(nodes_[static_cast<size_t>(labelNode[k])]);
        // This level's own port: from the border, straight to the port's node.
        for (int side = 0; side < 2; side++) {
            const int end = side == 0 ? piece.a : piece.b;
            if (end != kPort) continue;
            const Point inner = side == 0 ? points.front() : points.back();
            Point border = inner;
            const bool after = piece.bottom;
            switch (dir) {
            case kBT: border.y = after ? 0.0 : boxHeight; break;
            case kLR: border.x = after ? boxWidth : 0.0; break;
            case kRL: border.x = after ? 0.0 : boxWidth; break;
            default: border.y = after ? boxHeight : 0.0; break;
            }
            piece.port = border;
            if (side == 0) points.insert(points.begin(), border);
            else points.push_back(border);
        }
        // Subgraphs the edge goes into: their port.
        for (int side = 0; side < 2; side++) {
            const int end = side == 0 ? piece.a : piece.b;
            if (!EntersCluster(piece, end)) continue;
            const Vertex& cluster = f_.vertices[static_cast<size_t>(end)];
            const Piece* inner = PieceIn(piece.edge, end);
            const Point port{ cluster.x + inner->port.x, cluster.y + inner->port.y };
            (side == 0 ? points.front() : points.back()) = port;
        }
        // Nodes and subgraphs the edge ends at: their border.
        for (int side = 0; side < 2; side++) {
            const int end = side == 0 ? piece.a : piece.b;
            if (end < 0 || EntersCluster(piece, end)) continue;
            const Vertex& v = f_.vertices[static_cast<size_t>(end)];
            const Point toward = side == 0 ? points[1] : points[points.size() - 2];
            const int shape = v.cluster ? Geometry::kRect : v.shape;
            (side == 0 ? points.front() : points.back()) = Border(shape, v.x, v.y, v.width, v.height, toward);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Flowcharts: drawing

void DrawNode(Canvas& canvas, const Vertex& v, int index) {
    std::string& out = canvas.body;
    const Palette& p = canvas.palette;
    const double x = v.ax, y = v.ay, w = v.width, h = v.height;
    switch (v.shape) {
    case Geometry::kCircle:
        out += "<circle";
        NumAttr(out, "cx", x + w / 2.0);
        NumAttr(out, "cy", y + h / 2.0);
        NumAttr(out, "r", w / 2.0);
        ShapePaint(canvas, p.nodeFill, p.nodeStroke, 1.0, v.style);
        out += "/>\n";
        break;
    case Geometry::kCylinder: {
        const double ry = CylinderCap(w), k = 0.5523, cx = x + w / 2.0, rx = w / 2.0, x1 = x + w, y1 = y + h;
        out += "<path d=\"M";
        PointText(out, { x, y + ry });
        out += " C";
        PointText(out, { x, y + ry - k * ry });
        out += ' ';
        PointText(out, { cx - k * rx, y });
        out += ' ';
        PointText(out, { cx, y });
        out += " C";
        PointText(out, { cx + k * rx, y });
        out += ' ';
        PointText(out, { x1, y + ry - k * ry });
        out += ' ';
        PointText(out, { x1, y + ry });
        out += " L";
        PointText(out, { x1, y1 - ry });
        out += " C";
        PointText(out, { x1, y1 - ry + k * ry });
        out += ' ';
        PointText(out, { cx + k * rx, y1 });
        out += ' ';
        PointText(out, { cx, y1 });
        out += " C";
        PointText(out, { cx - k * rx, y1 });
        out += ' ';
        PointText(out, { x, y1 - ry + k * ry });
        out += ' ';
        PointText(out, { x, y1 - ry });
        out += " Z\"";
        ShapePaint(canvas, p.nodeFill, p.nodeStroke, 1.0, v.style);
        out += "/>\n<path d=\"M";
        PointText(out, { x, y + ry });
        out += " C";
        PointText(out, { x, y + ry + k * ry });
        out += ' ';
        PointText(out, { cx - k * rx, y + 2.0 * ry });
        out += ' ';
        PointText(out, { cx, y + 2.0 * ry });
        out += " C";
        PointText(out, { cx + k * rx, y + 2.0 * ry });
        out += ' ';
        PointText(out, { x1, y + ry + k * ry });
        out += ' ';
        PointText(out, { x1, y + ry });
        out += '"';
        PaintAttr(out, "fill", 0, "none");
        PaintAttr(out, "stroke", p.nodeStroke, v.style.stroke);
        NumAttr(out, "stroke-width", v.style.width >= 0.0 ? v.style.width : 1.0);
        out += "/>\n";
        break;
    }
    case Geometry::kRect:
    case Geometry::kRound:
    case Geometry::kStadium:
    case Geometry::kSubroutine:
        Rect(out, x, y, w, h);
        if (v.shape == Geometry::kRound) NumAttr(out, "rx", 5.0);
        else if (v.shape == Geometry::kStadium) NumAttr(out, "rx", h / 2.0);
        ShapePaint(canvas, p.nodeFill, p.nodeStroke, 1.0, v.style);
        out += "/>\n";
        if (v.shape == Geometry::kSubroutine) {
            out += "<path d=\"M";
            PointText(out, { x + 8.0, y });
            out += " L";
            PointText(out, { x + 8.0, y + h });
            out += " M";
            PointText(out, { x + w - 8.0, y });
            out += " L";
            PointText(out, { x + w - 8.0, y + h });
            out += '"';
            PaintAttr(out, "fill", 0, "none");
            PaintAttr(out, "stroke", p.nodeStroke, v.style.stroke);
            NumAttr(out, "stroke-width", v.style.width >= 0.0 ? v.style.width : 1.0);
            out += "/>\n";
        }
        break;
    default: {
        Point corners[6];
        const int count = Corners(v.shape, x, y, w, h, corners);
        out += "<polygon points=\"";
        for (int k = 0; k < count; k++) {
            if (k > 0) out += ' ';
            PointText(out, corners[k]);
        }
        out += '"';
        ShapePaint(canvas, p.nodeFill, p.nodeStroke, 1.0, v.style);
        out += "/>\n";
        break;
    }
    }
    double centreY = y + h / 2.0;
    if (v.shape == Geometry::kCylinder) {
        const double ry = CylinderCap(w);
        centreY = (y + 2.0 * ry + y + h - ry) / 2.0;
    }
    CentredLabel(canvas, v.label, x + w / 2.0, centreY, p.text, v.style.color, index);
    if (Geometry* geometry = canvas.geometry) geometry->nodes.push_back({ x, y, w, h, v.shape });
}

// A line through `points`: straight between two, else a Catmull-Rom spline as cubic Béziers.
// The ends are moved back by `startCut` and `endCut` along the line, where heads take over.
void EdgePath(std::string& out, std::vector<Point> points, double startCut, double endCut) {
    const size_t n = points.size();
    double ux, uy;
    if (startCut > 0.0) {
        Direction(points[1], points[0], ux, uy);
        points[0].x -= ux * startCut;
        points[0].y -= uy * startCut;
    }
    if (endCut > 0.0) {
        Direction(points[n - 2], points[n - 1], ux, uy);
        points[n - 1].x -= ux * endCut;
        points[n - 1].y -= uy * endCut;
    }
    out += "<path d=\"M";
    PointText(out, points[0]);
    if (n == 2) {
        out += " L";
        PointText(out, points[1]);
    } else {
        for (size_t k = 0; k + 1 < n; k++) {
            const Point p0 = points[k == 0 ? 0 : k - 1], p1 = points[k], p2 = points[k + 1], p3 = points[k + 2 < n ? k + 2 : n - 1];
            out += " C";
            PointText(out, { p1.x + (p2.x - p0.x) / 6.0, p1.y + (p2.y - p0.y) / 6.0 });
            out += ' ';
            PointText(out, { p2.x - (p3.x - p1.x) / 6.0, p2.y - (p3.y - p1.y) / 6.0 });
            out += ' ';
            PointText(out, p2);
        }
    }
    out += '"';
}

// The head of an edge at `tip`, coming from direction (ux, uy).
void DrawHead(std::string& out, Head head, Point tip, double ux, double uy, uint32_t rgb) {
    if (head == kArrowHead) {
        ArrowHead(out, tip, ux, uy, rgb);
    } else if (head == kCircleHead) {
        out += "<circle";
        NumAttr(out, "cx", tip.x - ux * 4.0);
        NumAttr(out, "cy", tip.y - uy * 4.0);
        NumAttr(out, "r", 4.0);
        PaintAttr(out, "fill", rgb);
        out += "/>\n";
    } else if (head == kCrossHead) {
        const Point c{ tip.x - ux * 5.0, tip.y - uy * 5.0 };
        const double s = 3.5;
        out += "<path d=\"M";
        PointText(out, { c.x - s * (ux + uy), c.y - s * (uy - ux) });
        out += " L";
        PointText(out, { c.x + s * (ux + uy), c.y + s * (uy - ux) });
        out += " M";
        PointText(out, { c.x - s * (ux - uy), c.y - s * (uy + ux) });
        out += " L";
        PointText(out, { c.x + s * (ux - uy), c.y + s * (uy + ux) });
        out += '"';
        PaintAttr(out, "fill", 0, "none");
        PaintAttr(out, "stroke", rgb);
        out += " stroke-width=\"2\"/>\n";
    }
}

double HeadCut(Head head) {
    return head == kArrowHead ? kArrowLength - 1.0 : head == kCircleHead ? 8.0 : head == kCrossHead ? 5.0 : 0.0;
}

void LineStyle(std::string& out, LineKind line, uint32_t rgb) {
    PaintAttr(out, "fill", 0, "none");
    PaintAttr(out, "stroke", rgb);
    if (line == kThick) {
        out += " stroke-width=\"3.5\"";
    } else {
        out += " stroke-width=\"2\"";
        if (line == kDotted) out += " stroke-dasharray=\"3 3\"";
    }
    out += "/>\n";
}

void EdgeLabel(Canvas& canvas, const Label& label, Point centre) {
    if (label.text.empty()) return;
    canvas.painted = true;
    std::string& out = canvas.body;
    Rect(out, centre.x - label.width / 2.0 - 2.0, centre.y - label.height / 2.0 - 2.0, label.width + 4.0, label.height + 4.0);
    PaintAttr(out, "fill", canvas.palette.clusterFill);
    out += "/>\n";
    CentredLabel(canvas, label, centre.x, centre.y, canvas.palette.text);
}

bool DrawFlow(Canvas& canvas, Flow& f, double& width, double& height) {
    FlowLayout layout(f, canvas.host);
    double contentWidth, contentHeight;
    if (!layout.Run(contentWidth, contentHeight)) return false;
    width = contentWidth + 2.0 * kMargin;
    height = contentHeight + 2.0 * kMargin;
    const Palette& p = canvas.palette;
    std::string& out = canvas.body;
    f.nodeIndex.assign(f.vertices.size(), -1);
    int drawn = 0;
    for (size_t index = 0; index < f.vertices.size(); index++) {
        if (!f.vertices[index].cluster) f.nodeIndex[index] = drawn++;
    }

    // Subgraphs, outer ones first.
    for (int index : f.clusterOrder) {
        const Vertex& v = f.vertices[static_cast<size_t>(index)];
        Rect(out, v.ax, v.ay, v.width, v.height);
        ShapePaint(canvas, p.clusterFill, p.clusterStroke, 1.0, v.style);
        out += "/>\n";
        LabelLines(canvas, v.label, v.ax + v.width / 2.0, v.ay + kClusterPad / 2.0 + 2.0, kMiddle, p.text, v.style.color);
        if (canvas.geometry) canvas.geometry->clusters.push_back({ v.ax, v.ay, v.width, v.height, -1 });
    }
    // Edges, their heads, then their labels over them.
    for (const Edge& edge : f.edges) {
        if (edge.from == edge.to || edge.line == kInvisible || edge.points.size() < 2) continue;
        const std::vector<Point>& points = edge.points;
        EdgePath(out, points, HeadCut(edge.startHead), HeadCut(edge.endHead));
        LineStyle(out, edge.line, p.line);
        canvas.painted = true;
        double ux, uy;
        Direction(points[points.size() - 2], points.back(), ux, uy);
        DrawHead(out, edge.endHead, points.back(), ux, uy, p.line);
        Direction(points[1], points[0], ux, uy);
        DrawHead(out, edge.startHead, points[0], ux, uy, p.line);
        if (Geometry* geometry = canvas.geometry) {
            const Vertex& a = f.vertices[static_cast<size_t>(edge.from)];
            const Vertex& b = f.vertices[static_cast<size_t>(edge.to)];
            geometry->ends.push_back({ points.front().x, points.front().y, a.cluster ? -1 : f.nodeIndex[static_cast<size_t>(edge.from)] });
            geometry->ends.push_back({ points.back().x, points.back().y, b.cluster ? -1 : f.nodeIndex[static_cast<size_t>(edge.to)] });
        }
    }
    // Self loops, on the side after the node on its level's order axis.
    for (size_t index = 0; index < f.vertices.size(); index++) {
        Vertex& v = f.vertices[index];
        if (v.loops == 0) continue;
        const Dir dir = v.parent < 0 ? f.dir : f.vertices[static_cast<size_t>(v.parent)].flow;
        const bool below = Horizontal(dir);
        int nth = 0;
        double labelCursor = below ? v.ay + v.height + kLoopReach + 12.0 * (v.loops - 1) + 2.0
                                   : v.ay + v.height / 2.0 - v.loopLabelHeight / 2.0;
        for (const Edge& edge : f.edges) {
            if (edge.from != static_cast<int>(index) || edge.to != edge.from) continue;
            const double reach = kLoopReach + 12.0 * nth++;
            const double cx = v.ax + v.width / 2.0, cy = v.ay + v.height / 2.0;
            Point start, end, c1, c2;
            if (below) {
                start = Border(v.shape, v.ax, v.ay, v.width, v.height, { cx - v.width / 4.0, cy + v.height });
                end = Border(v.shape, v.ax, v.ay, v.width, v.height, { cx + v.width / 4.0, cy + v.height });
                const double far = v.ay + v.height + reach / 0.75;
                c1 = { start.x - 10.0, far };
                c2 = { end.x + 10.0, far };
            } else {
                start = Border(v.shape, v.ax, v.ay, v.width, v.height, { cx + v.width, cy - v.height / 4.0 });
                end = Border(v.shape, v.ax, v.ay, v.width, v.height, { cx + v.width, cy + v.height / 4.0 });
                const double far = v.ax + v.width + reach / 0.75;
                c1 = { far, start.y - 10.0 };
                c2 = { far, end.y + 10.0 };
            }
            if (edge.line != kInvisible) {
                double ux, uy;
                Direction(c2, end, ux, uy);
                const Point cut{ end.x - ux * HeadCut(edge.endHead), end.y - uy * HeadCut(edge.endHead) };
                out += "<path d=\"M";
                PointText(out, start);
                out += " C";
                PointText(out, c1);
                out += ' ';
                PointText(out, c2);
                out += ' ';
                PointText(out, cut);
                out += '"';
                LineStyle(out, edge.line, p.line);
                canvas.painted = true;
                DrawHead(out, edge.endHead, end, ux, uy, p.line);
                Direction(c1, start, ux, uy);
                DrawHead(out, edge.startHead, start, ux, uy, p.line);
                if (Geometry* geometry = canvas.geometry) {
                    geometry->ends.push_back({ start.x, start.y, f.nodeIndex[index] });
                    geometry->ends.push_back({ end.x, end.y, f.nodeIndex[index] });
                }
            }
            if (!edge.label.text.empty()) {
                if (below) {
                    EdgeLabel(canvas, edge.label, { cx, labelCursor + edge.label.height / 2.0 + 2.0 });
                    labelCursor += edge.label.height + 4.0;
                } else {
                    const double x = v.ax + v.width + kLoopReach + 12.0 * (v.loops - 1) + 4.0 + edge.label.width / 2.0;
                    EdgeLabel(canvas, edge.label, { x, labelCursor + edge.label.height / 2.0 + 2.0 });
                    labelCursor += edge.label.height + 4.0;
                }
            }
        }
    }
    for (const Edge& edge : f.edges) {
        if (edge.from != edge.to && edge.hasText) EdgeLabel(canvas, edge.label, edge.labelAt);
    }
    for (size_t index = 0; index < f.vertices.size(); index++) {
        const Vertex& v = f.vertices[index];
        if (!v.cluster) DrawNode(canvas, v, f.nodeIndex[index]);
    }
    return true;
}

bool Flowchart(Canvas& canvas, Cursor cursor, double& width, double& height) {
    Flow f;
    // The header's direction.
    cursor.SkipBlanks();
    const std::string_view word = cursor.Word();
    if (!word.empty()) {
        if (!ParseDir(word, f.dir)) return false;
        cursor.at += word.size();
    }
    cursor.SkipBlanks();
    if (!cursor.AtStatementEnd(true)) return false;
    if (!FlowReader(f, cursor).Read()) return false;

    // Styles: the default class, the classes in source order, then style statements.
    const auto defaults = f.classes.find("default");
    std::vector<Style> own(f.vertices.size());
    for (size_t index = 0; index < f.vertices.size(); index++) {
        Vertex& v = f.vertices[index];
        own[index] = v.style;
        v.style = Style();
        if (!v.cluster && defaults != f.classes.end()) v.style = defaults->second;
    }
    for (const auto& [vertex, name] : f.classUses) {
        const auto found = f.classes.find(name);
        if (found != f.classes.end()) MergeStyle(f.vertices[static_cast<size_t>(vertex)].style, found->second);
    }
    for (size_t index = 0; index < f.vertices.size(); index++) MergeStyle(f.vertices[index].style, own[index]);

    for (Vertex& v : f.vertices) {
        if (!SetLabel(v.hasText ? v.text : v.id, kFontSize, canvas.host, v.label)) return false;
        if (!v.cluster) SizeNode(v);
    }
    for (Edge& edge : f.edges) {
        if (edge.hasText && !SetLabel(edge.text, kEdgeFontSize, canvas.host, edge.label)) return false;
    }
    return DrawFlow(canvas, f, width, height);
}

// ---------------------------------------------------------------------------------------------
// Sequence diagrams

constexpr size_t kMaxParticipants = 64;
constexpr size_t kMaxStatements = 2000;      // messages, notes, frame lines, activations
constexpr size_t kMaxFrameDepth = 16;
constexpr double kTitleSize = 16.0;
constexpr double kLaneGap = 50.0;            // between two participants' boxes
constexpr double kActivationHalf = 5.0;      // half an activation box's width; nested ones move this much

enum SeqKind : uint8_t { kSeqMessage, kSeqNote, kSeqFrame, kSeqSection, kSeqEnd, kSeqActivate, kSeqDeactivate };
enum SeqHead : uint8_t { kSeqNoHead, kSeqArrow, kSeqCross, kSeqOpen };
enum NotePlace : uint8_t { kLeftOf, kRightOf, kOver };
enum FrameKind : uint8_t { kLoop, kAlt, kOpt, kPar, kCritical, kBreak, kRectFrame };

const char* const kFrameWords[] = { "loop", "alt", "opt", "par", "critical", "break", "" };

struct Participant {
    std::string_view name;
    std::string_view text;
    bool actor = false;
    Label label;
    double width = 0.0;
    double x = 0.0;                    // its lifeline
    std::vector<double> active;        // the tops of its open activations
};

struct SeqStatement {
    SeqKind kind = kSeqMessage;
    int a = -1, b = -1;                // participants: from and to, or a note's first and last
    bool dotted = false;
    SeqHead head = kSeqNoHead;
    int activation = 0;                // +1 activates b, -1 deactivates a
    NotePlace place = kOver;
    FrameKind frame = kLoop;
    int number = 0;                    // its autonumber, 0 for none
    std::string text;                  // as drawn: a frame's condition in brackets
    std::string_view color;            // a rect frame's
    Label label;
};

struct Sequence {
    std::vector<Participant> participants;
    std::unordered_map<std::string_view, int> names;
    std::vector<SeqStatement> statements;
    std::string_view title;
};

// Whether a participant's name may hold `ch`: Mermaid's names end at these.
bool IsNameStop(char ch) {
    return ch == '<' || ch == '=' || ch == '>' || ch == '-' || ch == ':' || ch == ',' || ch == ';' || ch == '\n';
}

bool IsName(std::string_view name) {
    if (name.empty()) return false;
    for (char ch : name) {
        if (IsNameStop(ch)) return false;
    }
    return true;
}

class SequenceReader {
public:
    SequenceReader(Sequence& sequence, Cursor cursor) : s_(sequence), c_(cursor) {}

    bool Read() {
        for (;;) {
            c_.SkipBlanks();
            if (c_.Done()) break;
            if (*c_.at == '\n' || *c_.at == ';') {
                c_.at++;
                continue;
            }
            if (c_.StartsWith("%%")) {
                c_.Line();
                continue;
            }
            const char* start = c_.at;
            while (!c_.AtStatementEnd(true)) c_.at++;
            if (!Statement(Trim(std::string_view(start, static_cast<size_t>(c_.at - start))))) return false;
        }
        return frames_.empty() && !s_.participants.empty();
    }

private:
    // The statement's first word when a blank, ':' or its end follows, else empty.
    static std::string_view Keyword(std::string_view statement) {
        size_t end = 0;
        while (end < statement.size() && IsAlpha(statement[end])) end++;
        if (end < statement.size() && !IsBlank(statement[end]) && statement[end] != ':') return {};
        return statement.substr(0, end);
    }

    bool Find(std::string_view name, int& index) {
        name = Trim(name);
        if (!IsName(name)) return false;
        const auto found = s_.names.find(name);
        if (found != s_.names.end()) {
            index = found->second;
            return true;
        }
        if (s_.participants.size() >= kMaxParticipants) return false;
        index = static_cast<int>(s_.participants.size());
        s_.participants.emplace_back();
        s_.participants.back().name = name;
        s_.participants.back().text = name;
        s_.names.emplace(name, index);
        return true;
    }

    bool Add(SeqStatement&& statement) {
        if (s_.statements.size() >= kMaxStatements) return false;
        s_.statements.push_back(std::move(statement));
        return true;
    }

    bool Statement(std::string_view statement) {
        const std::string_view word = Keyword(statement);
        const std::string_view rest = Trim(statement.substr(word.size()));
        if (word == "participant" || word == "actor") {
            std::string_view name = rest, alias;
            for (size_t at = 0; at + 4 <= rest.size(); at++) {
                if (IsBlank(rest[at]) && rest.substr(at + 1, 2) == "as" && IsBlank(rest[at + 3])) {
                    name = rest.substr(0, at);
                    alias = Trim(rest.substr(at + 4));
                    break;
                }
            }
            int index;
            if (!Find(name, index)) return false;
            Participant& p = s_.participants[static_cast<size_t>(index)];
            p.actor = word == "actor";
            if (!alias.empty()) p.text = alias;
            return true;
        }
        if (word == "activate" || word == "deactivate") {
            SeqStatement item;
            item.kind = word == "activate" ? kSeqActivate : kSeqDeactivate;
            return Find(rest, item.a) && Add(std::move(item));
        }
        if (EqualsNoCase(word, "note")) return Note(rest);
        for (int kind = kLoop; kind <= kBreak; kind++) {
            if (word == kFrameWords[kind]) return Frame(static_cast<FrameKind>(kind), rest);
        }
        if (word == "rect") {
            if (!IsPaintValue(rest)) return false;
            SeqStatement item;
            item.kind = kSeqFrame;
            item.frame = kRectFrame;
            item.color = rest;
            frames_.push_back(kRectFrame);
            return frames_.size() <= kMaxFrameDepth && Add(std::move(item));
        }
        if (word == "else" || word == "and" || word == "option") {
            const FrameKind owner = word == "else" ? kAlt : word == "and" ? kPar : kCritical;
            if (frames_.empty() || frames_.back() != owner) return false;
            SeqStatement item;
            item.kind = kSeqSection;
            item.frame = owner;
            if (!rest.empty()) item.text = "[" + std::string(rest) + "]";
            return Add(std::move(item));
        }
        if (word == "end" && rest.empty()) {
            if (frames_.empty()) return false;
            frames_.pop_back();
            SeqStatement item;
            item.kind = kSeqEnd;
            return Add(std::move(item));
        }
        if (word == "autonumber") return Autonumber(rest);
        if (word == "title") {
            s_.title = rest.size() > 0 && rest.front() == ':' ? Trim(rest.substr(1)) : rest;
            return true;
        }
        if (word == "accTitle" || word == "accDescr") return true;   // read past (a statement)
        return Message(statement);
    }

    bool Frame(FrameKind kind, std::string_view text) {
        frames_.push_back(kind);
        if (frames_.size() > kMaxFrameDepth) return false;
        SeqStatement item;
        item.kind = kSeqFrame;
        item.frame = kind;
        if (!text.empty()) item.text = "[" + std::string(text) + "]";
        return Add(std::move(item));
    }

    bool Note(std::string_view rest) {
        SeqStatement item;
        item.kind = kSeqNote;
        auto take = [&](std::string_view words) {
            if (rest.size() < words.size() || !EqualsNoCase(rest.substr(0, words.size()), words)) return false;
            if (rest.size() > words.size() && !IsBlank(rest[words.size()])) return false;
            rest = Trim(rest.substr(words.size()));
            return true;
        };
        if (take("left of")) item.place = kLeftOf;
        else if (take("right of")) item.place = kRightOf;
        else if (take("over")) item.place = kOver;
        else return false;
        const size_t colon = rest.find(':');
        if (colon == std::string_view::npos) return false;
        const std::string_view names = rest.substr(0, colon);
        const size_t comma = names.find(',');
        if (comma == std::string_view::npos) {
            if (!Find(names, item.a)) return false;
            item.b = item.a;
        } else {
            if (item.place != kOver || !Find(names.substr(0, comma), item.a) || !Find(names.substr(comma + 1), item.b)) return false;
        }
        item.text = std::string(Trim(rest.substr(colon + 1)));
        return Add(std::move(item));
    }

    static bool Integer(std::string_view text, int& value) {
        if (text.empty() || text.size() > 6) return false;
        value = 0;
        for (char ch : text) {
            if (!IsDigit(ch)) return false;
            value = value * 10 + (ch - '0');
        }
        return true;
    }

    bool Autonumber(std::string_view rest) {
        if (rest == "off") {
            step_ = 0;
            return true;
        }
        next_ = 1;
        step_ = 1;
        if (rest.empty()) return true;
        size_t blank = 0;
        while (blank < rest.size() && !IsBlank(rest[blank])) blank++;
        if (!Integer(rest.substr(0, blank), next_)) return false;
        const std::string_view second = Trim(rest.substr(blank));
        return second.empty() || (Integer(second, step_) && step_ > 0);
    }

    // "A->>B: text" and the other arrows, with an activation mark after the arrow.
    bool Message(std::string_view statement) {
        SeqStatement item;
        size_t at = 0;
        while (at < statement.size() && !IsNameStop(statement[at])) at++;
        if (at == statement.size() || statement[at] != '-' || !Find(statement.substr(0, at), item.a)) return false;
        size_t dashes = 0;
        while (at < statement.size() && statement[at] == '-') {
            at++;
            dashes++;
        }
        if (dashes > 2 || at == statement.size()) return false;
        item.dotted = dashes == 2;
        const std::string_view arrow = statement.substr(at);
        if (arrow.substr(0, 2) == ">>") {
            item.head = kSeqArrow;
            at += 2;
        } else if (arrow[0] == '>') {
            item.head = kSeqNoHead;
            at++;
        } else if (arrow[0] == 'x') {
            item.head = kSeqCross;
            at++;
        } else if (arrow[0] == ')') {
            item.head = kSeqOpen;
            at++;
        } else {
            return false;
        }
        while (at < statement.size() && IsBlank(statement[at])) at++;
        if (at < statement.size() && (statement[at] == '+' || statement[at] == '-')) {
            item.activation = statement[at] == '+' ? 1 : -1;
            at++;
        }
        const size_t nameStart = at;
        while (at < statement.size() && !IsNameStop(statement[at])) at++;
        if (at < statement.size() && statement[at] != ':') return false;
        if (!Find(statement.substr(nameStart, at - nameStart), item.b)) return false;
        if (at < statement.size()) item.text = std::string(Trim(statement.substr(at + 1)));
        if (step_ > 0) {
            item.number = next_;
            if (next_ > 1000000) return false;
            next_ += step_;
        }
        return Add(std::move(item));
    }

    Sequence& s_;
    Cursor c_;
    std::vector<FrameKind> frames_;   // open frames, innermost last
    int next_ = 1, step_ = 0;         // autonumber: the next number, 0 steps when off
};

// What the walk down the rows places, drawn afterwards in layers.
struct SeqMessageBox {
    const SeqStatement* statement;
    double from, to, line, labelTop;   // the ends' x, the line's y, the label's top
    bool self;
};

struct SeqFrameBox {
    const SeqStatement* statement;
    double x0, x1, y0, y1;
    std::vector<std::pair<double, const SeqStatement*>> sections;   // the y of each divider
};

struct SeqActivationBox {
    int lane;
    double x, y0, y1;
};

struct SeqNoteBox {
    const SeqStatement* statement;
    double x, y, width, height;
};

// The x where a message meets a lifeline, on the side towards `toward`: the edge of its open
// activation, or the line.
double LifelineEdge(const Participant& p, double toward) {
    if (p.active.empty()) return p.x;
    const double centre = p.x + kActivationHalf * static_cast<double>(p.active.size() - 1);
    return toward >= centre ? centre + kActivationHalf : centre - kActivationHalf;
}

void OpenHead(std::string& out, Point tip, double ux, uint32_t rgb) {
    out += "<path d=\"M";
    PointText(out, { tip.x - ux * 9.0, tip.y - 5.0 });
    out += " L";
    PointText(out, tip);
    out += " L";
    PointText(out, { tip.x - ux * 9.0, tip.y + 5.0 });
    out += '"';
    PaintAttr(out, "fill", 0, "none");
    PaintAttr(out, "stroke", rgb);
    out += " stroke-width=\"1.5\"/>\n";
}

void Line(std::string& out, Point a, Point b, uint32_t rgb, double width, const char* dash = nullptr) {
    out += "<path d=\"M";
    PointText(out, a);
    out += " L";
    PointText(out, b);
    out += '"';
    PaintAttr(out, "fill", 0, "none");
    PaintAttr(out, "stroke", rgb);
    NumAttr(out, "stroke-width", width);
    if (dash) {
        out += " stroke-dasharray=\"";
        out += dash;
        out += '"';
    }
    out += "/>\n";
}

bool DrawSequence(Canvas& canvas, Cursor cursor, double& width, double& height) {
    Sequence s;
    cursor.SkipBlanks();
    if (!cursor.AtStatementEnd(true)) return false;
    if (!SequenceReader(s, cursor).Read()) return false;
    SvgHost& host = canvas.host;
    const Palette& pal = canvas.palette;

    // Sizes.
    double rowHeight = 0.0;
    for (Participant& p : s.participants) {
        if (!SetLabel(p.text, kFontSize, host, p.label)) return false;
        p.width = p.actor ? std::max(p.label.width + 20.0, 60.0) : std::max(p.label.width + 30.0, 90.0);
        rowHeight = std::max(rowHeight, p.actor ? 44.0 + p.label.height + 2.0 : std::max(p.label.height + 20.0, 40.0));
    }
    for (SeqStatement& item : s.statements) {
        const double size = item.kind == kSeqFrame || item.kind == kSeqSection ? kEdgeFontSize : kFontSize;
        if (!SetLabel(item.text, size, host, item.label)) return false;
    }
    Label title;
    if (!SetLabel(s.title, kTitleSize, host, title)) return false;
    const double numberRadius = 9.0;

    // Lanes: the space between neighbours is the widest need of what lies between them, needs
    // taken by span, the narrowest first.
    const size_t lanes = s.participants.size();
    std::vector<double> gaps(lanes > 1 ? lanes - 1 : 0, 0.0);
    for (size_t k = 0; k + 1 < lanes; k++) gaps[k] = s.participants[k].width / 2.0 + kLaneGap + s.participants[k + 1].width / 2.0;
    struct Need {
        size_t lo, hi;
        double width;
    };
    std::vector<Need> needs;
    double leftExtra = 0.0, rightExtra = 0.0;
    auto need = [&](int lo, int hi, double size) {
        if (lo < 0) leftExtra = std::max(leftExtra, size);
        else if (static_cast<size_t>(hi) >= lanes) rightExtra = std::max(rightExtra, size);
        else if (lo < hi) needs.push_back({ static_cast<size_t>(lo), static_cast<size_t>(hi), size });
    };
    for (const SeqStatement& item : s.statements) {
        const double noteWidth = item.label.width + 20.0;
        if (item.kind == kSeqMessage) {
            const double number = item.number > 0 ? 2.0 * numberRadius : 0.0;
            if (item.a == item.b) need(item.a, item.a + 1, std::max(item.label.width + 10.0, 40.0) + 16.0 + number);
            else need(std::min(item.a, item.b), std::max(item.a, item.b), item.label.width + 24.0 + number);
        } else if (item.kind == kSeqNote) {
            if (item.place == kRightOf) need(item.a, item.a + 1, noteWidth + 20.0);
            else if (item.place == kLeftOf) need(item.a - 1, item.a, noteWidth + 20.0);
            else if (item.a == item.b) {
                need(item.a - 1, item.a, noteWidth / 2.0 + 10.0);
                need(item.a, item.a + 1, noteWidth / 2.0 + 10.0);
            } else {
                need(std::min(item.a, item.b), std::max(item.a, item.b), noteWidth - 40.0);
            }
        }
    }
    std::stable_sort(needs.begin(), needs.end(), [](const Need& p, const Need& q) { return p.hi - p.lo < q.hi - q.lo; });
    for (const Need& item : needs) {
        double span = 0.0;
        for (size_t k = item.lo; k < item.hi; k++) span += gaps[k];
        if (span < item.width) gaps[item.hi - 1] += item.width - span;
    }
    double x = 0.0;
    for (size_t k = 0; k < lanes; k++) {
        if (k > 0) x += gaps[k - 1];
        s.participants[k].x = x;
    }
    double minX = -s.participants[0].width / 2.0 - leftExtra;
    double maxX = s.participants[lanes - 1].x + s.participants[lanes - 1].width / 2.0 + rightExtra;

    // Rows, top down.
    const double top = kMargin + (title.text.empty() ? 0.0 : title.height + 10.0);
    double y = top + rowHeight + 20.0;
    std::vector<SeqMessageBox> messages;
    std::vector<SeqFrameBox> frames;
    std::vector<SeqActivationBox> activations;
    std::vector<SeqNoteBox> notes;
    struct Open {
        size_t frame;
        double lo, hi;
    };
    std::vector<Open> open;
    auto extend = [&](double lo, double hi) {
        minX = std::min(minX, lo);
        maxX = std::max(maxX, hi);
        if (!open.empty()) {
            open.back().lo = std::min(open.back().lo, lo);
            open.back().hi = std::max(open.back().hi, hi);
        }
    };
    double lastLine = 0.0;
    bool afterMessage = false;
    auto close = [&](Participant& p, int lane, double end) {
        const double centre = p.x + kActivationHalf * static_cast<double>(p.active.size() - 1);
        activations.push_back({ lane, centre, p.active.back(), std::max(end, p.active.back() + 10.0) });
        p.active.pop_back();
    };
    for (const SeqStatement& item : s.statements) {
        switch (item.kind) {
        case kSeqMessage: {
            Participant& a = s.participants[static_cast<size_t>(item.a)];
            Participant& b = s.participants[static_cast<size_t>(item.b)];
            const double line = y + item.label.height + 8.0;
            SeqMessageBox box{ &item, 0.0, 0.0, line, y, item.a == item.b };
            if (box.self) {
                box.from = LifelineEdge(a, a.x + 1e9);
                if (item.activation > 0) b.active.push_back(line);
                box.to = LifelineEdge(b, b.x + 1e9);
                extend(box.from - (item.number > 0 ? numberRadius : 0.0), std::max(box.from + 40.0, box.from + 6.0 + item.label.width));
                y = line + 20.0 + 14.0;
            } else {
                box.from = LifelineEdge(a, b.x);
                if (item.activation > 0) b.active.push_back(line);
                box.to = LifelineEdge(b, a.x);
                const double centre = (box.from + box.to) / 2.0;
                extend(std::min(box.from, box.to) - (item.number > 0 ? numberRadius : 0.0), std::max(box.from, box.to));
                extend(centre - item.label.width / 2.0, centre + item.label.width / 2.0);
                y = line + 14.0;
            }
            if (item.activation < 0) {
                if (a.active.empty()) return false;
                close(a, item.a, line);
            }
            messages.push_back(box);
            lastLine = line;
            afterMessage = true;
            break;
        }
        case kSeqNote: {
            const Participant& a = s.participants[static_cast<size_t>(item.a)];
            const Participant& b = s.participants[static_cast<size_t>(item.b)];
            const double noteWidth = item.label.width + 20.0, noteHeight = item.label.height + 12.0;
            double left;
            double w = noteWidth;
            if (item.place == kRightOf) {
                left = a.x + 10.0;
            } else if (item.place == kLeftOf) {
                left = a.x - 10.0 - noteWidth;
            } else {
                const double lo = std::min(a.x, b.x) - (item.a == item.b ? 0.0 : 20.0);
                const double hi = std::max(a.x, b.x) + (item.a == item.b ? 0.0 : 20.0);
                w = std::max(noteWidth, hi - lo);
                left = (lo + hi) / 2.0 - w / 2.0;
            }
            notes.push_back({ &item, left, y, w, noteHeight });
            extend(left, left + w);
            y += noteHeight + 10.0;
            afterMessage = false;
            break;
        }
        case kSeqFrame:
            frames.push_back({ &item, 0.0, 0.0, y, 0.0, {} });
            open.push_back({ frames.size() - 1, 1e300, -1e300 });
            if (item.frame == kRectFrame) y += 10.0;
            else y += std::max(LineHeight(kEdgeFontSize) + 6.0, item.label.height + 6.0) + 10.0;
            afterMessage = false;
            break;
        case kSeqSection: {
            SeqFrameBox& frame = frames[open.back().frame];
            frame.sections.emplace_back(y + 2.0, &item);
            y += 2.0 + 3.0 + item.label.height + 10.0;
            afterMessage = false;
            break;
        }
        case kSeqEnd: {
            const Open done = open.back();
            open.pop_back();
            SeqFrameBox& frame = frames[done.frame];
            y += 6.0;
            frame.y1 = y;
            double lo = done.lo, hi = done.hi;
            if (lo > hi) {
                lo = s.participants[0].x - s.participants[0].width / 2.0;
                hi = s.participants[lanes - 1].x + s.participants[lanes - 1].width / 2.0;
            }
            frame.x0 = lo - 10.0;
            frame.x1 = hi + 10.0;
            const SeqStatement& head = *frame.statement;
            if (head.frame != kRectFrame) {
                const double word = host.TextWidth(kFrameWords[head.frame], kEdgeFontSize, false) + 16.0;
                frame.x1 = std::max(frame.x1, frame.x0 + word + 8.0 + head.label.width + 10.0);
            }
            for (const auto& section : frame.sections) frame.x1 = std::max(frame.x1, frame.x0 + section.second->label.width + 20.0);
            extend(frame.x0, frame.x1);
            y += 12.0;
            afterMessage = false;
            break;
        }
        case kSeqActivate:
            s.participants[static_cast<size_t>(item.a)].active.push_back(afterMessage ? lastLine : y);
            break;
        case kSeqDeactivate: {
            Participant& p = s.participants[static_cast<size_t>(item.a)];
            if (p.active.empty()) return false;
            close(p, item.a, afterMessage ? lastLine : y);
            break;
        }
        }
        if (canvas.host.Cancelled()) return false;
    }
    y += 10.0;
    for (size_t k = 0; k < lanes; k++) {
        Participant& p = s.participants[k];
        while (!p.active.empty()) close(p, static_cast<int>(k), y);
    }
    const double bottom = y + 10.0;
    if (!title.text.empty() && maxX - minX < title.width) {
        const double grow = (title.width - (maxX - minX)) / 2.0;
        minX -= grow;
        maxX += grow;
    }
    const double dx = kMargin - minX;
    width = maxX - minX + 2.0 * kMargin;
    height = bottom + rowHeight + kMargin;

    std::string& out = canvas.body;
    canvas.painted = true;   // participants and lifelines in palette colours
    if (!title.text.empty()) LabelLines(canvas, title, width / 2.0, kMargin, kMiddle, pal.text);
    // Backgrounds, lifelines, participants, activations, frames, notes, messages.
    for (const SeqFrameBox& frame : frames) {
        if (frame.statement->frame != kRectFrame) continue;
        Rect(out, frame.x0 + dx, frame.y0, frame.x1 - frame.x0, frame.y1 - frame.y0);
        PaintAttr(out, "fill", 0, frame.statement->color);
        out += "/>\n";
    }
    for (const Participant& p : s.participants) Line(out, { p.x + dx, top + rowHeight }, { p.x + dx, bottom }, pal.line, 0.5);
    for (int row = 0; row < 2; row++) {
        const double boxTop = row == 0 ? top : bottom;
        for (size_t k = 0; k < lanes; k++) {
            const Participant& p = s.participants[k];
            const double cx = p.x + dx, left = cx - p.width / 2.0;
            const int node = row == 0 ? static_cast<int>(k) : -1;
            if (p.actor) {
                out += "<circle";
                NumAttr(out, "cx", cx);
                NumAttr(out, "cy", boxTop + 9.0);
                NumAttr(out, "r", 8.0);
                PaintAttr(out, "fill", pal.nodeFill);
                PaintAttr(out, "stroke", pal.nodeStroke);
                out += " stroke-width=\"1.5\"/>\n<path d=\"M";
                PointText(out, { cx, boxTop + 17.0 });
                out += " L";
                PointText(out, { cx, boxTop + 31.0 });
                out += " M";
                PointText(out, { cx - 12.0, boxTop + 22.0 });
                out += " L";
                PointText(out, { cx + 12.0, boxTop + 22.0 });
                out += " M";
                PointText(out, { cx - 10.0, boxTop + 43.0 });
                out += " L";
                PointText(out, { cx, boxTop + 31.0 });
                out += " L";
                PointText(out, { cx + 10.0, boxTop + 43.0 });
                out += '"';
                PaintAttr(out, "fill", 0, "none");
                PaintAttr(out, "stroke", pal.nodeStroke);
                out += " stroke-width=\"1.5\"/>\n";
                LabelLines(canvas, p.label, cx, boxTop + 45.0, kMiddle, pal.text, {}, node);
            } else {
                Rect(out, left, boxTop, p.width, rowHeight);
                NumAttr(out, "rx", 3.0);
                PaintAttr(out, "fill", pal.nodeFill);
                PaintAttr(out, "stroke", pal.nodeStroke);
                out += "/>\n";
                CentredLabel(canvas, p.label, cx, boxTop + rowHeight / 2.0, pal.text, {}, node);
            }
            if (row == 0 && canvas.geometry) canvas.geometry->nodes.push_back({ left, boxTop, p.width, rowHeight, Geometry::kRect });
        }
    }
    for (const SeqActivationBox& box : activations) {
        Rect(out, box.x + dx - kActivationHalf, box.y0, 2.0 * kActivationHalf, box.y1 - box.y0);
        PaintAttr(out, "fill", pal.clusterFill);
        PaintAttr(out, "stroke", pal.nodeStroke);
        out += "/>\n";
    }
    for (const SeqFrameBox& frame : frames) {
        if (frame.statement->frame == kRectFrame) continue;
        const double x0 = frame.x0 + dx, x1 = frame.x1 + dx;
        Rect(out, x0, frame.y0, x1 - x0, frame.y1 - frame.y0);
        PaintAttr(out, "fill", 0, "none");
        PaintAttr(out, "stroke", pal.clusterStroke);
        out += "/>\n";
        const char* word = kFrameWords[frame.statement->frame];
        const double tagWidth = host.TextWidth(word, kEdgeFontSize, false) + 16.0;
        const double tagHeight = LineHeight(kEdgeFontSize) + 6.0;
        out += "<polygon points=\"";
        PointText(out, { x0, frame.y0 });
        out += ' ';
        PointText(out, { x0 + tagWidth, frame.y0 });
        out += ' ';
        PointText(out, { x0 + tagWidth, frame.y0 + tagHeight - 5.0 });
        out += ' ';
        PointText(out, { x0 + tagWidth - 5.0, frame.y0 + tagHeight });
        out += ' ';
        PointText(out, { x0, frame.y0 + tagHeight });
        out += '"';
        PaintAttr(out, "fill", pal.clusterFill);
        PaintAttr(out, "stroke", pal.clusterStroke);
        out += "/>\n";
        const double wordTop = frame.y0 + 3.0;
        TextLine(canvas, word, x0 + 8.0, wordTop + (LineHeight(kEdgeFontSize) - kEdgeFontSize) / 2.0 + 0.8 * kEdgeFontSize,
                 kEdgeFontSize, kStart, pal.text, {}, -1);
        LabelLines(canvas, frame.statement->label, x0 + tagWidth + 8.0, wordTop, kStart, pal.text);
        for (const auto& [lineY, section] : frame.sections) {
            Line(out, { x0, lineY }, { x1, lineY }, pal.clusterStroke, 1.0, "3 3");
            LabelLines(canvas, section->label, (x0 + x1) / 2.0, lineY + 3.0, kMiddle, pal.text);
        }
        if (canvas.geometry) canvas.geometry->clusters.push_back({ x0, frame.y0, x1 - x0, frame.y1 - frame.y0, -1 });
    }
    for (const SeqFrameBox& frame : frames) {
        if (frame.statement->frame == kRectFrame && canvas.geometry) {
            canvas.geometry->clusters.push_back({ frame.x0 + dx, frame.y0, frame.x1 - frame.x0, frame.y1 - frame.y0, -1 });
        }
    }
    for (const SeqNoteBox& note : notes) {
        Rect(out, note.x + dx, note.y, note.width, note.height);
        PaintAttr(out, "fill", pal.noteFill);
        PaintAttr(out, "stroke", pal.clusterStroke);
        out += "/>\n";
        CentredLabel(canvas, note.statement->label, note.x + dx + note.width / 2.0, note.y + note.height / 2.0, pal.text);
        if (canvas.geometry) canvas.geometry->clusters.push_back({ note.x + dx, note.y, note.width, note.height, -1 });
    }
    for (const SeqMessageBox& box : messages) {
        const SeqStatement& item = *box.statement;
        const double from = box.from + dx, to = box.to + dx;
        const char* dash = item.dotted ? "3 3" : nullptr;
        Point tip;
        double ux;
        if (box.self) {
            const double far = std::max(from, to) + 40.0, low = box.line + 20.0;
            tip = { to, low };
            ux = -1.0;
            const double cut = item.head == kSeqArrow ? kArrowLength - 1.0 : 0.0;
            out += "<path d=\"M";
            PointText(out, { from, box.line });
            out += " L";
            PointText(out, { far, box.line });
            out += " L";
            PointText(out, { far, low });
            out += " L";
            PointText(out, { to + cut, low });
            out += '"';
            PaintAttr(out, "fill", 0, "none");
            PaintAttr(out, "stroke", pal.line);
            out += " stroke-width=\"1.5\"";
            if (dash) out += " stroke-dasharray=\"3 3\"";
            out += "/>\n";
            LabelLines(canvas, item.label, from + 6.0, box.labelTop, kStart, pal.text);
        } else {
            tip = { to, box.line };
            ux = to >= from ? 1.0 : -1.0;
            const double cut = item.head == kSeqArrow ? kArrowLength - 1.0 : 0.0;
            Line(out, { from, box.line }, { to - ux * cut, box.line }, pal.line, 1.5, dash);
            LabelLines(canvas, item.label, (from + to) / 2.0, box.labelTop, kMiddle, pal.text);
        }
        if (item.head == kSeqArrow) ArrowHead(out, tip, ux, 0.0, pal.line);
        else if (item.head == kSeqCross) DrawHead(out, kCrossHead, { tip.x + ux * 5.0, tip.y }, ux, 0.0, pal.line);
        else if (item.head == kSeqOpen) OpenHead(out, tip, ux, pal.line);
        if (item.number > 0) {
            out += "<circle";
            NumAttr(out, "cx", from);
            NumAttr(out, "cy", box.line);
            NumAttr(out, "r", numberRadius);
            PaintAttr(out, "fill", pal.accent);
            out += "/>\n";
            char digits[12];
            int count = 0;
            for (int value = item.number; value > 0 && count < 11; value /= 10) digits[count++] = static_cast<char>('0' + value % 10);
            std::string number;
            while (count > 0) number += digits[--count];
            TextLine(canvas, number, from, box.line + 0.3 * kEdgeFontSize, kEdgeFontSize * (number.size() > 2 ? 0.75 : 1.0), kMiddle,
                     pal.nodeFill, {}, -1);
        }
        if (canvas.geometry) {
            canvas.geometry->ends.push_back({ from, box.line, -1 });
            canvas.geometry->ends.push_back({ box.self ? to : tip.x, box.self ? box.line + 20.0 : box.line, -1 });
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Pie charts

constexpr size_t kMaxSlices = 64;
constexpr double kPieRadius = 150.0;
constexpr double kPi = 3.14159265358979323846;

// sin and cos of 2 pi turns without libm: a quarter turn's Taylor series to the 19th power
// (error below 1e-13), turned into its quadrant.
void SinCosTurns(double turns, double& sine, double& cosine) {
    turns -= RayoMd::ExactMath::Floor(turns);
    const double scaled = turns * 4.0;
    int quadrant = static_cast<int>(scaled);
    if (quadrant > 3) quadrant = 3;
    const double a = (scaled - quadrant) * (kPi / 2.0), a2 = a * a;
    double s = a, c = 1.0, term = a, cterm = 1.0;
    for (int k = 1; k <= 9; k++) {
        term *= -a2 / static_cast<double>((2 * k) * (2 * k + 1));
        cterm *= -a2 / static_cast<double>((2 * k - 1) * (2 * k));
        s += term;
        c += cterm;
    }
    switch (quadrant) {
    case 1: sine = c; cosine = -s; break;
    case 2: sine = -s; cosine = -c; break;
    case 3: sine = -c; cosine = s; break;
    default: sine = s; cosine = c; break;
    }
}

uint32_t Mix(uint32_t a, uint32_t b, double t) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (a >> shift) & 255u, cb = (b >> shift) & 255u;
        out |= static_cast<uint32_t>(LlRound(ca + (cb - ca) * t)) << shift;
    }
    return out;
}

// The colour of slice `index`: accent and clusterStroke in turn, their channels rotated
// (r, g, b to g, b, r), then mixed towards nodeFill.
uint32_t SliceColour(const Palette& palette, size_t index) {
    uint32_t base = index % 2 == 0 ? palette.accent : palette.clusterStroke;
    for (size_t turn = 0; turn < (index / 2) % 3; turn++) base = ((base << 8) | (base >> 16)) & 0xFFFFFFu;
    static const double kTints[] = { 0.0, 0.45, 0.7 };
    return Mix(base, palette.nodeFill, kTints[(index / 6) % 3]);
}

struct Slice {
    std::string_view label, value;
    double amount = 0.0;
    std::string legendText;
    Label legend;
};

// A non-negative decimal: digits with an optional fraction, at most 15 digits.
bool ParseAmount(std::string_view text, double& value) {
    if (text.empty() || text.size() > 16) return false;
    double whole = 0.0, scale = 1.0;
    bool point = false, digit = false;
    for (char ch : text) {
        if (ch == '.' && !point) {
            point = true;
        } else if (IsDigit(ch)) {
            digit = true;
            if (point) {
                scale /= 10.0;
                whole += (ch - '0') * scale;
            } else {
                whole = whole * 10.0 + (ch - '0');
            }
        } else {
            return false;
        }
    }
    value = whole;
    return digit;
}

bool DrawPie(Canvas& canvas, Cursor cursor, double& width, double& height) {
    std::string_view titleText;
    bool showData = false;
    // The header's options: showData, then title and its text.
    for (;;) {
        cursor.SkipBlanks();
        if (TakeKeyword(cursor, "showData", false)) {
            showData = true;
            continue;
        }
        if (TakeKeyword(cursor, "title", false)) {
            const char* start = cursor.at;
            while (!cursor.Done() && *cursor.at != '\n') cursor.at++;
            titleText = Trim(std::string_view(start, static_cast<size_t>(cursor.at - start)));
            break;
        }
        break;
    }
    cursor.SkipBlanks();
    if (!cursor.AtStatementEnd(false)) return false;
    std::vector<Slice> slices;
    while (!cursor.Done()) {
        const std::string_view line = Trim(cursor.Line());
        if (line.empty() || line.substr(0, 2) == "%%") continue;
        Cursor c{ line.data(), line.data() + line.size() };
        if (TakeKeyword(c, "title", false)) {
            titleText = Trim(std::string_view(c.at, static_cast<size_t>(c.end - c.at)));
            continue;
        }
        if (TakeKeyword(c, "showData", false) && c.Done()) {
            showData = true;
            continue;
        }
        if (line.substr(0, 8) == "accTitle" || line.substr(0, 8) == "accDescr") {
            if (line.find(':') == std::string_view::npos) return false;
            continue;
        }
        if (line.front() != '"') return false;
        const size_t close = line.find('"', 1);
        if (close == std::string_view::npos) return false;
        std::string_view rest = Trim(line.substr(close + 1));
        if (rest.empty() || rest.front() != ':') return false;
        Slice slice;
        slice.label = line.substr(1, close - 1);
        slice.value = Trim(rest.substr(1));
        if (!ParseAmount(slice.value, slice.amount) || slices.size() >= kMaxSlices) return false;
        slices.push_back(std::move(slice));
    }
    double total = 0.0;
    for (const Slice& slice : slices) total += slice.amount;
    if (!(total > 0.0)) return false;

    SvgHost& host = canvas.host;
    const Palette& pal = canvas.palette;
    Label title;
    if (!SetLabel(titleText, kTitleSize, host, title)) return false;
    double legendWidth = 0.0;
    for (Slice& slice : slices) {
        slice.legendText = std::string(slice.label);
        if (showData) {
            slice.legendText += " [";
            slice.legendText += slice.value;
            slice.legendText += ']';
        }
        if (!SetLabel(slice.legendText, kFontSize, host, slice.legend)) return false;
        legendWidth = std::max(legendWidth, slice.legend.width);
    }
    const double rowHeight = 22.0;
    const double legendHeight = rowHeight * static_cast<double>(slices.size());
    const double r = kPieRadius;
    const double top = kMargin + (title.text.empty() ? 0.0 : title.height + 10.0);
    const double chartHeight = std::max(2.0 * r, legendHeight);
    const double cx = kMargin + r, cy = top + chartHeight / 2.0;
    const double legendX = cx + r + 40.0;
    width = std::max(legendX + 20.0 + legendWidth, kMargin + title.width) + kMargin;
    height = top + chartHeight + kMargin;
    std::string& out = canvas.body;
    canvas.painted = true;   // slices and the legend in palette colours
    if (!title.text.empty()) LabelLines(canvas, title, width / 2.0, kMargin, kMiddle, pal.text);

    auto point = [&](double turns, double radius) -> Point {
        double sine, cosine;
        SinCosTurns(turns, sine, cosine);
        return { cx + radius * sine, cy - radius * cosine };
    };
    double start = 0.0;
    for (size_t index = 0; index < slices.size(); index++) {
        const double fraction = slices[index].amount / total;
        if (fraction <= 0.0) continue;
        const uint32_t colour = SliceColour(pal, index);
        if (fraction >= 1.0 - 1e-12) {
            out += "<circle";
            NumAttr(out, "cx", cx);
            NumAttr(out, "cy", cy);
            NumAttr(out, "r", r);
        } else {
            // The arc as cubic Béziers of at most a quarter turn: control points 4/3 tan(a/4) out
            // along the tangents.
            const int pieces = static_cast<int>(RayoMd::ExactMath::Ceil(fraction * 4.0 - 1e-9));
            out += "<path d=\"M";
            PointText(out, { cx, cy });
            out += " L";
            PointText(out, point(start, r));
            for (int k = 0; k < pieces; k++) {
                const double from = start + fraction * k / pieces, to = start + fraction * (k + 1) / pieces;
                double s4, c4, s0, c0, s1, c1;
                SinCosTurns((to - from) / 4.0, s4, c4);
                SinCosTurns(from, s0, c0);
                SinCosTurns(to, s1, c1);
                const double kappa = 4.0 / 3.0 * s4 / c4 * r;
                out += " C";
                PointText(out, { cx + r * s0 + kappa * c0, cy - r * c0 + kappa * s0 });
                out += ' ';
                PointText(out, { cx + r * s1 - kappa * c1, cy - r * c1 - kappa * s1 });
                out += ' ';
                PointText(out, point(to, r));
            }
            out += " Z\"";
        }
        PaintAttr(out, "fill", colour);
        PaintAttr(out, "stroke", pal.line);
        out += " stroke-width=\"1\"/>\n";
        start += fraction;
    }
    if (canvas.geometry) canvas.geometry->clusters.push_back({ cx - r, cy - r, 2.0 * r, 2.0 * r, -1 });
    // Percentages inside the slices wide enough for them.
    start = 0.0;
    for (const Slice& slice : slices) {
        const double fraction = slice.amount / total;
        const long long percent = LlRound(fraction * 100.0);
        std::string text;
        if (percent >= 100) text = "100";
        else if (percent >= 10) text = { static_cast<char>('0' + percent / 10), static_cast<char>('0' + percent % 10) };
        else text = std::string(1, static_cast<char>('0' + percent));
        text += '%';
        const double textWidth = host.TextWidth(text, kFontSize, false);
        if (fraction > 0.0 && fraction * 2.0 * kPi * 0.75 * r >= textWidth + 8.0) {
            const Point at = point(start + fraction / 2.0, 0.75 * r);
            TextLine(canvas, text, at.x, at.y + 0.3 * kFontSize, kFontSize, kMiddle, pal.text, {}, -1);
        }
        start += fraction;
    }
    for (size_t index = 0; index < slices.size(); index++) {
        const double rowTop = cy - legendHeight / 2.0 + rowHeight * static_cast<double>(index);
        Rect(out, legendX, rowTop + 4.0, 14.0, 14.0);
        PaintAttr(out, "fill", SliceColour(pal, index));
        PaintAttr(out, "stroke", pal.line);
        out += " stroke-width=\"0.5\"/>\n";
        LabelLines(canvas, slices[index].legend, legendX + 20.0, rowTop + (rowHeight - LineHeight(kFontSize)) / 2.0, kStart, pal.text);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The diagram types

// The first statement: after blank lines, comments and a front matter block.
bool Header(Cursor& cursor, std::string_view& keyword) {
    bool first = true;
    while (!cursor.Done()) {
        Cursor line = cursor;
        line.SkipBlanks();
        if (line.AtStatementEnd(false)) {
            cursor.Line();
            continue;
        }
        if (line.StartsWith("%%")) {
            cursor.Line();
            first = false;
            continue;
        }
        if (first && Trim(Cursor(line).Line()) == "---") {
            // Front matter (a title, a config): read past, not drawn.
            cursor.Line();
            for (;;) {
                if (cursor.Done()) return false;
                if (Trim(cursor.Line()) == "---") break;
            }
            first = false;
            continue;
        }
        cursor = line;
        const char* start = cursor.at;
        while (!cursor.Done() && (IsAlpha(*cursor.at) || IsDigit(*cursor.at) || *cursor.at == '-')) cursor.at++;
        keyword = std::string_view(start, static_cast<size_t>(cursor.at - start));
        return !keyword.empty();
    }
    return false;
}

} // namespace

bool ToSvg(std::string_view source, SvgHost& host, const Palette& palette, std::string& svg, Geometry* geometry) {
    svg.clear();
    if (geometry) *geometry = Geometry();
    if (source.size() > kMaxSourceBytes || !IsXmlText(source)) return false;
    Cursor cursor{ source.data(), source.data() + source.size() };
    std::string_view keyword;
    if (!Header(cursor, keyword)) return false;
    Canvas canvas{ host, palette, std::string(), geometry, 0 };
    double width = 0.0, height = 0.0;
    bool drawn = false;
    if (keyword == "flowchart" || keyword == "graph") drawn = Flowchart(canvas, cursor, width, height);
    else if (keyword == "sequenceDiagram") drawn = DrawSequence(canvas, cursor, width, height);
    else if (keyword == "pie") drawn = DrawPie(canvas, cursor, width, height);
    // Too large for a content stream's numbers: drawn smaller, the same picture, unless that
    // leaves less than a pixel across.
    const double scale = drawn && width > 0.0 && height > 0.0 && width <= 1e9 && height <= 1e9 ? std::min(1.0, std::min(kMaxExtent / width, kMaxExtent / height)) : 0.0;
    if (!drawn || !canvas.painted || canvas.textBytes > kMaxTextBytes || !(width * scale >= 1.0 && height * scale >= 1.0)) {
        if (geometry) *geometry = Geometry();
        return false;
    }
    svg.reserve(canvas.body.size() + 200);
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    NumAttr(svg, "width", width * scale);
    NumAttr(svg, "height", height * scale);
    svg += " viewBox=\"0 0 ";
    Num(svg, width);
    svg += ' ';
    Num(svg, height);
    svg += "\">\n";
    svg += canvas.body;
    svg += "</svg>\n";
    if (svg.size() > kMaxSvgBytes) {
        svg.clear();
        if (geometry) *geometry = Geometry();
        return false;
    }
    return true;
}

bool ToSvg(std::string_view source, SvgHost& host, const Palette& palette, std::string& svg) {
    return ToSvg(source, host, palette, svg, nullptr);
}

} // namespace TinyPdf::Internal::Mermaid
