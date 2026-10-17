// SPDX-License-Identifier: Apache-2.0

// Needs <chrono>, <iostream>, <string>, <string_view>, <vector>, src/core/mermaid.h, src/core/svg.h,
// src/core/rayomd_pdf_source.h and Bounded of parse_limits_tests.inc.
// Mermaid diagrams (docs/development/native_mermaid.md): what each syntax gives, what is refused,
// the layout's invariants, golden digests of the SVG, that every SVG converts, and the limits.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace mermaid_tests {

using TinyPdf::Internal::Mermaid::Geometry;

// Text 0.6 em a character, as a monospaced font: the same SVG on every platform.
class FixedHost : public TinyPdf::Internal::SvgHost {
public:
    double TextWidth(std::string_view text, double size, bool) override {
        size_t characters = 0;
        for (char ch : text) characters += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
        return 0.6 * size * static_cast<double>(characters);
    }
    void AppendText(std::string& out, std::string_view text, bool) override {
        out += '(';
        for (char ch : text) {
            if (ch == '(' || ch == ')' || ch == '\\') out += '\\';
            out += ch;
        }
        out += ')';
    }
    bool HasBoldFace() const override { return false; }
    int AddImage(std::string_view, int&, int&) override { return -1; }
};

bool Draw(std::string_view source, std::string& svg, Geometry* geometry = nullptr) {
    FixedHost host;
    return TinyPdf::Internal::Mermaid::ToSvg(source, host, TinyPdf::Internal::Mermaid::kDefaultPalette, svg, geometry);
}

// Whether the SVG of a diagram converts into a form that draws something.
bool Converts(const std::string& svg) {
    FixedHost host;
    TinyPdf::Internal::SvgForm form;
    return TinyPdf::Internal::ConvertSvg(svg, host, form) && !form.content.empty() && form.width > 0.0 && form.height > 0.0;
}

bool Contains(const std::string& text, std::string_view piece) { return text.find(piece) != std::string::npos; }

size_t Count(const std::string& text, std::string_view piece) {
    size_t count = 0;
    for (size_t at = text.find(piece); at != std::string::npos; at = text.find(piece, at + 1)) count++;
    return count;
}

// Accepted diagrams: each drawn, converted, and its SVG holding the given pieces.
struct Accepted {
    const char* name;
    const char* source;
    std::vector<const char*> pieces;
};

bool CheckAccepted(const std::vector<Accepted>& cases) {
    for (const Accepted& item : cases) {
        std::string svg;
        if (!Draw(item.source, svg)) {
            std::cerr << "Mermaid refused " << item.name << std::endl;
            return false;
        }
        if (!Converts(svg)) {
            std::cerr << "Mermaid SVG does not convert: " << item.name << "\n" << svg << std::endl;
            return false;
        }
        for (const char* piece : item.pieces) {
            if (!Contains(svg, piece)) {
                std::cerr << "Mermaid " << item.name << " lacks " << piece << "\n" << svg << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool CheckRefused(const std::vector<std::pair<const char*, std::string>>& cases) {
    for (const auto& [name, source] : cases) {
        std::string svg = "stale";
        Geometry geometry;
        geometry.nodes.push_back({ 0, 0, 1, 1, 0 });
        if (Draw(source, svg, &geometry) || !svg.empty() || !geometry.nodes.empty()) {
            std::cerr << "Mermaid drew what it must refuse: " << name << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckFlowchartSyntax() {
    const std::vector<Accepted> cases = {
        { "graph TD", "graph TD\n  A --> B\n", { ">A</text>", ">B</text>", "<polygon" } },
        { "flowchart without direction", "flowchart\nA-->B", { ">A</text>" } },
        { "semicolons", "graph LR; A-->B; B-->C;", { ">C</text>" } },
        { "every shape",
          "flowchart TD\n a[rect] --> b(round) --> c([stadium]) --> d[[sub]] --> e[(db)] --> f((circle))\n"
          " f --> g{rhombus} --> h{{hex}} --> i[/lean right/] --> j[\\lean left\\] --> k[/trapezoid\\] --> l[\\alt/]\n",
          { ">rect<", ">round<", ">stadium<", ">sub<", ">db<", ">circle<", ">rhombus<", ">hex<", ">lean right<", ">lean left<",
            ">trapezoid<", ">alt<", "<circle", "rx=\"5\"" } },
        { "every link",
          "flowchart LR\n a --> b\n a --- c\n a -.-> d\n a -.- e\n a ==> f\n a === g\n a --x h\n a --o i\n a <--> j\n"
          " a x--x k\n a o--o l\n a ~~~ m\n a ---> n\n a -..-> o\n a ====> p\n",
          { "stroke-dasharray=\"3 3\"", "stroke-width=\"3.5\"", " r=\"4\"" } },
        { "labels on links",
          "flowchart TD\n A -->|yes| B\n A -- no --> C\n A == thick ==> D\n A -. dots .-> E\n A ---|plain| F\n A -- open --- G\n"
          " A -->|\"quoted | bar\"| H\n",
          { ">yes<", ">no<", ">thick<", ">dots<", ">plain<", ">open<", ">quoted | bar<" } },
        { "chains and &", "flowchart TD\n A --> B --> C\n D & E --> F & G\n", { ">G<" } },
        { "quoted labels and escaping",
          "flowchart TD\n A[\"a [b] (c) {d}\"] --> B(\"x < y & z\")\n C{\"<b>not bold</b>\"}\n",
          { ">a [b] (c) {d}<", ">x &lt; y &amp; z<", ">&lt;b&gt;not bold&lt;/b&gt;<" } },
        { "line breaks", "flowchart TD\n A[first<br>second<BR/>third<br />fourth]\n", { ">first<", ">second<", ">third<", ">fourth<" } },
        { "comments and ignored statements",
          "%% a comment before\nflowchart TD\n%% a comment\n  A --> B\n  linkStyle 0 stroke:#f00\n  click A callback\n"
          "  accTitle: A title\n  accDescr {\n many\n lines\n }\n",
          { ">A<", ">B<" } },
        { "front matter", "---\ntitle: Ignored\nconfig:\n  theme: dark\n---\nflowchart LR\n A --> B\n", { ">B<" } },
        { "ids with dashes and UTF-8", "flowchart TD\n my-node --> \xC5\xBC\xC3\xB3\xC5\x82w\n", { ">my-node<", ">\xC5\xBC\xC3\xB3\xC5\x82w<" } },
        { "subgraphs",
          "flowchart TB\n c1-->a2\n subgraph one\n a1-->a2\n end\n subgraph two [The second]\n b1-->b2\n end\n"
          " subgraph \"Third one\"\n c1-->c2\n end\n one --> two\n",
          { ">one<", ">The second<", ">Third one<", "fill=\"#ffffde\"" } },
        { "nested subgraphs with directions",
          "flowchart LR\n subgraph TOP\n direction TB\n subgraph B1\n direction RL\n i1 -->f1\n end\n subgraph B2\n direction BT\n"
          " i2 -->f2\n end\n end\n A --> TOP --> B\n B1 --> B2\n",
          { ">TOP<", ">B1<", ">B2<" } },
        { "styles",
          "flowchart LR\n A:::hot --> B --> C\n classDef hot fill:#f96,stroke:#333,stroke-width:4px\n classDef cool,calm fill:rgb(1, 2, 3)\n"
          " class B cool\n style C fill:#bbf,stroke:#f66,stroke-width:2px,color:#fff,stroke-dasharray: 5 5,font-weight:bold\n"
          " classDef default stroke:green\n",
          { "fill=\"#f96\"", "stroke=\"#333\"", "stroke-width=\"4\"", "fill=\"rgb(1, 2, 3)\"", "fill=\"#bbf\"", "stroke-dasharray=\"5 5\"",
            "fill=\"#fff\">C<", "stroke=\"green\"" } },
        { "subgraph style", "flowchart TD\n subgraph S [Styled]\n A\n end\n style S fill:#eee,stroke:#999\n", { "fill=\"#eee\"" } },
        { "self loop", "flowchart TD\n A --> A\n A -->|again| A\n", { ">again<" } },
        { "an edge to a subgraph and out of it", "flowchart TD\n subgraph S\n x --> y\n end\n a --> S\n S --> b\n y --> b\n a --> x\n", { ">S<" } },
        { "empty subgraph", "flowchart TD\n subgraph E [Empty]\n end\n A\n", { ">Empty<" } },
    };
    if (!CheckAccepted(cases)) return false;

    // What the statements make: the nodes and edge ends, the shapes, the label lines.
    Geometry geometry;
    std::string svg;
    if (!Draw("flowchart TD\n A & B --> C & D\n", svg, &geometry) || geometry.nodes.size() != 4 || geometry.ends.size() != 8 ||
        Count(svg, "<polygon") != 4) {
        std::cerr << "Mermaid '&' does not make every edge" << std::endl;
        return false;
    }
    if (!Draw("flowchart TD\n a[x] --> b(x) --> c([x]) --> d[[x]] --> e[(x)] --> f((x)) --> g{x} --> h{{x}} --> i[/x/] --> j[\\x\\]"
              " --> k[/x\\] --> l[\\x/]\n", svg, &geometry) || geometry.nodes.size() != 12) {
        std::cerr << "Mermaid shapes not drawn" << std::endl;
        return false;
    }
    for (int k = 0; k < 12; k++) {
        if (geometry.nodes[static_cast<size_t>(k)].shape != k) {
            std::cerr << "Mermaid shape " << k << " read as " << geometry.nodes[static_cast<size_t>(k)].shape << std::endl;
            return false;
        }
    }
    if (!Draw("flowchart TD\n A[one<br>two<br>three]\n A[\"redefined<br>here\"]\n", svg, &geometry) || geometry.texts.size() != 2 ||
        !Contains(svg, ">redefined<") || Contains(svg, ">one<")) {
        std::cerr << "Mermaid label lines or redefinition wrong" << std::endl;
        return false;
    }
    // Directions: the target below, above, right or left of the source.
    const struct {
        const char* source;
        int axis;    // 0 x, 1 y
        int sign;
    } directions[] = {
        { "flowchart TD\nA-->B", 1, 1 }, { "flowchart TB\nA-->B", 1, 1 }, { "flowchart BT\nA-->B", 1, -1 },
        { "flowchart LR\nA-->B", 0, 1 }, { "flowchart RL\nA-->B", 0, -1 }, { "graph TD\ndirection LR\nA-->B", 0, 1 },
    };
    for (const auto& item : directions) {
        if (!Draw(item.source, svg, &geometry) || geometry.nodes.size() != 2) return false;
        const Geometry::Box& a = geometry.nodes[0];
        const Geometry::Box& b = geometry.nodes[1];
        const double delta = item.axis == 0 ? b.x - a.x : b.y - a.y;
        if (delta * item.sign <= 0.0) {
            std::cerr << "Mermaid direction wrong: " << item.source << std::endl;
            return false;
        }
    }
    // A subgraph that edges cross keeps its parent's direction; one they do not, its own.
    if (!Draw("flowchart TD\n subgraph S\n direction LR\n p --> q\n end\n", svg, &geometry) || geometry.nodes.size() != 2 ||
        !(geometry.nodes[1].x > geometry.nodes[0].x + geometry.nodes[0].width)) {
        std::cerr << "Mermaid subgraph direction not applied" << std::endl;
        return false;
    }
    if (!Draw("flowchart TD\n subgraph S\n direction LR\n p --> q\n end\n r --> p\n", svg, &geometry) || geometry.nodes.size() != 3 ||
        !(geometry.nodes[1].y > geometry.nodes[0].y + geometry.nodes[0].height)) {
        std::cerr << "Mermaid crossed subgraph keeps its own direction" << std::endl;
        return false;
    }
    return true;
}

std::vector<std::pair<const char*, std::string>> RefusedFlowcharts() {
    std::string manyLines = "flowchart TD\n A[";
    for (int k = 0; k < 40; k++) manyLines += "line<br>";
    manyLines += "]\n";
    std::string deep = "flowchart TD\n";
    for (int k = 0; k < 13; k++) deep += "subgraph s" + std::to_string(k) + "\n";
    deep += "x\n";
    for (int k = 0; k < 13; k++) deep += "end\n";
    std::string nodes = "flowchart LR\n";
    for (int k = 0; k < 501; k++) nodes += "a" + std::to_string(k) + " --> b" + std::to_string(k) + "\n";
    return {
        { "empty", "" },
        { "only comments", "%% nothing\n%% here\n" },
        { "a header alone", "flowchart TD\n" },
        { "unknown type", "classDiagram\n class A\n" },
        { "xychart", "xychart-beta\n title \"x\"\n x-axis [a, b]\n bar [1, 2]\n" },
        { "state diagram", "stateDiagram-v2\n [*] --> A\n" },
        { "gantt", "gantt\n title x\n" },
        { "unknown direction", "flowchart XY\n A --> B\n" },
        { "flowchart-elk", "flowchart-elk TD\n A --> B\n" },
        { "asymmetric shape", "flowchart TD\n A>flag] --> B\n" },
        { "double circle", "flowchart TD\n A(((x))) --> B\n" },
        { "new shape syntax", "flowchart TD\n A@{ shape: rect }\n" },
        { "unclosed bracket", "flowchart TD\n A[open --> B\n" },
        { "unclosed quote", "flowchart TD\n A[\"open] --> B\n" },
        { "link without target", "flowchart TD\n A -->\n" },
        { "half a link", "flowchart TD\n A - B\n" },
        { "start head without end", "flowchart TD\n A <-- B\n" },
        { "two labels", "flowchart TD\n A -- one -->|two| B\n" },
        { "unterminated subgraph", "flowchart TD\n subgraph S\n A --> B\n" },
        { "end without subgraph", "flowchart TD\n A --> B\n end\n" },
        { "subgraph declared twice", "flowchart TD\n subgraph S\n A\n end\n subgraph S\n B\n end\n" },
        { "edge into its own subgraph", "flowchart TD\n subgraph S\n A\n end\n S --> A\n" },
        { "edge out of its own subgraph", "flowchart TD\n subgraph S\n A\n end\n A --> S\n" },
        { "self loop on a subgraph", "flowchart TD\n subgraph S\n A\n end\n S --> S\n" },
        { "shape on a subgraph", "flowchart TD\n subgraph S\n A\n end\n S[box]\n" },
        { "invalid UTF-8", "flowchart TD\n A[\xC3\x28] --> B\n" },
        { "overlong UTF-8", "flowchart TD\n A[\xC0\xAF] --> B\n" },
        { "a surrogate", "flowchart TD\n A[\xED\xA0\x80] --> B\n" },
        { "a control character", std::string("flowchart TD\n A[a\x01" "b] --> B\n") },
        { "a NUL", std::string("flowchart TD\n A --> B\n") + std::string(1, '\0') },
        { "a label past 2,048 bytes", "flowchart TD\n A[" + std::string(2049, 'x') + "]\n" },
        { "a label of 41 lines", manyLines },
        { "13 nested subgraphs", deep },
        { "1,002 nodes", nodes },
        { "a source past 128 KiB", "flowchart TD\n A --> B\n%%" + std::string(128 << 10, 'x') + "\n" },
        { "unfinished front matter", "---\ntitle: x\nflowchart TD\n A --> B\n" },
        { "a bad style statement", "flowchart TD\n A\n style\n" },
    };
}

// ---- Layout invariants

double Distance(double x0, double y0, double x1, double y1) {
    return std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
}

// The corners of a polygonal node, as the shapes are documented; none for the round ones.
std::vector<std::pair<double, double>> Polygon(const Geometry::Box& b) {
    const double x = b.x, y = b.y, x1 = b.x + b.width, y1 = b.y + b.height, cx = x + b.width / 2, cy = y + b.height / 2;
    const double s = b.height / 2;
    switch (b.shape) {
    case Geometry::kRhombus: return { { cx, y }, { x1, cy }, { cx, y1 }, { x, cy } };
    case Geometry::kHexagon: return { { x + b.height / 4, y }, { x1 - b.height / 4, y }, { x1, cy }, { x1 - b.height / 4, y1 }, { x + b.height / 4, y1 }, { x, cy } };
    case Geometry::kLeanRight: return { { x + s, y }, { x1, y }, { x1 - s, y1 }, { x, y1 } };
    case Geometry::kLeanLeft: return { { x, y }, { x1 - s, y }, { x1, y1 }, { x + s, y1 } };
    case Geometry::kTrapezoid: return { { x + s / 2, y }, { x1 - s / 2, y }, { x1, y1 }, { x, y1 } };
    case Geometry::kTrapezoidAlt: return { { x, y }, { x1, y }, { x1 - s / 2, y1 }, { x + s / 2, y1 } };
    case Geometry::kCircle:
    case Geometry::kStadium: return {};
    default: return { { x, y }, { x1, y }, { x1, y1 }, { x, y1 } };
    }
}

double SegmentDistance(double px, double py, std::pair<double, double> a, std::pair<double, double> b) {
    const double ex = b.first - a.first, ey = b.second - a.second;
    double t = ((px - a.first) * ex + (py - a.second) * ey) / (ex * ex + ey * ey);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return Distance(px, py, a.first + t * ex, a.second + t * ey);
}

// How far (x, y) is from the outline of a node.
double OffBorder(const Geometry::Box& b, double x, double y) {
    const double cx = b.x + b.width / 2, cy = b.y + b.height / 2;
    if (b.shape == Geometry::kCircle) return std::fabs(Distance(x, y, cx, cy) - b.width / 2);
    if (b.shape == Geometry::kStadium) {
        const double r = b.height / 2;
        double best = 1e9;
        if (x >= b.x + r && x <= b.x + b.width - r) best = std::min(std::fabs(y - b.y), std::fabs(y - b.y - b.height));
        best = std::min(best, std::fabs(Distance(x, y, b.x + r, cy) - r));
        return std::min(best, std::fabs(Distance(x, y, b.x + b.width - r, cy) - r));
    }
    const std::vector<std::pair<double, double>> corners = Polygon(b);
    double best = 1e9;
    for (size_t k = 0; k < corners.size(); k++) best = std::min(best, SegmentDistance(x, y, corners[k], corners[(k + 1) % corners.size()]));
    return best;
}

// Whether (x, y) is inside a node's outline, `slack` px allowed.
bool Inside(const Geometry::Box& b, double x, double y, double slack) {
    const double cx = b.x + b.width / 2, cy = b.y + b.height / 2;
    if (b.shape == Geometry::kCircle) return Distance(x, y, cx, cy) <= b.width / 2 + slack;
    if (b.shape == Geometry::kStadium) {
        const double r = b.height / 2;
        if (y < b.y - slack || y > b.y + b.height + slack) return false;
        if (x >= b.x + r && x <= b.x + b.width - r) return true;
        return Distance(x, y, x < cx ? b.x + r : b.x + b.width - r, cy) <= r + slack;
    }
    const std::vector<std::pair<double, double>> corners = Polygon(b);
    for (size_t k = 0; k < corners.size(); k++) {
        const auto a = corners[k], c = corners[(k + 1) % corners.size()];
        const double cross = (c.first - a.first) * (y - a.second) - (c.second - a.second) * (x - a.first);
        const double length = Distance(a.first, a.second, c.first, c.second);
        if (cross < -slack * length) return false;   // clockwise in a y-down frame: inside is on the right
    }
    return true;
}

bool Overlap(const Geometry::Box& a, const Geometry::Box& b) {
    const double slack = 0.01;
    return a.x + a.width > b.x + slack && b.x + b.width > a.x + slack && a.y + a.height > b.y + slack && b.y + b.height > a.y + slack;
}

bool Within(const Geometry::Box& inner, const Geometry::Box& outer) {
    const double slack = 0.01;
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack && inner.x + inner.width <= outer.x + outer.width + slack &&
           inner.y + inner.height <= outer.y + outer.height + slack;
}

// No two nodes overlap, subgraphs nest or stay apart and hold no part of a node they do not
// hold whole, each edge ends on its node's outline, every node's text is inside its outline,
// edge labels touch no node.
bool CheckInvariants(const char* name, std::string_view source) {
    Geometry g;
    std::string svg;
    if (!Draw(source, svg, &g) || !Converts(svg)) {
        std::cerr << "Mermaid layout case not drawn: " << name << std::endl;
        return false;
    }
    auto fail = [&](const char* what) {
        std::cerr << "Mermaid layout of " << name << ": " << what << "\n" << svg << std::endl;
        return false;
    };
    for (size_t a = 0; a < g.nodes.size(); a++) {
        for (size_t b = a + 1; b < g.nodes.size(); b++) {
            if (Overlap(g.nodes[a], g.nodes[b])) return fail("nodes overlap");
        }
        for (const Geometry::Box& cluster : g.clusters) {
            if (Overlap(g.nodes[a], cluster) && !Within(g.nodes[a], cluster)) return fail("a node crosses a subgraph's border");
        }
    }
    for (size_t a = 0; a < g.clusters.size(); a++) {
        for (size_t b = a + 1; b < g.clusters.size(); b++) {
            if (Overlap(g.clusters[a], g.clusters[b]) && !Within(g.clusters[a], g.clusters[b]) && !Within(g.clusters[b], g.clusters[a])) {
                return fail("subgraphs overlap");
            }
        }
    }
    for (const Geometry::End& end : g.ends) {
        if (end.node < 0) continue;
        if (OffBorder(g.nodes[static_cast<size_t>(end.node)], end.x, end.y) > 0.5) return fail("an edge does not end on its node");
    }
    for (size_t k = 0; k < g.texts.size(); k++) {
        const Geometry::Box& t = g.texts[k];
        const int node = g.textNode[k];
        if (node < 0) {
            for (const Geometry::Box& n : g.nodes) {
                if (Overlap(t, n)) return fail("an edge label overlaps a node");
            }
            continue;
        }
        const Geometry::Box& n = g.nodes[static_cast<size_t>(node)];
        for (int corner = 0; corner < 4; corner++) {
            const double x = t.x + (corner & 1 ? t.width : 0), y = t.y + (corner & 2 ? t.height : 0);
            if (!Inside(n, x, y, 0.01)) return fail("text leaves its node");
        }
    }
    return true;
}

bool CheckFlowchartLayout() {
    const std::pair<const char*, const char*> cases[] = {
        { "decision", "flowchart TD\n A[Christmas] -->|Get money| B(Go shopping)\n B --> C{Let me think}\n C -->|One| D[Laptop]\n"
                      " C -->|Two| E[iPhone]\n C -->|Three| F[Car]\n" },
        { "shapes TD", "flowchart TD\n a[rect] --> b(round) --> c([stadium]) --> d[[sub]] --> e[(db)] --> f((circle))\n"
                       " f --> g{rhombus} --> h{{hex}} --> i[/lean right/] --> j[\\lean left\\] --> k[/trapezoid\\] --> l[\\alt/]\n"
                       " a --> g\n c --> h\n e --> j\n a --> l\n" },
        { "shapes LR", "flowchart LR\n a[rect] --> b(round) & c([stadium]) & d[[sub]] & e[(db)] & f((circle)) & g{rhombus}\n"
                       " h{{hex}} --> i[/lean right/] & j[\\lean left\\] & k[/trapezoid\\] & l[\\alt/]\n g --> h\n" },
        { "multi-line text", "flowchart BT\n A[one<br>two<br>three<br>four<br>five<br>six] --> B{a<br>tall<br>decision<br>here}\n"
                             " B --> C[/slanted<br>and<br>tall/] --> D((round<br>and<br>tall))\n B -->|a long<br>edge label| D\n" },
        { "subgraphs", "flowchart TB\n c1-->a2\n subgraph one\n a1-->a2\n end\n subgraph two\n b1-->b2\n end\n subgraph three\n c1-->c2\n end\n"
                       " one --> two\n three --> two\n two --> c2\n" },
        { "nested subgraphs", "flowchart LR\n subgraph TOP\n direction TB\n subgraph B1\n direction RL\n i1 -->f1\n end\n subgraph B2\n"
                              " direction BT\n i2 -->f2\n end\n end\n A --> TOP --> B\n B1 --> B2\n" },
        { "edges through levels", "flowchart RL\n subgraph O [outer]\n subgraph I [inner]\n x --> y\n end\n z --> x\n end\n w --> y\n"
                                  " y --> v\n x --> z\n v --> w\n" },
        { "cycles and loops", "flowchart TD\n A --> B --> C --> A\n C --> C\n B -->|self| B\n D --> D\n A ==> C\n" },
        { "long edges and labels", "flowchart LR\n A ---->|four| B\n A -->|one| C --> D --> B\n A -.->|dotted| D\n" },
    };
    for (const auto& [name, source] : cases) {
        if (!CheckInvariants(name, source)) return false;
    }
    return true;
}

// ---- Golden SVG digests (FixedHost): a change of the output shows here. Re-pin them only for a
// deliberate change of what diagrams look like, and say why.

bool CheckGoldens() {
    const struct {
        const char* source;
        const char* sha256;
    } goldens[] = {
        { "flowchart TD\n A[Christmas] -->|Get money| B(Go shopping)\n B --> C{Let me think}\n C -->|One| D[Laptop]\n"
          " C -->|Two| E[iPhone]\n C -->|Three| F[Car]\n",
          "1c2efa2228eb2ab07bd43a39729755e5de255f670f9c03b4e75795926b336b3b" },
        { "flowchart LR\n start([Start]) --> read[/Read input/]\n read --> check{Valid?}\n check -- yes --> store[(Database)]\n"
          " check -- no --> err[[Report error]]\n err -.-> read\n store ==> done((Done))\n done --> done\n",
          "1d11286fbb3bee9dfe8c8feb45b153c889a20f73e7ccc9972c4e062309c88ecf" },
        { "flowchart TB\n c1-->a2\n subgraph one\n a1-->a2\n end\n subgraph two\n b1-->b2\n end\n subgraph three\n c1-->c2\n end\n"
          " one --> two\n three --> two\n two --> c2\n classDef hot fill:#f96\n class a1 hot\n",
          "6ba385f6d75c82af41b392076588af0d0a18703701dd69beeb9c11946b82707f" },
    };
    bool ok = true;
    for (const auto& golden : goldens) {
        std::string svg;
        if (!Draw(golden.source, svg)) return false;
        const std::string digest = RayoMd::PdfSource::Sha256Hex(svg);
        if (digest != golden.sha256) {
            std::cerr << "Mermaid golden changed: " << digest << " for\n" << golden.source << std::endl;
            ok = false;
        }
    }
    return ok;
}

// ---- Limits: hostile input stays linear and fails closed.

bool CheckBounds() {
    using parse_limits_tests::Bounded;
    std::string svg;
    std::string source = "flowchart LR\n";
    for (int k = 0; k < 5000; k++) source += "a" + std::to_string(k) + "-->b" + std::to_string(k) + "\n";
    if (!Bounded("Mermaid: 10,000 nodes", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n";
    for (int k = 0; k < 10000; k++) source += "subgraph s\n";
    if (!Bounded("Mermaid: 10,000 nested subgraphs", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n A[" + std::string(1 << 20, 'x') + "] --> B\n";
    if (!Bounded("Mermaid: a megabyte label", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n A[" + std::string(120 << 10, 'x') + "] --> B\n";
    if (!Bounded("Mermaid: a 120 KiB label", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n A[\"" + std::string(120 << 10, '[') + "\n";
    if (!Bounded("Mermaid: an unterminated label", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n A ";
    for (int k = 0; k < 300; k++) source += "& A ";
    source += "--> B";
    for (int k = 0; k < 333; k++) source += " & B";
    source += "\n";
    if (!Bounded("Mermaid: 100,000 edges between two nodes", 2.0, 64, [&]() { return !Draw(source, svg); })) return false;
    source = "flowchart TD\n";
    for (int k = 0; k < 2000; k++) source += "A-->B\n";
    if (!Bounded("Mermaid: 2,000 edges between two nodes", 5.0, 128, [&]() { return Draw(source, svg) && Converts(svg); })) return false;
    source = "flowchart TD\n";
    for (int k = 0; k < 999; k++) source += "c" + std::to_string(k) + " ---------> c" + std::to_string(k + 1) + "\n";
    if (!Bounded("Mermaid: a chain of 1,000 long edges", 5.0, 256, [&]() { return Draw(source, svg) && Converts(svg); })) return false;
    source = "flowchart TD\n";
    for (int k = 0; k < 999; k++) source += "h --> t" + std::to_string(k) + "\n";
    if (!Bounded("Mermaid: a node with 999 successors", 5.0, 256, [&]() { return Draw(source, svg) && Converts(svg); })) return false;
    source = "flowchart TD\n";
    uint32_t state = 7;
    for (int k = 0; k < 2000; k++) {
        state = state * 1103515245u + 12345u;
        const uint32_t a = (state >> 8) % 1000;
        state = state * 1103515245u + 12345u;
        source += "m" + std::to_string(a) + " --> m" + std::to_string((state >> 8) % 1000) + "\n";
    }
    if (!Bounded("Mermaid: 2,000 random edges over 1,000 nodes", 5.0, 256, [&]() { Draw(source, svg); return true; })) return false;
    // 200 nodes in a random tree with 100 more edges: the size of a large real diagram.
    source = "flowchart TD\n";
    state = 11;
    for (int k = 1; k < 200; k++) {
        state = state * 1103515245u + 12345u;
        source += "n" + std::to_string((state >> 8) % static_cast<uint32_t>(k)) + " --> n" + std::to_string(k) + "[Node " + std::to_string(k) + "]\n";
    }
    for (int k = 0; k < 100; k++) {
        state = state * 1103515245u + 12345u;
        const uint32_t a = (state >> 8) % 200;
        state = state * 1103515245u + 12345u;
        source += "n" + std::to_string(a) + " -.-> n" + std::to_string((state >> 8) % 200) + "\n";
    }
    return Bounded("Mermaid: 200 nodes and 299 edges", 1.0, 64, [&]() { return Draw(source, svg) && Converts(svg); });
}

bool CheckMermaid() {
    return CheckFlowchartSyntax() && CheckRefused(RefusedFlowcharts()) && CheckFlowchartLayout() && CheckGoldens() && CheckBounds();
}

} // namespace mermaid_tests
