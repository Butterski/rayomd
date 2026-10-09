#pragma once

// SVG images: a static subset of SVG drawn as a PDF form XObject, for the charts and diagrams
// that matplotlib, Vega, Plotly and Graphviz export and for logos and badges. No browser, no
// rasterizing: paths, shapes, transforms, clipping, linear gradients, text and embedded raster
// images map to PDF operators. Built at -Os: an SVG is converted once per document.

#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// What an SVG form needs from the document it is drawn in: the document's font, which shows the
// form's text, and its raster images.
class SvgHost {
public:
    virtual ~SvgHost() = default;
    // The width in points of `text` (UTF-8) at font size `size`.
    virtual double TextWidth(std::string_view text, double size, bool bold) = 0;
    // Appends `text` (UTF-8) as the operand of Tj in the font the form names /F1, or /F2 for bold
    // text when HasBoldFace: "(...)" or "<...>".
    virtual void AppendText(std::string& out, std::string_view text, bool bold) = 0;
    // Whether bold text has a face of its own, /F2; without, the form draws it in /F1 with a thin
    // stroke of its fill colour (2 Tr).
    virtual bool HasBoldFace() const = 0;
    // A raster image, the PNG or JPEG bytes of a data: URI, which the form draws as /Im<index + 1>:
    // its index, with its size in pixels; -1 when it cannot be decoded.
    virtual int AddImage(std::string_view bytes, int& width, int& height) = 0;
};

// An SVG as a PDF form: its content stream over [0 0 width height] in points, the origin at the
// bottom left, and the resources it names besides the document's fonts and images.
struct SvgForm {
    double width = 0.0;
    double height = 0.0;
    std::string content;
    std::string extGStates;    // the entries of its /ExtGState dictionary, "/GS1 << /ca 0.5 >> ...", or none
    std::string shadings;      // the entries of its /Shading dictionary, "/Sh1 << ... >> ...", or none
    std::vector<int> images;   // the host's images (SvgHost::AddImage) it draws
    bool text = false;         // it draws text, in /F1 (and /F2)
};

// Whether `bytes` are SVG: after a byte order mark, white space, an XML declaration, comments and
// a document type declaration, the first element is <svg>.
bool LooksLikeSvg(std::string_view bytes);

// Converts SVG markup. False when it is not an SVG RayoMD can draw as it means: malformed, past a
// safety limit, painting nothing, or depending on content it does not draw (scripts, HTML, style
// rules whose selectors it does not match); the caller then shows the image's alt text.
bool ConvertSvg(std::string_view svg, SvgHost& host, SvgForm& form);

} // namespace TinyPdf::Internal
