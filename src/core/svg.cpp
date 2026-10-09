#include "svg.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../common/text_utils.h"

namespace TinyPdf::Internal {
namespace {

// Safety limits: an SVG past any of them is not drawn. Every pass is linear in the input, and
// these bound the work `use` can multiply and the size of the content stream.
constexpr size_t kMaxSvgBytes = 16u << 20;        // the markup
constexpr size_t kMaxDepth = 64;                  // XML nesting
constexpr size_t kMaxElements = 500000;
constexpr size_t kMaxNodes = 1000000;             // elements and the text nodes kept
constexpr size_t kMaxAttributes = 256;            // per element
constexpr size_t kMaxPathNumbers = 8000000;       // numbers of all path data and point lists
constexpr int kMaxUseDepth = 8;                   // use instances inside use instances
constexpr int kMaxRenderDepth = 256;              // elements on the render stack, use instances included
constexpr int kMaxClipDepth = 8;                  // clip-path scopes inside each other
constexpr int kMaxClipChain = 4;                  // a clipPath clipped by a clipPath ...
constexpr size_t kMaxSaveDepth = 28;              // q nesting (PDF Reference 1.4, Appendix C)
constexpr int kMaxGradientHops = 4;               // href chain of a gradient
constexpr size_t kMaxStops = 256;
constexpr size_t kMaxDashes = 64;
constexpr size_t kMaxStyleBytes = 64u << 10;      // the text of all <style> elements
constexpr size_t kMaxStyleRules = 1024;
constexpr size_t kMaxContentBytes = 64u << 20;    // the form's content stream
constexpr size_t kMaxTextPerElement = 64u << 10;  // bytes of one text element
constexpr size_t kMaxTextTotal = 1u << 20;        // bytes of all text
constexpr double kMaxReal = 32767.0;              // the largest real a content stream holds
constexpr double kPi = 3.14159265358979323846;
constexpr double kPxToPt = 0.75;

// ---------------------------------------------------------------------------------------------
// Characters and numbers

// Helpers on the per-byte paths, inlined even at -Os, where GCC leaves small functions (and
// std::string_view's) out of line.
#if defined(__GNUC__) || defined(__clang__)
#define SVG_INLINE inline __attribute__((always_inline))
#else
#define SVG_INLINE inline
#endif

// Byte classes for one-lookup scans.
enum : uint8_t {
    kByteSpace = 1,       // SVG white space: space, tab, line feed, carriage return, form feed
    kByteNameStart = 2,   // starts an XML name: letters, '_', ':', non-ASCII
    kByteName = 4,        // continues an XML name: also digits, '-', '.'
    kByteDecode = 8,      // needs attention in an attribute value: '&', '<', tab, line feed, carriage return
};

constexpr std::array<uint8_t, 256> MakeByteClasses() {
    std::array<uint8_t, 256> classes{};
    for (size_t value = 0; value < 256; value++) {
        const bool letter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_' || value == ':' || value >= 0x80;
        const bool other = (value >= '0' && value <= '9') || value == '-' || value == '.';
        classes[value] = static_cast<uint8_t>((letter ? kByteNameStart | kByteName : 0) | (other ? kByteName : 0));
    }
    for (unsigned char ch : { ' ', '\t', '\n', '\r', '\f' }) classes[ch] |= kByteSpace;
    for (unsigned char ch : { '&', '<', '\t', '\n', '\r' }) classes[ch] |= kByteDecode;
    return classes;
}
constexpr std::array<uint8_t, 256> kByteClasses = MakeByteClasses();

SVG_INLINE bool HasClass(char ch, uint8_t mask) { return (kByteClasses[static_cast<unsigned char>(ch)] & mask) != 0; }
SVG_INLINE bool IsDigit(char ch) { return static_cast<unsigned char>(ch - '0') < 10; }
SVG_INLINE bool IsSpace(char ch) { return HasClass(ch, kByteSpace); }
SVG_INLINE bool IsAlpha(char ch) { return static_cast<unsigned char>((ch | 0x20) - 'a') < 26; }
SVG_INLINE char Lower(char ch) { return static_cast<unsigned char>(ch - 'A') < 26 ? static_cast<char>(ch + 32) : ch; }

SVG_INLINE bool Equal(const char* a, const char* b, size_t size) {
    for (size_t index = 0; index < size; index++) {
        if (a[index] != b[index]) return false;
    }
    return true;
}

// `text` equals a literal: its length known when compiled, without strlen or a call.
template <size_t N>
SVG_INLINE bool operator==(std::string_view text, const char (&literal)[N]) {
    return text.size() == N - 1 && Equal(text.data(), literal, N - 1);
}
template <size_t N>
SVG_INLINE bool operator!=(std::string_view text, const char (&literal)[N]) {
    return !(text == literal);
}

// `text` holds a literal at `at`.
template <size_t N>
SVG_INLINE bool StartsAt(std::string_view text, size_t at, const char (&literal)[N]) {
    return at <= text.size() && text.size() - at >= N - 1 && Equal(text.data() + at, literal, N - 1);
}

// A name passed as a literal (its length known when compiled) or a view.
struct Name {
    std::string_view text;
    template <size_t N>
    SVG_INLINE constexpr Name(const char (&literal)[N]) : text(literal, N - 1) {}
    SVG_INLINE constexpr Name(std::string_view view) : text(view) {}
};

SVG_INLINE bool Same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && Equal(a.data(), b.data(), a.size());
}

std::string_view Trim(std::string_view text) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    while (begin < end && IsSpace(*begin)) begin++;
    while (end > begin && IsSpace(end[-1])) end--;
    return std::string_view(begin, static_cast<size_t>(end - begin));
}

bool EqualsIgnoreCase(std::string_view a, Name name) {
    const std::string_view b = name.text;
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); index++) {
        if (Lower(a[index]) != Lower(b[index])) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(std::string_view text, Name prefix) {
    return text.size() >= prefix.text.size() && EqualsIgnoreCase(std::string_view(text.data(), prefix.text.size()), prefix);
}

SVG_INLINE void SkipSpace(const char*& at, const char* end) {
    while (at < end && IsSpace(*at)) at++;
}

// Skips SVG's comma-wsp: white space with at most one comma.
SVG_INLINE void SkipCommaSpace(const char*& at, const char* end) {
    SkipSpace(at, end);
    if (at < end && *at == ',') {
        at++;
        SkipSpace(at, end);
    }
}

constexpr double kPowersOfTen[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// Scans an SVG number at `at`: [sign] digits [. digits] [e [sign] digits], also ".5" and "5.".
// A second point or a sign ends it ("0.6.5" is 0.6 then .5, "100-200" is 100 then -200), and an
// "e" without digits is left for a unit ("1em"). Moves `at` past it; false when none starts
// there. Locale-free: up to 19 significant digits scaled by exact powers of ten. A magnitude
// past 1e308 gives infinity, which callers reject.
bool ScanNumberSlow(const char*& at, const char* end, double& value) {
    const char* p = at;
    bool negative = false;
    if (p < end && (*p == '+' || *p == '-')) {
        negative = *p == '-';
        p++;
    }
    uint64_t mantissa = 0;
    int digits = 0;
    int exponent = 0;
    bool any = false;
    for (; p < end && IsDigit(*p); p++) {
        any = true;
        if (digits < 19) {
            mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
            if (mantissa != 0) digits++;
        } else {
            exponent++;
        }
    }
    if (p < end && *p == '.' && (any || (p + 1 < end && IsDigit(p[1])))) {
        p++;
        for (; p < end && IsDigit(*p); p++) {
            any = true;
            if (digits < 19) {
                mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
                if (mantissa != 0) digits++;
                exponent--;
            }
        }
    }
    if (!any) return false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool exponentNegative = false;
        if (q < end && (*q == '+' || *q == '-')) {
            exponentNegative = *q == '-';
            q++;
        }
        if (q < end && IsDigit(*q)) {
            int written = 0;
            for (; q < end && IsDigit(*q); q++) {
                if (written < 100000) written = written * 10 + (*q - '0');
            }
            exponent += exponentNegative ? -written : written;
            p = q;
        }
    }
    at = p;
    if (mantissa == 0) {
        value = 0.0;
        return true;
    }
    const int magnitude = exponent + digits - 1;
    if (magnitude > 308) {
        value = HUGE_VAL;
    } else if (magnitude < -330) {
        value = 0.0;
    } else {
        value = static_cast<double>(mantissa);
        while (exponent > 22) {
            value *= 1e22;
            exponent -= 22;
        }
        while (exponent < -22) {
            value /= 1e22;
            exponent += 22;
        }
        value = exponent >= 0 ? value * kPowersOfTen[exponent] : value / kPowersOfTen[-exponent];
    }
    if (negative) value = -value;
    return true;
}

bool ScanNumber(const char*& at, const char* end, double& value) {
    // The numbers of charts: at most 19 digits and no exponent, read in one pass with the same
    // result as the general scan.
    const char* p = at;
    const bool negative = p < end && *p == '-';
    if (p < end && (*p == '+' || *p == '-')) p++;
    const char* const first = p;
    uint64_t mantissa = 0;
    for (; p < end && IsDigit(*p); p++) mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
    size_t digits = static_cast<size_t>(p - first);
    size_t fraction = 0;
    if (p < end && *p == '.' && (digits > 0 || (p + 1 < end && IsDigit(p[1])))) {
        const char* const point = ++p;
        for (; p < end && IsDigit(*p); p++) mantissa = mantissa * 10 + static_cast<unsigned>(*p - '0');
        fraction = static_cast<size_t>(p - point);
        digits += fraction;
    }
    if (digits == 0 || digits > 19 || (p < end && (*p == 'e' || *p == 'E'))) return ScanNumberSlow(at, end, value);
    at = p;
    if (mantissa == 0) {
        value = 0.0;
        return true;
    }
    value = static_cast<double>(mantissa) / kPowersOfTen[fraction];
    if (negative) value = -value;
    return true;
}

// A whole string that is one finite number, white space around it allowed.
bool ParseNumber(std::string_view text, double& value) {
    text = Trim(text);
    const char* at = text.data();
    const char* end = at + text.size();
    return ScanNumber(at, end, value) && at == end && std::isfinite(value);
}

// ---------------------------------------------------------------------------------------------
// Geometry

struct Matrix {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

// m · n: n applies first.
Matrix Multiply(const Matrix& m, const Matrix& n) {
    return { m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b,
             m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
             m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
}

Matrix Translation(double x, double y) { return { 1.0, 0.0, 0.0, 1.0, x, y }; }

double Determinant(const Matrix& m) { return m.a * m.d - m.b * m.c; }

bool IsUsable(const Matrix& m) {
    const double det = Determinant(m);
    return std::isfinite(det) && det != 0.0 && std::isfinite(m.e) && std::isfinite(m.f);
}

// Rotation, reflection and uniform scale: a stroke keeps its shape when its points are
// transformed and its width scaled.
bool IsSimilarity(const Matrix& m) {
    const double p = m.a * m.a + m.b * m.b;
    const double q = m.c * m.c + m.d * m.d;
    const double tolerance = 1e-6 * std::max(p, q);
    return std::fabs(m.a * m.c + m.b * m.d) <= tolerance && std::fabs(p - q) <= tolerance;
}

struct Box {
    double x0 = HUGE_VAL, y0 = HUGE_VAL, x1 = -HUGE_VAL, y1 = -HUGE_VAL;
    bool Empty() const { return x0 > x1 || y0 > y1; }
    void Add(double x, double y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void Add(const Box& other) {
        if (other.Empty()) return;
        Add(other.x0, other.y0);
        Add(other.x1, other.y1);
    }
};

// Adds the extremes of one coordinate of a cubic to `box`: the roots of its derivative.
void AddCubicExtremes(Box& box, const double (&x)[4], const double (&y)[4], const double (&p)[4]) {
    const double a = -p[0] + 3.0 * p[1] - 3.0 * p[2] + p[3];
    const double b = 2.0 * (p[0] - 2.0 * p[1] + p[2]);
    const double c = p[1] - p[0];
    double roots[2];
    int count = 0;
    if (std::fabs(a) < 1e-12) {
        if (std::fabs(b) > 1e-12) roots[count++] = -c / b;
    } else {
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            roots[count++] = (-b + root) / (2.0 * a);
            roots[count++] = (-b - root) / (2.0 * a);
        }
    }
    for (int index = 0; index < count; index++) {
        const double t = roots[index];
        if (!(t > 0.0 && t < 1.0)) continue;
        const double u = 1.0 - t;
        const double w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
        box.Add(w0 * x[0] + w1 * x[1] + w2 * x[2] + w3 * x[3], w0 * y[0] + w1 * y[1] + w2 * y[2] + w3 * y[3]);
    }
}

// ---------------------------------------------------------------------------------------------
// Numbers in the content stream

bool Fits(double value) { return std::fabs(value) <= kMaxReal; }   // false for NaN too

// Two decimals: coordinates already in form space. Writes at most kFixed2MaxChars characters.
char* WriteNumber(char* out, double value) {
    if (std::fabs(value) < 0.005) value = 0.0;
    return RayoMd::Text::WriteFixed2(out, value);
}

void AppendNumber(std::string& out, double value) {
    char buffer[RayoMd::Text::kFixed2MaxChars];
    out.append(buffer, static_cast<size_t>(WriteNumber(buffer, value) - buffer));
}

// Four decimals: text matrices, line widths, dashes, gradient coordinates and the paths of
// strokes drawn under cm. `value` must Fit: at most 11 characters.
char* WriteFixed4(char* out, double value) {
    if (std::fabs(value) < 0.00005) value = 0.0;
    if (value < 0.0) *out++ = '-';
    const uint64_t scaled = static_cast<uint64_t>(std::fabs(value) * 10000.0 + 0.5);
    out = std::to_chars(out, out + 20, scaled / 10000).ptr;
    unsigned fraction = static_cast<unsigned>(scaled % 10000);
    if (fraction == 0) return out;
    *out++ = '.';
    for (unsigned divisor = 1000; fraction != 0; divisor /= 10) {
        *out++ = static_cast<char>('0' + fraction / divisor);
        fraction %= divisor;
    }
    return out;
}

void AppendFixed4(std::string& out, double value) {
    char buffer[24];
    out.append(buffer, static_cast<size_t>(WriteFixed4(buffer, value) - buffer));
}

// Thousandths: 500 is "0.5".
void AppendMilli(std::string& out, int milli) {
    if (milli >= 1000) {
        out += '1';
        return;
    }
    if (milli <= 0) {
        out += '0';
        return;
    }
    char digits[5] = { '0', '.', static_cast<char>('0' + milli / 100), static_cast<char>('0' + milli / 10 % 10),
                       static_cast<char>('0' + milli % 10) };
    size_t length = 5;
    while (digits[length - 1] == '0') length--;
    out.append(digits, length);
}

// "r g b" with each channel /255 at three decimals.
void AppendColor(std::string& out, uint32_t rgb) {
    for (int shift = 16; shift >= 0; shift -= 8) {
        const int channel = static_cast<int>((rgb >> shift) & 0xFFu);
        AppendMilli(out, (channel * 2000 + 255) / 510);
        if (shift != 0) out += ' ';
    }
}

void AppendMatrix(std::string& out, const Matrix& m) {
    for (double value : { m.a, m.b, m.c, m.d, m.e, m.f }) {
        AppendFixed4(out, value);
        out += ' ';
    }
    out += "cm\n";
}

bool MatrixFits(const Matrix& m) {
    return Fits(m.a) && Fits(m.b) && Fits(m.c) && Fits(m.d) && Fits(m.e) && Fits(m.f);
}

// ---------------------------------------------------------------------------------------------
// Colors

// The 148 named colors of CSS Color 4: their names, sorted and separated by spaces, and their
// values in the same order. No pointers, so the tables need no relocations.
constexpr char kColorNames[] =
    "aliceblue antiquewhite aqua aquamarine azure beige bisque black blanchedalmond blue blueviolet "
    "brown burlywood cadetblue chartreuse chocolate coral cornflowerblue cornsilk crimson cyan "
    "darkblue darkcyan darkgoldenrod darkgray darkgreen darkgrey darkkhaki darkmagenta "
    "darkolivegreen darkorange darkorchid darkred darksalmon darkseagreen darkslateblue "
    "darkslategray darkslategrey darkturquoise darkviolet deeppink deepskyblue dimgray dimgrey "
    "dodgerblue firebrick floralwhite forestgreen fuchsia gainsboro ghostwhite gold goldenrod gray "
    "green greenyellow grey honeydew hotpink indianred indigo ivory khaki lavender lavenderblush "
    "lawngreen lemonchiffon lightblue lightcoral lightcyan lightgoldenrodyellow lightgray lightgreen "
    "lightgrey lightpink lightsalmon lightseagreen lightskyblue lightslategray lightslategrey "
    "lightsteelblue lightyellow lime limegreen linen magenta maroon mediumaquamarine mediumblue "
    "mediumorchid mediumpurple mediumseagreen mediumslateblue mediumspringgreen mediumturquoise "
    "mediumvioletred midnightblue mintcream mistyrose moccasin navajowhite navy oldlace olive "
    "olivedrab orange orangered orchid palegoldenrod palegreen paleturquoise palevioletred "
    "papayawhip peachpuff peru pink plum powderblue purple rebeccapurple red rosybrown royalblue "
    "saddlebrown salmon sandybrown seagreen seashell sienna silver skyblue slateblue slategray "
    "slategrey snow springgreen steelblue tan teal thistle tomato turquoise violet wheat white "
    "whitesmoke yellow yellowgreen";
constexpr uint32_t kColorValues[] = {
    0xf0f8ff, 0xfaebd7, 0x00ffff, 0x7fffd4, 0xf0ffff, 0xf5f5dc, 0xffe4c4, 0x000000, 0xffebcd, 0x0000ff, 0x8a2be2, 0xa52a2a,
    0xdeb887, 0x5f9ea0, 0x7fff00, 0xd2691e, 0xff7f50, 0x6495ed, 0xfff8dc, 0xdc143c, 0x00ffff, 0x00008b, 0x008b8b, 0xb8860b,
    0xa9a9a9, 0x006400, 0xa9a9a9, 0xbdb76b, 0x8b008b, 0x556b2f, 0xff8c00, 0x9932cc, 0x8b0000, 0xe9967a, 0x8fbc8f, 0x483d8b,
    0x2f4f4f, 0x2f4f4f, 0x00ced1, 0x9400d3, 0xff1493, 0x00bfff, 0x696969, 0x696969, 0x1e90ff, 0xb22222, 0xfffaf0, 0x228b22,
    0xff00ff, 0xdcdcdc, 0xf8f8ff, 0xffd700, 0xdaa520, 0x808080, 0x008000, 0xadff2f, 0x808080, 0xf0fff0, 0xff69b4, 0xcd5c5c,
    0x4b0082, 0xfffff0, 0xf0e68c, 0xe6e6fa, 0xfff0f5, 0x7cfc00, 0xfffacd, 0xadd8e6, 0xf08080, 0xe0ffff, 0xfafad2, 0xd3d3d3,
    0x90ee90, 0xd3d3d3, 0xffb6c1, 0xffa07a, 0x20b2aa, 0x87cefa, 0x778899, 0x778899, 0xb0c4de, 0xffffe0, 0x00ff00, 0x32cd32,
    0xfaf0e6, 0xff00ff, 0x800000, 0x66cdaa, 0x0000cd, 0xba55d3, 0x9370db, 0x3cb371, 0x7b68ee, 0x00fa9a, 0x48d1cc, 0xc71585,
    0x191970, 0xf5fffa, 0xffe4e1, 0xffe4b5, 0xffdead, 0x000080, 0xfdf5e6, 0x808000, 0x6b8e23, 0xffa500, 0xff4500, 0xda70d6,
    0xeee8aa, 0x98fb98, 0xafeeee, 0xdb7093, 0xffefd5, 0xffdab9, 0xcd853f, 0xffc0cb, 0xdda0dd, 0xb0e0e6, 0x800080, 0x663399,
    0xff0000, 0xbc8f8f, 0x4169e1, 0x8b4513, 0xfa8072, 0xf4a460, 0x2e8b57, 0xfff5ee, 0xa0522d, 0xc0c0c0, 0x87ceeb, 0x6a5acd,
    0x708090, 0x708090, 0xfffafa, 0x00ff7f, 0x4682b4, 0xd2b48c, 0x008080, 0xd8bfd8, 0xff6347, 0x40e0d0, 0xee82ee, 0xf5deb3,
    0xffffff, 0xf5f5f5, 0xffff00, 0x9acd32,
};
constexpr size_t kColorCount = sizeof kColorValues / sizeof kColorValues[0];

// A word of a space-separated list: where it starts and its length. Tables of words need no
// pointers (no relocations) and compare lengths before bytes (no strlen).
struct Word {
    uint16_t offset;
    uint8_t size;
};

template <size_t N>
constexpr std::array<Word, N> MakeWords(const char* words) {
    std::array<Word, N> table{};
    size_t count = 0;
    size_t start = 0;
    for (size_t at = 0;; at++) {
        if (words[at] != ' ' && words[at] != '\0') continue;
        table[count++] = { static_cast<uint16_t>(start), static_cast<uint8_t>(at - start) };
        start = at + 1;
        if (words[at] == '\0') break;
    }
    return table;
}

// The 1-based position of `key` among the words of a table, 0 when absent.
size_t FindWord(const char* words, const Word* table, size_t count, std::string_view key) {
    for (size_t index = 0; index < count; index++) {
        if (table[index].size == key.size() && Equal(words + table[index].offset, key.data(), key.size())) return index + 1;
    }
    return 0;
}

constexpr std::array<Word, kColorCount> kColorWords = MakeWords<kColorCount>(kColorNames);
static_assert(kColorCount == 148 && kColorWords[147].size == 11, "148 names, 148 values");

int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = Lower(ch);
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

// `value` limited to [0, 1], NaN to 0: safe to scale and convert to an integer.
double Clamp01(double value) {
    return value > 0.0 ? (value < 1.0 ? value : 1.0) : 0.0;
}

// Thousandths of a value in [0, 1].
int Milli(double value) {
    return static_cast<int>(Clamp01(value) * 1000.0 + 0.5);
}

uint32_t PackRgb(double r, double g, double b) {
    auto channel = [](double value) {
        return static_cast<uint32_t>(Clamp01(value / 255.0) * 255.0 + 0.5);
    };
    return channel(r) << 16 | channel(g) << 8 | channel(b);
}

// The arguments of rgb()/rgba()/hsl()/hsla(): comma or space syntax, "/ alpha", percentages.
bool ParseColorFunction(std::string_view args, bool hsl, uint32_t& rgb, double& alpha) {
    double values[4];
    bool percent[4];
    int count = 0;
    const char* at = args.data();
    const char* end = at + args.size();
    while (true) {
        SkipSpace(at, end);
        if (at == end) break;
        if (count > 0 && (*at == ',' || *at == '/')) {
            at++;
            SkipSpace(at, end);
        }
        double value;
        if (count == 4 || !ScanNumber(at, end, value) || !std::isfinite(value)) return false;
        percent[count] = false;
        if (at < end && *at == '%') {
            percent[count] = true;
            at++;
        } else if (hsl && count == 0) {
            const std::string_view unit(at, static_cast<size_t>(end - at));
            if (StartsWithIgnoreCase(unit, "deg")) {
                at += 3;
            } else if (StartsWithIgnoreCase(unit, "grad")) {
                value *= 0.9;
                at += 4;
            } else if (StartsWithIgnoreCase(unit, "rad")) {
                value *= 180.0 / kPi;
                at += 3;
            } else if (StartsWithIgnoreCase(unit, "turn")) {
                value *= 360.0;
                at += 4;
            }
            if (!std::isfinite(value)) return false;   // 1e308turn
        }
        values[count++] = value;
    }
    if (count < 3) return false;
    alpha = count == 4 ? std::clamp(percent[3] ? values[3] / 100.0 : values[3], 0.0, 1.0) : 1.0;
    if (hsl) {
        double hue = std::fmod(values[0], 360.0);
        if (hue < 0.0) hue += 360.0;
        const double saturation = std::clamp(values[1] / 100.0, 0.0, 1.0);
        const double lightness = std::clamp(values[2] / 100.0, 0.0, 1.0);
        const double chroma = (1.0 - std::fabs(2.0 * lightness - 1.0)) * saturation;
        const double h = hue / 60.0;
        const double x = chroma * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
        double r = 0.0, g = 0.0, b = 0.0;
        if (h < 1.0) { r = chroma; g = x; }
        else if (h < 2.0) { r = x; g = chroma; }
        else if (h < 3.0) { g = chroma; b = x; }
        else if (h < 4.0) { g = x; b = chroma; }
        else if (h < 5.0) { r = x; b = chroma; }
        else { r = chroma; b = x; }
        const double m = lightness - chroma / 2.0;
        rgb = PackRgb((r + m) * 255.0, (g + m) * 255.0, (b + m) * 255.0);
        return true;
    }
    double channels[3];
    for (int index = 0; index < 3; index++) channels[index] = percent[index] ? values[index] * 2.55 : values[index];
    rgb = PackRgb(channels[0], channels[1], channels[2]);
    return true;
}

// A CSS color: a name, transparent, #rgb, #rgba, #rrggbb, #rrggbbaa, rgb[a](), hsl[a]().
// currentColor and none are the caller's.
bool ParseColor(std::string_view text, uint32_t& rgb, double& alpha) {
    text = Trim(text);
    alpha = 1.0;
    if (text.empty()) return false;
    if (text[0] == '#') {
        const std::string_view hex = text.substr(1);
        int digits[8];
        if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) return false;
        for (size_t index = 0; index < hex.size(); index++) {
            digits[index] = HexDigit(hex[index]);
            if (digits[index] < 0) return false;
        }
        if (hex.size() <= 4) {
            rgb = static_cast<uint32_t>(digits[0] * 17) << 16 | static_cast<uint32_t>(digits[1] * 17) << 8 |
                  static_cast<uint32_t>(digits[2] * 17);
            if (hex.size() == 4) alpha = digits[3] * 17 / 255.0;
        } else {
            rgb = static_cast<uint32_t>(digits[0] << 20 | digits[1] << 16 | digits[2] << 12 | digits[3] << 8 |
                                        digits[4] << 4 | digits[5]);
            if (hex.size() == 8) alpha = (digits[6] * 16 + digits[7]) / 255.0;
        }
        return true;
    }
    const size_t open = text.find('(');
    if (open != std::string_view::npos) {
        if (text.back() != ')') return false;
        const std::string_view name = Trim(text.substr(0, open));
        const std::string_view args = text.substr(open + 1, text.size() - open - 2);
        if (EqualsIgnoreCase(name, "rgb") || EqualsIgnoreCase(name, "rgba")) return ParseColorFunction(args, false, rgb, alpha);
        if (EqualsIgnoreCase(name, "hsl") || EqualsIgnoreCase(name, "hsla")) return ParseColorFunction(args, true, rgb, alpha);
        return false;
    }
    if (EqualsIgnoreCase(text, "transparent")) {
        rgb = 0;
        alpha = 0.0;
        return true;
    }
    char key[24];
    if (text.size() >= sizeof key) return false;
    for (size_t index = 0; index < text.size(); index++) key[index] = Lower(text[index]);
    const std::string_view name(key, text.size());
    // Binary search of the sorted names.
    size_t low = 0;
    size_t high = kColorCount;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const Word word = kColorWords[middle];
        const int order = std::string_view(kColorNames + word.offset, word.size).compare(name);
        if (order == 0) {
            rgb = kColorValues[middle];
            return true;
        }
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Elements and properties

enum Tag : uint8_t {
    kTagOther, kTagSvg, kTagG, kTagA, kTagDefs, kTagPath, kTagRect, kTagCircle, kTagEllipse, kTagLine,
    kTagPolyline, kTagPolygon, kTagText, kTagTspan, kTagTextPath, kTagUse, kTagImage, kTagClipPath,
    kTagLinearGradient, kTagRadialGradient, kTagStop, kTagPattern, kTagSymbol, kTagSwitch, kTagStyle,
    kTagForeignObject, kTagScript,
    kTagTextNode,   // character data
    kTagForeign,    // an element outside the SVG namespace
};

// The names of kTagSvg .. kTagScript, in their order.
constexpr char kTagNames[] =
    "svg g a defs path rect circle ellipse line polyline polygon text tspan textPath use image clipPath "
    "linearGradient radialGradient stop pattern symbol switch style foreignObject script";
constexpr std::array<Word, kTagScript> kTagWords = MakeWords<kTagScript>(kTagNames);
static_assert(kTagWords[kTagScript - 1].size == 6, "a name per tag");

Tag TagOf(std::string_view local) {
    return static_cast<Tag>(FindWord(kTagNames, kTagWords.data(), kTagWords.size(), local));
}

bool IsShape(Tag tag) {
    return tag == kTagPath || tag == kTagRect || tag == kTagCircle || tag == kTagEllipse || tag == kTagLine ||
           tag == kTagPolyline || tag == kTagPolygon;
}

enum Property : uint8_t {
    kPropNone, kPropFill, kPropFillOpacity, kPropFillRule, kPropStroke, kPropStrokeOpacity, kPropStrokeWidth,
    kPropStrokeLinecap, kPropStrokeLinejoin, kPropStrokeMiterlimit, kPropStrokeDasharray, kPropStrokeDashoffset,
    kPropOpacity, kPropClipPath, kPropClipRule, kPropDisplay, kPropVisibility, kPropColor, kPropStopColor,
    kPropStopOpacity, kPropFontSize, kPropFontWeight, kPropTextAnchor, kPropWhiteSpace, kPropOverflow,
};

// The names of kPropFill .. kPropOverflow, in their order.
constexpr char kPropertyNames[] =
    "fill fill-opacity fill-rule stroke stroke-opacity stroke-width stroke-linecap stroke-linejoin "
    "stroke-miterlimit stroke-dasharray stroke-dashoffset opacity clip-path clip-rule display visibility "
    "color stop-color stop-opacity font-size font-weight text-anchor white-space overflow";
constexpr std::array<Word, kPropOverflow> kPropertyWords = MakeWords<kPropOverflow>(kPropertyNames);
static_assert(kPropertyWords[kPropOverflow - 1].size == 8, "a name per property");

// A hash of a property name, its length and two of its letters, that no two properties share.
constexpr size_t PropertyHash(const char* name, size_t size) {
    return (size + 2u * static_cast<unsigned char>(name[0]) + 3u * static_cast<unsigned char>(name[size < 8 ? size - 1 : 7])) & 63u;
}

// The properties by hash: each slot the property plus one, or 0.
constexpr std::array<uint8_t, 64> MakePropertyHashes() {
    std::array<uint8_t, 64> slots{};
    for (size_t index = 0; index < kPropertyWords.size(); index++) {
        const Word word = kPropertyWords[index];
        uint8_t& slot = slots[PropertyHash(kPropertyNames + word.offset, word.size)];
        slot = slot == 0 ? static_cast<uint8_t>(index + 1) : 0xFF;
    }
    return slots;
}
constexpr std::array<uint8_t, 64> kPropertyHashes = MakePropertyHashes();

constexpr bool PropertyHashesDistinct() {
    size_t used = 0;
    for (uint8_t slot : kPropertyHashes) {
        if (slot == 0xFF) return false;
        used += slot != 0;
    }
    return used == kPropertyWords.size();
}
static_assert(PropertyHashesDistinct(), "no two properties hash alike");

// A property by its name, in any case: one hash, one comparison.
Property PropertyOf(std::string_view name) {
    char key[18];
    if (name.empty() || name.size() >= sizeof key) return kPropNone;
    for (size_t index = 0; index < name.size(); index++) key[index] = Lower(name[index]);
    const uint8_t slot = kPropertyHashes[PropertyHash(key, name.size())];
    if (slot == 0) return kPropNone;
    const Word word = kPropertyWords[slot - 1u];
    return word.size == name.size() && Equal(kPropertyNames + word.offset, key, word.size) ? static_cast<Property>(slot) : kPropNone;
}

// Turns CSS comments into spaces; an unclosed comment runs to the end.
void BlankComments(std::string& css) {
    for (size_t at = css.find("/*"); at != std::string::npos; at = css.find("/*", at)) {
        size_t close = css.find("*/", at + 2);
        close = close == std::string::npos ? css.size() : close + 2;
        std::fill(css.begin() + static_cast<std::ptrdiff_t>(at), css.begin() + static_cast<std::ptrdiff_t>(close), ' ');
        at = close;
    }
}

// Calls `apply(property, value, important)` for each declaration of a CSS block or style
// attribute: "name: value; ...", semicolons inside quotes and parentheses kept.
template <class Apply>
void ForEachDeclaration(std::string_view block, Apply&& apply) {
    size_t start = 0;
    char quote = 0;
    int parentheses = 0;
    for (size_t at = 0; at <= block.size(); at++) {
        const char ch = at < block.size() ? block[at] : ';';
        if (quote != 0) {
            if (ch == quote) quote = 0;
            if (at < block.size()) continue;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        } else if (ch == '(') {
            parentheses++;
            continue;
        } else if (ch == ')') {
            if (parentheses > 0) parentheses--;
            continue;
        }
        if (ch != ';' || (parentheses > 0 && at < block.size())) continue;
        const char* const begin = block.data() + start;
        const char* const end = block.data() + at;
        start = at + 1;
        const char* colon = begin;
        while (colon < end && *colon != ':') colon++;
        if (colon == end) continue;
        const Property property = PropertyOf(Trim(std::string_view(begin, static_cast<size_t>(colon - begin))));
        if (property == kPropNone) continue;
        std::string_view value = Trim(std::string_view(colon + 1, static_cast<size_t>(end - colon - 1)));
        bool important = false;
        const char* bang = value.data() + value.size();
        while (bang > value.data() && bang[-1] != '!') bang--;
        if (bang > value.data()) {
            const char* const valueEnd = value.data() + value.size();
            if (EqualsIgnoreCase(Trim(std::string_view(bang, static_cast<size_t>(valueEnd - bang))), "important")) {
                important = true;
                value = Trim(std::string_view(value.data(), static_cast<size_t>(bang - 1 - value.data())));
            }
        }
        apply(property, value, important);
    }
}

// ---------------------------------------------------------------------------------------------
// XML

struct Attribute {
    std::string_view name;
    std::string_view value;
};

struct Node {
    std::string_view name;   // the qualified name of an element, the text of a text node
    uint32_t firstAttribute = 0;
    uint32_t attributeCount = 0;
    int32_t parent = -1;
    int32_t firstChild = -1;
    int32_t nextSibling = -1;
    Tag tag = kTagOther;
};

struct Document {
    std::vector<Node> nodes;
    std::vector<Attribute> attributes;
    std::deque<std::string> decoded;   // attribute values and text whose characters differ from the markup
    size_t elements = 0;
    int root = -1;
    bool script = false;               // a script element anywhere
    bool foreignText = false;          // text inside a foreignObject
};

constexpr char kSvgNamespace[] = "http://www.w3.org/2000/svg";

bool AllSpace(std::string_view text) {
    for (char ch : text) {
        if (!IsSpace(ch)) return false;
    }
    return true;
}

void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | code >> 18);
        out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// Reads well-formed XML into a Document, without recursion: no DTD, the five named entities
// and character references only. Keeps the text of style and text elements; other text is
// checked and dropped.
class XmlReader {
public:
    XmlReader(std::string_view source, Document& document) : source_(source), document_(document) {}

    bool Read() {
        const std::string_view& src = source_;
        size_t at = 0;
        if (src.size() >= 2 && ((src[0] == '\xFE' && src[1] == '\xFF') || (src[0] == '\xFF' && src[1] == '\xFE'))) {
            return false;   // UTF-16
        }
        if (StartsAt(src, 0, "\xEF\xBB\xBF")) at = 3;
        while (at < src.size()) {
            if (src[at] != '<') {
                size_t next = src.find('<', at);
                if (next == std::string_view::npos) next = src.size();
                if (!Text(src.substr(at, next - at), false)) return false;
                at = next;
                continue;
            }
            const char next = at + 1 < src.size() ? src[at + 1] : '\0';
            if (next != '!' && next != '?' && next != '/') {
                if (!StartTag(at)) return false;
            } else if (StartsAt(src, at, "<!--")) {
                const size_t close = src.find("-->", at + 4);
                if (close == std::string_view::npos) return false;
                at = close + 3;
            } else if (StartsAt(src, at, "<![CDATA[")) {
                const size_t close = src.find("]]>", at + 9);
                if (close == std::string_view::npos || !Text(src.substr(at + 9, close - at - 9), true)) return false;
                at = close + 3;
            } else if (next == '!') {
                if (document_.root >= 0 || !StartsAt(src, at, "<!DOCTYPE")) return false;
                char quote = 0;
                for (at += 9; at < src.size(); at++) {
                    const char ch = src[at];
                    if (quote != 0) {
                        if (ch == quote) quote = 0;
                    } else if (ch == '"' || ch == '\'') {
                        quote = ch;
                    } else if (ch == '[') {
                        return false;   // an internal subset: entities RayoMD does not expand
                    } else if (ch == '>') {
                        break;
                    }
                }
                if (at == src.size()) return false;
                at++;
            } else if (next == '?') {
                const size_t close = src.find("?>", at + 2);
                if (close == std::string_view::npos) return false;
                at = close + 2;
            } else {
                at += 2;
                const std::string_view name = ReadName(at);
                while (at < src.size() && IsSpace(src[at])) at++;
                if (name.empty() || at >= src.size() || src[at] != '>' || open_.empty()) return false;
                if (!Same(document_.nodes[static_cast<size_t>(open_.back().node)].name, name)) return false;
                Close();
                at++;
            }
        }
        return document_.root >= 0 && open_.empty();
    }

private:
    struct Open {
        int node;
        int lastChild;
        bool foreignDefault;   // the default namespace is not SVG's
    };

    std::string_view ReadName(size_t& at) const {
        const char* const begin = source_.data() + at;
        const char* const end = source_.data() + source_.size();
        const char* p = begin;
        if (p < end && HasClass(*p, kByteNameStart)) {
            for (p++; p < end && HasClass(*p, kByteName);) p++;
        }
        at += static_cast<size_t>(p - begin);
        return std::string_view(begin, static_cast<size_t>(p - begin));
    }

    // Decodes references, and in attribute values turns tabs and line ends into spaces; '<' in
    // an attribute value is not well formed. The view itself when there is nothing to decode.
    bool Decode(std::string_view raw, bool attribute, std::string_view& out) {
        const char* const begin = raw.data();
        const char* const end = begin + raw.size();
        const char* p = begin;
        if (attribute) {
            while (p < end && !HasClass(*p, kByteDecode)) p++;
        } else {
            p = static_cast<const char*>(std::memchr(begin, '&', raw.size()));
            if (p == nullptr) p = end;
        }
        if (p == end) {
            out = raw;
            return true;
        }
        // Runs between the bytes that need attention are copied whole.
        std::string text(begin, static_cast<size_t>(p - begin));
        text.reserve(raw.size());
        while (p < end) {
            const char ch = *p;
            if (ch == '<') return false;
            if (ch != '&') {
                // A tab or line end in an attribute value; CR LF is one line end (XML 2.11).
                text += ' ';
                p += ch == '\r' && p + 1 < end && p[1] == '\n' ? 2 : 1;
            } else {
                const char* semicolon = p + 1;
                while (semicolon < end && semicolon < p + 12 && *semicolon != ';') semicolon++;
                if (semicolon >= end || *semicolon != ';') return false;
                const std::string_view name(p + 1, static_cast<size_t>(semicolon - p - 1));
                if (name == "lt") text += '<';
                else if (name == "gt") text += '>';
                else if (name == "amp") text += '&';
                else if (name == "quot") text += '"';
                else if (name == "apos") text += '\'';
                else if (name.size() >= 2 && name[0] == '#') {
                    uint32_t code = 0;
                    const bool hex = name[1] == 'x';
                    const std::string_view digits = name.substr(hex ? 2 : 1);
                    if (digits.empty()) return false;
                    for (char digit : digits) {
                        const int value = hex ? HexDigit(digit) : (IsDigit(digit) ? digit - '0' : -1);
                        if (value < 0) return false;
                        code = code * (hex ? 16u : 10u) + static_cast<uint32_t>(value);
                        if (code > 0x10FFFF) return false;
                    }
                    if (code == 0 || (code >= 0xD800 && code <= 0xDFFF)) return false;
                    AppendUtf8(text, code);
                } else {
                    return false;
                }
                p = semicolon + 1;
            }
            const char* const run = p;
            if (attribute) {
                while (p < end && !HasClass(*p, kByteDecode)) p++;
            } else {
                p = static_cast<const char*>(std::memchr(p, '&', static_cast<size_t>(end - p)));
                if (p == nullptr) p = end;
            }
            text.append(run, static_cast<size_t>(p - run));
        }
        document_.decoded.push_back(std::move(text));
        out = document_.decoded.back();
        return true;
    }

    int AddNode(std::string_view name, Tag tag) {
        if (document_.nodes.size() >= kMaxNodes) return -1;
        const int index = static_cast<int>(document_.nodes.size());
        Node node;
        node.name = name;
        node.tag = tag;
        if (!open_.empty()) {
            Open& parent = open_.back();
            node.parent = parent.node;
            if (parent.lastChild >= 0) document_.nodes[static_cast<size_t>(parent.lastChild)].nextSibling = index;
            else document_.nodes[static_cast<size_t>(parent.node)].firstChild = index;
            parent.lastChild = index;
        }
        document_.nodes.push_back(node);
        return index;
    }

    bool Text(std::string_view raw, bool cdata) {
        if (open_.empty()) return !cdata && AllSpace(raw);
        const bool space = AllSpace(raw);
        if (foreignObjects_ > 0 && !space) document_.foreignText = true;
        std::string_view text = raw;
        if (!cdata && !Decode(raw, false, text)) return false;
        const Tag parent = document_.nodes[static_cast<size_t>(open_.back().node)].tag;
        if (texts_ == 0 && (parent != kTagStyle || space)) return true;
        return AddNode(text, kTagTextNode) >= 0;
    }

    bool StartTag(size_t& at) {
        const std::string_view& src = source_;
        if (open_.empty() && document_.root >= 0) return false;   // a second root
        at++;
        const std::string_view name = ReadName(at);
        if (name.empty()) return false;
        const size_t firstAttribute = document_.attributes.size();
        bool selfClosing = false;
        while (true) {
            const size_t before = at;
            while (at < src.size() && IsSpace(src[at])) at++;
            if (at >= src.size()) return false;
            if (src[at] == '>') {
                at++;
                break;
            }
            if (src[at] == '/') {
                if (at + 1 >= src.size() || src[at + 1] != '>') return false;
                at += 2;
                selfClosing = true;
                break;
            }
            if (at == before) return false;
            const std::string_view attributeName = ReadName(at);
            if (attributeName.empty()) return false;
            while (at < src.size() && IsSpace(src[at])) at++;
            if (at >= src.size() || src[at] != '=') return false;
            at++;
            while (at < src.size() && IsSpace(src[at])) at++;
            if (at >= src.size() || (src[at] != '"' && src[at] != '\'')) return false;
            // The value: read where it is unless it holds a reference or white space to normalise.
            const char quote = src[at];
            const char* const valueBegin = src.data() + at + 1;
            const char* const srcEnd = src.data() + src.size();
            const char* p = valueBegin;
            while (p < srcEnd && *p != quote && !HasClass(*p, kByteDecode)) p++;
            size_t close = static_cast<size_t>(p - src.data());
            if (p < srcEnd && *p != quote) close = src.find(quote, close);
            if (close >= src.size()) return false;
            std::string_view value(valueBegin, close - at - 1);
            if (*p != quote && !Decode(value, true, value)) return false;
            if (document_.attributes.size() - firstAttribute >= kMaxAttributes) return false;
            document_.attributes.push_back({ attributeName, value });
            at = close + 1;
        }
        if (++document_.elements > kMaxElements) return false;

        // The namespace: "svg:" or a default namespace that is SVG's (or none).
        bool foreignDefault = open_.empty() ? false : open_.back().foreignDefault;
        for (size_t index = firstAttribute; index < document_.attributes.size(); index++) {
            const Attribute& attribute = document_.attributes[index];
            if (attribute.name == "xmlns") foreignDefault = !attribute.value.empty() && attribute.value != kSvgNamespace;
        }
        std::string_view local = name;
        bool foreign = foreignDefault;
        const size_t colon = name.find(':');
        if (colon != std::string_view::npos) {
            local = name.substr(colon + 1);
            foreign = name.substr(0, colon) != "svg";
        }
        if (local == "script") document_.script = true;
        const Tag tag = foreign ? kTagForeign : TagOf(local);
        const int index = AddNode(name, tag);
        if (index < 0) return false;
        Node& node = document_.nodes[static_cast<size_t>(index)];
        node.firstAttribute = static_cast<uint32_t>(firstAttribute);
        node.attributeCount = static_cast<uint32_t>(document_.attributes.size() - firstAttribute);
        if (document_.root < 0) document_.root = index;
        if (!selfClosing) {
            if (open_.size() >= kMaxDepth) return false;
            open_.push_back({ index, -1, foreignDefault });
            if (tag == kTagText) texts_++;
            if (tag == kTagForeignObject) foreignObjects_++;
        }
        return true;
    }

    void Close() {
        const Tag tag = document_.nodes[static_cast<size_t>(open_.back().node)].tag;
        if (tag == kTagText) texts_--;
        if (tag == kTagForeignObject) foreignObjects_--;
        open_.pop_back();
    }

    std::string_view source_;
    Document& document_;
    std::vector<Open> open_;
    int texts_ = 0;            // open text elements, whose white space matters
    int foreignObjects_ = 0;   // open foreignObject elements
};

// The local name of an element: its name after a prefix.
std::string_view LocalName(std::string_view name) {
    const size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

// ---------------------------------------------------------------------------------------------
// Style

enum PaintKind : uint8_t { kPaintNone, kPaintColor, kPaintCurrent, kPaintServer };

struct Paint {
    PaintKind kind = kPaintNone;
    uint32_t rgb = 0;
    double alpha = 1.0;
    int server = -1;   // a gradient or pattern element
};

enum Anchor : uint8_t { kAnchorStart, kAnchorMiddle, kAnchorEnd };

// Computed values of the properties RayoMD draws: inherited ones first, then those each
// element starts afresh (opacity, display, clip-path, stop-*).
struct Style {
    Paint fill{ kPaintColor, 0, 1.0, -1 };
    Paint stroke;
    double fillOpacity = 1.0;
    double strokeOpacity = 1.0;
    double strokeWidth = 1.0;
    double miterLimit = 4.0;
    double dashOffset = 0.0;
    double fontSize = 16.0;
    std::string_view dashArray;   // empty for none
    uint32_t color = 0;
    uint8_t cap = 0;              // PDF's J
    uint8_t join = 0;             // PDF's j
    Anchor anchor = kAnchorStart;
    bool fillEvenOdd = false;
    bool clipEvenOdd = false;
    bool hidden = false;
    bool bold = false;
    bool preserveSpace = false;
    // Not inherited
    double opacity = 1.0;
    double stopOpacity = 1.0;
    double stopAlpha = 1.0;       // the alpha of stop-color
    uint32_t stopColor = 0;
    bool stopCurrent = false;
    bool displayNone = false;
    bool overflowVisible = false; // a nested viewport does not clip
    int clip = -1;                // a clipPath element
};

enum Axis : uint8_t { kAxisX, kAxisY, kAxisOther };

struct AspectRatio {
    int alignX = 1;     // 0 min, 1 mid, 2 max
    int alignY = 1;
    bool none = false;
    bool slice = false;
};

bool ParseAspectRatio(std::string_view text, AspectRatio& out) {
    text = Trim(text);
    if (text.empty()) return false;
    if (StartsWithIgnoreCase(text, "defer")) text = Trim(text.substr(5));
    size_t space = 0;
    while (space < text.size() && !IsSpace(text[space])) space++;
    const std::string_view align = text.substr(0, space);
    const std::string_view mode = Trim(text.substr(space));
    AspectRatio result;
    if (align == "none") {
        result.none = true;
    } else if (align.size() == 8 && align[0] == 'x' && align[4] == 'Y') {
        const std::string_view x = align.substr(1, 3);
        const std::string_view y = align.substr(5, 3);
        result.alignX = x == "Min" ? 0 : x == "Mid" ? 1 : x == "Max" ? 2 : -1;
        result.alignY = y == "Min" ? 0 : y == "Mid" ? 1 : y == "Max" ? 2 : -1;
        if (result.alignX < 0 || result.alignY < 0) return false;
    } else {
        return false;
    }
    if (mode == "slice") result.slice = true;
    else if (!mode.empty() && mode != "meet") return false;
    out = result;
    return true;
}

// The transform that fits the viewBox (x y w h) into a viewport (0 0 width height).
Matrix ViewBoxTransform(const double (&box)[4], double width, double height, const AspectRatio& aspect) {
    double sx = width / box[2];
    double sy = height / box[3];
    if (!aspect.none) {
        const double scale = aspect.slice ? std::max(sx, sy) : std::min(sx, sy);
        sx = sy = scale;
    }
    double tx = -box[0] * sx;
    double ty = -box[1] * sy;
    tx += (width - box[2] * sx) * aspect.alignX / 2.0;
    ty += (height - box[3] * sy) * aspect.alignY / 2.0;
    return { sx, 0.0, 0.0, sy, tx, ty };
}

bool ParseViewBox(std::string_view text, double (&box)[4]) {
    const char* at = text.data();
    const char* end = at + text.size();
    for (int index = 0; index < 4; index++) {
        SkipCommaSpace(at, end);
        if (!ScanNumber(at, end, box[index]) || !std::isfinite(box[index])) return false;
    }
    SkipSpace(at, end);
    return at == end;
}

// A transform list; false (identity) on a syntax error or a number that is not finite.
bool ParseTransform(std::string_view text, Matrix& out) {
    Matrix result;
    const char* at = text.data();
    const char* end = at + text.size();
    while (true) {
        SkipCommaSpace(at, end);
        if (at == end) break;
        const char* nameBegin = at;
        while (at < end && IsAlpha(*at)) at++;
        const std::string_view name(nameBegin, static_cast<size_t>(at - nameBegin));
        SkipSpace(at, end);
        if (at == end || *at != '(') return false;
        at++;
        double args[6];
        int count = 0;
        while (true) {
            SkipCommaSpace(at, end);
            if (at < end && *at == ')') {
                at++;
                break;
            }
            if (count == 6 || !ScanNumber(at, end, args[count]) || !std::isfinite(args[count])) return false;
            count++;
        }
        Matrix step;
        if (name == "matrix" && count == 6) {
            step = { args[0], args[1], args[2], args[3], args[4], args[5] };
        } else if (name == "translate" && (count == 1 || count == 2)) {
            step = Translation(args[0], count == 2 ? args[1] : 0.0);
        } else if (name == "scale" && (count == 1 || count == 2)) {
            step = { args[0], 0.0, 0.0, count == 2 ? args[1] : args[0], 0.0, 0.0 };
        } else if (name == "rotate" && (count == 1 || count == 3)) {
            const double angle = std::fmod(args[0], 360.0) * kPi / 180.0;
            const double cosine = std::cos(angle);
            const double sine = std::sin(angle);
            step = { cosine, sine, -sine, cosine, 0.0, 0.0 };
            if (count == 3) step = Multiply(Multiply(Translation(args[1], args[2]), step), Translation(-args[1], -args[2]));
        } else if (name == "skewX" && count == 1) {
            step.c = std::tan(args[0] * kPi / 180.0);
        } else if (name == "skewY" && count == 1) {
            step.b = std::tan(args[0] * kPi / 180.0);
        } else {
            return false;
        }
        result = Multiply(result, step);
    }
    for (double value : { result.a, result.b, result.c, result.d, result.e, result.f }) {
        if (!std::isfinite(value)) return false;
    }
    out = result;
    return true;
}

// The id of a local reference: "#id", "url(#id)", "url('#id')"; `rest` is what follows the url().
bool ParseUrl(std::string_view text, std::string_view& id, std::string_view& rest) {
    text = Trim(text);
    if (!StartsWithIgnoreCase(text, "url(")) return false;
    const size_t close = text.find(')');
    if (close == std::string_view::npos) return false;
    std::string_view inner = Trim(text.substr(4, close - 4));
    if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'') && inner.back() == inner[0]) {
        inner = Trim(inner.substr(1, inner.size() - 2));
    }
    if (inner.empty() || inner[0] != '#') return false;
    id = inner.substr(1);
    rest = Trim(text.substr(close + 1));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Paths

// What a PathWriter tracks of the bounding box of its points.
enum BoxMode : uint8_t {
    kNoBox,
    kLocalBox,         // of the points as given (a gradient's objectBoundingBox)
    kTransformedBox,   // of the points as transformed (an objectBoundingBox clip's measure)
};

// Writes a path into the content stream: points transformed by `matrix`, numbers with two
// decimals (four for paths drawn under cm), a segment at a time. A subpath of only a moveto is
// dropped. Marks the path bad when a number is not finite or past the content stream's limit,
// and full when it would pass `limit` bytes.
class PathWriter {
public:
    PathWriter(std::string* out, const Matrix& matrix, bool fourDecimals, BoxMode boxMode, size_t limit = 0)
        : out_(out), m_(matrix), limit_(limit), fourDecimals_(fourDecimals), boxMode_(boxMode) {}

    bool bad = false;
    bool full = false;
    Box box;

    void MoveTo(double x, double y) {
        pending_ = true;
        moveX_ = x;
        moveY_ = y;
    }

    void LineTo(double x, double y) {
        Flush();
        Point(x, y);
        AddBox(x, y);
        Operator('l');
    }

    void CubicTo(double x1, double y1, double x2, double y2, double x3, double y3) {
        Flush();
        Point(x1, y1);
        Point(x2, y2);
        Point(x3, y3);
        if (boxMode_ != kNoBox) {
            double xs[4] = { lastX_, x1, x2, x3 };
            double ys[4] = { lastY_, y1, y2, y3 };
            if (boxMode_ == kTransformedBox) {
                for (int index = 0; index < 4; index++) {
                    const double x = xs[index], y = ys[index];
                    xs[index] = m_.a * x + m_.c * y + m_.e;
                    ys[index] = m_.b * x + m_.d * y + m_.f;
                }
            }
            box.Add(xs[3], ys[3]);
            AddCubicExtremes(box, xs, ys, xs);
            AddCubicExtremes(box, xs, ys, ys);
        }
        lastX_ = x3;
        lastY_ = y3;
        Operator('c');
    }

    void Close() {
        if (pending_) {
            pending_ = false;
            return;
        }
        if (open_) Operator('h');
        open_ = false;
    }

    // A rectangle: "re" when the matrix keeps it axis-aligned, its corners in the order of the
    // SVG rect's path so that winding stays the same.
    void Rect(double x, double y, double width, double height) {
        if (m_.b == 0.0 && m_.c == 0.0) {
            pending_ = false;
            open_ = false;
            const double x0 = m_.a * x + m_.e, y0 = m_.d * y + m_.f;
            const double x1 = m_.a * (x + width) + m_.e, y1 = m_.d * (y + height) + m_.f;
            AddBox(x, y);
            AddBox(x + width, y + height);
            if (!Fits(x0) || !Fits(y0) || !Fits(x1) || !Fits(y1)) bad = true;
            Number(x0);
            Number(y0);
            Number(x1 - x0);
            Number(y1 - y0);
            *cursor_++ = 'r';
            Operator('e');
            return;
        }
        MoveTo(x, y);
        LineTo(x + width, y);
        LineTo(x + width, y + height);
        LineTo(x, y + height);
        Close();
    }

private:
    void Flush() {
        if (!pending_) return;
        pending_ = false;
        open_ = true;
        Point(moveX_, moveY_);
        AddBox(moveX_, moveY_);
        Operator('m');
    }

    void AddBox(double x, double y) {
        lastX_ = x;
        lastY_ = y;
        if (boxMode_ == kTransformedBox) box.Add(m_.a * x + m_.c * y + m_.e, m_.b * x + m_.d * y + m_.f);
        else if (boxMode_ == kLocalBox) box.Add(x, y);
    }

    void Point(double x, double y) {
        const double tx = m_.a * x + m_.c * y + m_.e;
        const double ty = m_.b * x + m_.d * y + m_.f;
        if (!Fits(tx) || !Fits(ty)) bad = true;
        Number(tx);
        Number(ty);
    }

    // Adds a number and a space to the segment; nothing once the path is bad.
    void Number(double value) {
        if (out_ == nullptr || bad) return;
        cursor_ = fourDecimals_ ? WriteFixed4(cursor_, value) : WriteNumber(cursor_, value);
        *cursor_++ = ' ';
    }

    // Ends the segment with its operator and appends it to the content stream.
    void Operator(char op) {
        if (out_ != nullptr && !bad) {
            *cursor_++ = op;
            *cursor_++ = '\n';
            const size_t size = static_cast<size_t>(cursor_ - segment_);
            if (out_->size() + size > limit_) full = bad = true;
            else out_->append(segment_, size);
        }
        cursor_ = segment_;
    }

    std::string* out_;
    Matrix m_;
    size_t limit_;
    bool fourDecimals_;
    BoxMode boxMode_;
    bool pending_ = false;
    bool open_ = false;
    double moveX_ = 0.0, moveY_ = 0.0;
    double lastX_ = 0.0, lastY_ = 0.0;
    char segment_[96];   // a cubic: six numbers of at most 12 characters with their spaces, "c\n"
    char* cursor_ = segment_;
};

// An elliptical arc as at most four cubics (SVG 1.1 F.6.5, F.6.6).
void ArcTo(PathWriter& writer, double x1, double y1, double rx, double ry, double angle, bool large, bool sweep,
           double x2, double y2) {
    if (x1 == x2 && y1 == y2) return;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0.0 || ry == 0.0) {
        writer.LineTo(x2, y2);
        return;
    }
    const double phi = std::fmod(angle, 360.0) * kPi / 180.0;
    const double cosine = std::cos(phi);
    const double sine = std::sin(phi);
    const double dx = (x1 - x2) / 2.0;
    const double dy = (y1 - y2) / 2.0;
    const double xp = cosine * dx + sine * dy;
    const double yp = -sine * dx + cosine * dy;
    const double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1.0) {
        const double root = std::sqrt(lambda);
        rx *= root;
        ry *= root;
    }
    const double numerator = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    const double denominator = rx * rx * yp * yp + ry * ry * xp * xp;
    double coefficient = denominator > 0.0 ? std::sqrt(std::max(0.0, numerator / denominator)) : 0.0;
    if (large == sweep) coefficient = -coefficient;
    const double cxp = coefficient * rx * yp / ry;
    const double cyp = -coefficient * ry * xp / rx;
    const double cx = cosine * cxp - sine * cyp + (x1 + x2) / 2.0;
    const double cy = sine * cxp + cosine * cyp + (y1 + y2) / 2.0;
    const double ux = (xp - cxp) / rx, uy = (yp - cyp) / ry;
    const double vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
    const double theta = std::atan2(uy, ux);
    double delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0.0) delta -= 2.0 * kPi;
    else if (sweep && delta < 0.0) delta += 2.0 * kPi;
    if (!std::isfinite(delta) || !std::isfinite(cx) || !std::isfinite(cy)) {
        writer.LineTo(x2, y2);
        return;
    }
    const int segments = std::clamp(static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 2.0) - 1e-9)), 1, 4);
    const double step = delta / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    auto mapX = [&](double px, double py) { return cx + cosine * rx * px - sine * ry * py; };
    auto mapY = [&](double px, double py) { return cy + sine * rx * px + cosine * ry * py; };
    for (int index = 0; index < segments; index++) {
        const double a = theta + index * step;
        const double b = a + step;
        const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
        const double p1x = ca - k * sa, p1y = sa + k * ca;
        const double p2x = cb + k * sb, p2y = sb - k * cb;
        const bool last = index == segments - 1;
        writer.CubicTo(mapX(p1x, p1y), mapY(p1x, p1y), mapX(p2x, p2y), mapY(p2x, p2y),
                       last ? x2 : mapX(cb, sb), last ? y2 : mapY(cb, sb));
    }
}

// Path data (SVG 2 path grammar): draws up to the last complete segment of malformed data.
// Adds the numbers read to `numbers`; false when they pass the document's limit.
bool ParsePathData(std::string_view data, PathWriter& writer, size_t& numbers) {
    const char* at = data.data();
    const char* end = at + data.size();
    double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0;
    double c2x = 0.0, c2y = 0.0, q1x = 0.0, q1y = 0.0;
    char command = 0;
    char previous = 0;
    bool first = true;
    while (true) {
        SkipSpace(at, end);
        if (at < end && *at == ',' && command != 0) {
            at++;
            SkipSpace(at, end);
        }
        if (at == end) break;
        if (IsAlpha(*at)) {
            command = *at++;
            if (first && command != 'M' && command != 'm') return true;
        } else if (command == 0 || command == 'Z' || command == 'z') {
            return true;
        } else if (command == 'M') {
            command = 'L';
        } else if (command == 'm') {
            command = 'l';
        }
        const char upper = static_cast<char>(command & ~0x20);
        const bool relative = command != upper && !first;
        int count;
        switch (upper) {
        case 'Z': count = 0; break;
        case 'M': case 'L': case 'T': count = 2; break;
        case 'H': case 'V': count = 1; break;
        case 'S': case 'Q': count = 4; break;
        case 'C': count = 6; break;
        case 'A': count = 7; break;
        default: return true;
        }
        first = false;
        double args[7];
        for (int index = 0; index < count; index++) {
            SkipCommaSpace(at, end);
            if (upper == 'A' && (index == 3 || index == 4)) {
                if (at == end || (*at != '0' && *at != '1')) return true;
                args[index] = *at++ == '1' ? 1.0 : 0.0;
            } else if (!ScanNumber(at, end, args[index]) || !std::isfinite(args[index])) {
                return true;
            }
        }
        numbers += static_cast<size_t>(count);
        if (numbers > kMaxPathNumbers) return false;
        if (writer.full) return true;   // past the content limit: the caller fails
        const double ox = relative ? cx : 0.0;
        const double oy = relative ? cy : 0.0;
        switch (upper) {
        case 'Z':
            writer.Close();
            cx = sx;
            cy = sy;
            writer.MoveTo(sx, sy);   // a segment after Z starts at the subpath's start
            break;
        case 'M':
            cx = sx = args[0] + ox;
            cy = sy = args[1] + oy;
            writer.MoveTo(cx, cy);
            break;
        case 'L':
            cx = args[0] + ox;
            cy = args[1] + oy;
            writer.LineTo(cx, cy);
            break;
        case 'H':
            cx = args[0] + ox;
            writer.LineTo(cx, cy);
            break;
        case 'V':
            cy = args[0] + (relative ? cy : 0.0);
            writer.LineTo(cx, cy);
            break;
        case 'C':
            c2x = args[2] + ox;
            c2y = args[3] + oy;
            writer.CubicTo(args[0] + ox, args[1] + oy, c2x, c2y, args[4] + ox, args[5] + oy);
            cx = args[4] + ox;
            cy = args[5] + oy;
            break;
        case 'S': {
            const bool reflect = previous == 'C' || previous == 'S';
            const double x1 = reflect ? 2.0 * cx - c2x : cx;
            const double y1 = reflect ? 2.0 * cy - c2y : cy;
            c2x = args[0] + ox;
            c2y = args[1] + oy;
            writer.CubicTo(x1, y1, c2x, c2y, args[2] + ox, args[3] + oy);
            cx = args[2] + ox;
            cy = args[3] + oy;
            break;
        }
        case 'Q':
        case 'T': {
            if (upper == 'Q') {
                q1x = args[0] + ox;
                q1y = args[1] + oy;
            } else {
                const bool reflect = previous == 'Q' || previous == 'T';
                q1x = reflect ? 2.0 * cx - q1x : cx;
                q1y = reflect ? 2.0 * cy - q1y : cy;
            }
            const double x = (upper == 'Q' ? args[2] : args[0]) + ox;
            const double y = (upper == 'Q' ? args[3] : args[1]) + oy;
            writer.CubicTo(cx + 2.0 / 3.0 * (q1x - cx), cy + 2.0 / 3.0 * (q1y - cy),
                           x + 2.0 / 3.0 * (q1x - x), y + 2.0 / 3.0 * (q1y - y), x, y);
            cx = x;
            cy = y;
            break;
        }
        case 'A': {
            const double x = args[5] + ox;
            const double y = args[6] + oy;
            ArcTo(writer, cx, cy, args[0], args[1], args[2], args[3] != 0.0, args[4] != 0.0, x, y);
            cx = x;
            cy = y;
            break;
        }
        default:
            break;
        }
        previous = upper;
    }
    return true;
}

// A points list (polyline, polygon); an odd last number is dropped.
bool ParsePoints(std::string_view data, PathWriter& writer, bool close, size_t& numbers) {
    const char* at = data.data();
    const char* end = at + data.size();
    bool first = true;
    while (true) {
        double x, y;
        SkipCommaSpace(at, end);
        if (!ScanNumber(at, end, x) || !std::isfinite(x)) break;
        SkipCommaSpace(at, end);
        if (!ScanNumber(at, end, y) || !std::isfinite(y)) break;
        numbers += 2;
        if (numbers > kMaxPathNumbers) return false;
        if (first) writer.MoveTo(x, y);
        else writer.LineTo(x, y);
        first = false;
    }
    if (close && !first) writer.Close();
    return true;
}

// Base64 with white space; false on any other byte.
bool DecodeBase64(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t bits = 0;
    int count = 0;
    bool padded = false;
    for (char ch : text) {
        int value;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '+' || ch == '-') value = 62;
        else if (ch == '/' || ch == '_') value = 63;
        else if (ch == '=') { padded = true; continue; }
        else if (IsSpace(ch)) continue;
        else return false;
        if (padded) return false;
        bits = bits << 6 | static_cast<uint32_t>(value);
        if (++count == 4) {
            out += static_cast<char>(bits >> 16);
            out += static_cast<char>(bits >> 8 & 0xFF);
            out += static_cast<char>(bits & 0xFF);
            bits = 0;
            count = 0;
        }
    }
    if (count == 2) {
        out += static_cast<char>(bits >> 4);
    } else if (count == 3) {
        out += static_cast<char>(bits >> 10);
        out += static_cast<char>(bits >> 2 & 0xFF);
    } else if (count == 1) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The converter

enum Mode : uint8_t {
    kModePaint,     // draw into the content stream
    kModeClip,      // add the geometry of a clipPath's children to the clip
    kModeMeasure,   // add the bounding box of the geometry (objectBoundingBox clips)
};

struct Context {
    Mode mode = kModePaint;
    double opacity = 1.0;   // group opacity folded down to the leaves
    int useDepth = 0;
    int use = -1;           // the use this element is the instance of: sets an svg's or symbol's viewport
};

// A fill or stroke as drawn: a color with its alpha, or a linear gradient.
struct ResolvedPaint {
    enum Kind : uint8_t { kNone, kSolid, kLinear } kind = kNone;
    uint32_t rgb = 0;
    double alpha = 1.0;
    int server = -1;
};

struct GradientStop {
    double offset;
    uint32_t rgb;
    double alpha;
};

struct Gradient {
    bool radial = false;
    bool userSpace = false;
    Matrix transform;
    double x1 = 0.0, y1 = 0.0, x2 = 1.0, y2 = 0.0;
    std::vector<GradientStop> stops;
};

// What the content stream has set, so that an operator is written only when a value changes.
struct GraphicsState {
    uint32_t fill = 0;
    uint32_t stroke = 0;
    int fillAlpha = 1000;     // thousandths
    int strokeAlpha = 1000;
    int64_t width = 10000;    // ten-thousandths
    int64_t miter = 100000;
    uint8_t cap = 0;
    uint8_t join = 0;
    uint8_t textRender = 0;
    std::string dash = "[] 0";
};

struct TextRun {
    std::string text;
    double absoluteX, absoluteY, dx, dy;   // NaN when not given
    double x = 0.0, y = 0.0, width = 0.0;
    double fontSize;
    bool bold;
    bool hidden;
    bool preserveSpace;
    Anchor anchor;
    ResolvedPaint fill;
};

// A declaration of a style sheet rule.
struct SheetDeclaration {
    Property property;
    bool important;
    std::string_view value;
};

// One selector of a style sheet rule, a compound of a type or "*", classes and ids.
struct Selector {
    std::string_view text;
    std::string_view key;     // where it is filed: "#id", ".class", its type or "*"
    uint32_t specificity;     // ids << 16 | classes << 8 | types
    uint32_t begin, end;      // its rule's declarations
    int next;                 // the next selector, in document order, filed under the same key
};

class Converter {
public:
    Converter(const Document& document, size_t sourceBytes, SvgHost& host, SvgForm& form)
        : doc_(document), host_(host), form_(form), out_(form.content), workLimit_(12 * sourceBytes + (16u << 20)) {}

    bool Run() {
        const Node& root = doc_.nodes[static_cast<size_t>(doc_.root)];
        if (root.tag != kTagSvg || doc_.script || doc_.foreignText) return false;
        for (size_t index = 0; index < doc_.nodes.size(); index++) {
            const Node& node = doc_.nodes[index];
            if (node.tag == kTagTextNode || node.tag == kTagForeign) continue;
            std::string_view id;
            if (Attr(static_cast<int>(index), "id", id) && !id.empty()) ids_.emplace(id, static_cast<int>(index));
        }
        if (!ReadStyleSheets() || styleDependent_) return false;
        budget_ = 4 * doc_.elements + 100000;
        active_.assign(doc_.nodes.size(), 0);

        // The viewport: width and height in absolute units, else from the viewBox, else 300 x 150 px.
        double box[4] = { 0.0, 0.0, 0.0, 0.0 };
        std::string_view text;
        const bool hasBox = Attr(doc_.root, "viewBox", text) && ParseViewBox(text, box);
        if (hasBox && (box[2] <= 0.0 || box[3] <= 0.0)) return false;
        double width = 0.0, height = 0.0;
        const bool hasWidth = Attr(doc_.root, "width", text) && AbsoluteLength(text, width);
        const bool hasHeight = Attr(doc_.root, "height", text) && AbsoluteLength(text, height);
        if (!hasWidth || !hasHeight) {
            if (hasBox) {
                if (hasWidth) height = width * box[3] / box[2];
                else if (hasHeight) width = height * box[2] / box[3];
                else {
                    width = box[2];
                    height = box[3];
                }
            } else {
                if (!hasWidth) width = 300.0;
                if (!hasHeight) height = 150.0;
            }
        }
        const double formWidth = width * kPxToPt;
        const double formHeight = height * kPxToPt;
        if (!(formWidth > 0.0 && formHeight > 0.0 && formWidth <= kMaxReal && formHeight <= kMaxReal)) return false;
        viewportWidth_ = hasBox ? box[2] : width;
        viewportHeight_ = hasBox ? box[3] : height;
        viewportDiagonal_ = std::sqrt((viewportWidth_ * viewportWidth_ + viewportHeight_ * viewportHeight_) / 2.0);

        Matrix ctm{ kPxToPt, 0.0, 0.0, -kPxToPt, 0.0, formHeight };
        if (hasBox) {
            AspectRatio aspect;
            if (Attr(doc_.root, "preserveAspectRatio", text)) ParseAspectRatio(text, aspect);
            ctm = Multiply(ctm, ViewBoxTransform(box, width, height, aspect));
        }
        states_.assign(1, GraphicsState());
        out_.clear();
        Render(doc_.root, Style(), ctm, Context(), 0);
        if (failed_ || paintCount_ == 0 || Used() > kMaxContentBytes) return false;
        form_.width = formWidth;
        form_.height = formHeight;
        return true;
    }

private:
    // ----- Attributes

    bool Attr(int index, Name name, std::string_view& value) const {
        const Node& node = doc_.nodes[static_cast<size_t>(index)];
        for (uint32_t at = node.firstAttribute; at < node.firstAttribute + node.attributeCount; at++) {
            const Attribute& attribute = doc_.attributes[at];
            if (Same(attribute.name, name.text)) {
                value = attribute.value;
                return true;
            }
        }
        return false;
    }

    std::string_view Href(int index) const {
        std::string_view value;
        if (Attr(index, "href", value) || Attr(index, "xlink:href", value)) return Trim(value);
        return {};
    }

    int Lookup(std::string_view id) const {
        const auto found = ids_.find(id);
        return found == ids_.end() ? -1 : found->second;
    }

    // "#id" → its element, or -1.
    int Reference(std::string_view href) const {
        return href.size() > 1 && href[0] == '#' ? Lookup(href.substr(1)) : -1;
    }

    Tag TagAt(int index) const { return doc_.nodes[static_cast<size_t>(index)].tag; }

    // A length in user units (px): unitless, px, pt, pc, in, cm, mm, Q, em, ex, %.
    bool Length(std::string_view text, double fontSize, Axis axis, double& out) const {
        text = Trim(text);
        const char* at = text.data();
        const char* end = at + text.size();
        double value;
        if (!ScanNumber(at, end, value) || !std::isfinite(value)) return false;
        const std::string_view unit(at, static_cast<size_t>(end - at));
        double scale;
        if (unit.empty() || EqualsIgnoreCase(unit, "px")) scale = 1.0;
        else if (EqualsIgnoreCase(unit, "pt")) scale = 4.0 / 3.0;
        else if (EqualsIgnoreCase(unit, "pc")) scale = 16.0;
        else if (EqualsIgnoreCase(unit, "in")) scale = 96.0;
        else if (EqualsIgnoreCase(unit, "cm")) scale = 96.0 / 2.54;
        else if (EqualsIgnoreCase(unit, "mm")) scale = 96.0 / 25.4;
        else if (EqualsIgnoreCase(unit, "q")) scale = 96.0 / 101.6;
        else if (EqualsIgnoreCase(unit, "em")) scale = fontSize;
        else if (EqualsIgnoreCase(unit, "ex")) scale = fontSize / 2.0;
        else if (unit == "%") {
            scale = (axis == kAxisX ? viewportWidth_ : axis == kAxisY ? viewportHeight_ : viewportDiagonal_) / 100.0;
        } else {
            return false;
        }
        out = value * scale;
        return std::isfinite(out);
    }

    // The root's width or height: an absolute length (not %, em) in px.
    bool AbsoluteLength(std::string_view text, double& out) const {
        text = Trim(text);
        if (text.empty() || text.back() == '%' || (text.size() >= 2 && (Lower(text.back()) == 'm' || Lower(text.back()) == 'x') &&
                                                  Lower(text[text.size() - 2]) == 'e')) {
            return false;
        }
        return Length(text, 16.0, kAxisOther, out) && out > 0.0;
    }

    double LengthAttr(int index, Name name, Axis axis, double fontSize, double fallback) const {
        std::string_view text;
        double value;
        if (Attr(index, name, text) && Length(text, fontSize, axis, value)) return value;
        return fallback;
    }

    // The first length of a list attribute (text x, y, dx, dy), NaN when there is none.
    double FirstLength(int index, Name name, Axis axis, double fontSize) const {
        std::string_view text;
        if (!Attr(index, name, text)) return NAN;
        text = Trim(text);
        size_t end = 0;
        while (end < text.size() && !IsSpace(text[end]) && text[end] != ',') end++;
        double value;
        return Length(text.substr(0, end), fontSize, axis, value) ? value : NAN;
    }

    // ----- Style sheets

    // The <style> elements: rules whose selectors are compounds of a type or "*", classes and ids
    // apply; a rule of any other selector (combinators, attributes, pseudo-classes) that sets a
    // property RayoMD draws makes the drawing depend on selectors it does not match: not drawn.
    bool ReadStyleSheets() {
        size_t total = 0;
        for (size_t index = 0; index < doc_.nodes.size(); index++) {
            if (doc_.nodes[index].tag != kTagStyle) continue;
            std::string_view type;
            if (Attr(static_cast<int>(index), "type", type) && !Trim(type).empty() && !EqualsIgnoreCase(Trim(type), "text/css")) continue;
            std::string sheet;
            for (int child = doc_.nodes[index].firstChild; child >= 0; child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                if (TagAt(child) == kTagTextNode) sheet += doc_.nodes[static_cast<size_t>(child)].name;
            }
            total += sheet.size();
            if (total > kMaxStyleBytes) return false;
            if (!sheet.empty()) {
                sheets_.push_back(std::move(sheet));
                if (!ParseSheet(sheets_.back())) return false;
            }
        }
        // Filed by each selector's most specific part, so that an element looks up only the
        // rules that may match it; each key's selectors chained in document order.
        for (size_t index = selectors_.size(); index-- > 0;) {
            const std::string_view key = selectors_[index].key;
            if (key == "*") {
                selectors_[index].next = universal_;
                universal_ = static_cast<int>(index);
                continue;
            }
            keyKinds_ |= key[0] == '#' ? kIdKeys : key[0] == '.' ? kClassKeys : kTypeKeys;
            const auto filed = selectorIndex_.emplace(key, static_cast<int>(index));
            if (!filed.second) {
                selectors_[index].next = filed.first->second;
                filed.first->second = static_cast<int>(index);
            }
        }
        return true;
    }

    bool ParseSheet(std::string& sheet) {
        BlankComments(sheet);
        const std::string_view css = sheet;
        size_t at = 0;
        while (at < css.size()) {
            while (at < css.size() && IsSpace(css[at])) at++;
            if (at >= css.size()) break;
            if (css[at] == '@') {
                // An at-rule: to its semicolon or the end of its block. The rules of a grouping
                // rule (@media, @supports, ...) may apply; RayoMD does not evaluate them, so one
                // that sets a property it draws makes the drawing style-dependent. Others
                // (@font-face, @import, @keyframes, @page, ...) are skipped.
                size_t end = at + 1;
                while (end < css.size() && (IsAlpha(css[end]) || css[end] == '-')) end++;
                const std::string_view name = css.substr(at + 1, end - at - 1);
                static constexpr char kGrouping[] = "media supports layer container document -moz-document scope starting-style";
                static constexpr std::array<Word, 8> kGroupingWords = MakeWords<8>(kGrouping);
                char lower[16];
                bool grouping = false;
                if (name.size() <= sizeof lower) {
                    for (size_t index = 0; index < name.size(); index++) lower[index] = Lower(name[index]);
                    grouping = FindWord(kGrouping, kGroupingWords.data(), kGroupingWords.size(), std::string_view(lower, name.size())) != 0;
                }
                int depth = 0;
                size_t innermost = std::string_view::npos;   // after the last '{': a block of declarations
                for (; at < css.size(); at++) {
                    const char ch = css[at];
                    if (ch == ';' && depth == 0) break;
                    if (ch == '{') {
                        depth++;
                        innermost = at + 1;
                    } else if (ch == '}') {
                        if (grouping && innermost != std::string_view::npos) {
                            ForEachDeclaration(css.substr(innermost, at - innermost), [&](Property, std::string_view, bool) {
                                styleDependent_ = true;
                            });
                        }
                        innermost = std::string_view::npos;
                        if (--depth <= 0) break;
                    }
                }
                at++;
                continue;
            }
            const size_t open = css.find('{', at);
            if (open == std::string_view::npos) break;
            size_t close = css.find('}', open);
            if (close == std::string_view::npos) close = css.size();
            const std::string_view selectors = css.substr(at, open - at);
            const std::string_view block = css.substr(open + 1, close - open - 1);
            at = close + 1;
            if (++ruleCount_ > kMaxStyleRules) return false;
            const uint32_t begin = static_cast<uint32_t>(declarations_.size());
            ForEachDeclaration(block, [&](Property property, std::string_view value, bool important) {
                declarations_.push_back({ property, important, value });
                importantRules_ = importantRules_ || important;
            });
            const uint32_t end = static_cast<uint32_t>(declarations_.size());
            if (begin == end) continue;
            size_t start = 0;
            for (size_t comma = 0; comma <= selectors.size(); comma++) {
                if (comma < selectors.size() && selectors[comma] != ',') continue;
                Selector selector{ Trim(selectors.substr(start, comma - start)), {}, 0, begin, end, -1 };
                start = comma + 1;
                if (ParseSelector(selector)) selectors_.push_back(selector);
                else styleDependent_ = true;
            }
        }
        return true;
    }

    static bool IsNameChar(char ch) {
        return IsAlpha(ch) || IsDigit(ch) || ch == '-' || ch == '_' || static_cast<unsigned char>(ch) >= 0x80;
    }

    // A compound selector: [type | *] (.class | #id)*. Sets its specificity (ids, classes, types)
    // and its key: "#id", ".class", the type or "*". False for any other selector.
    static bool ParseSelector(Selector& selector) {
        const std::string_view text = selector.text;
        size_t at = 0;
        uint32_t ids = 0, classes = 0, types = 0;
        selector.key = "*";
        if (!text.empty() && text[0] == '*') {
            at = 1;
        } else {
            while (at < text.size() && IsNameChar(text[at])) at++;
            if (at > 0) {
                types = 1;
                selector.key = text.substr(0, at);
            }
        }
        while (at < text.size()) {
            const size_t mark = at;
            if (text[at] != '.' && text[at] != '#') return false;
            for (at++; at < text.size() && IsNameChar(text[at]);) at++;
            if (at == mark + 1) return false;
            if (text[mark] == '#') {
                if (ids++ == 0) selector.key = text.substr(mark, at - mark);
            } else if (classes++ == 0 && ids == 0) {
                selector.key = text.substr(mark, at - mark);
            }
        }
        if (at == 0 || ids > 255 || classes > 255) return false;   // past the specificity fields: the SVG falls back
        selector.specificity = ids << 16 | classes << 8 | types;
        return true;
    }

    // Whether the class attribute `classes` lists `name`.
    static bool HasClass(std::string_view classes, std::string_view name) {
        for (size_t at = 0; at < classes.size();) {
            while (at < classes.size() && IsSpace(classes[at])) at++;
            size_t end = at;
            while (end < classes.size() && !IsSpace(classes[end])) end++;
            if (classes.substr(at, end - at) == name) return true;
            at = end;
        }
        return false;
    }

    // Charges `bytes` to the work budget; false, failing the conversion, once it is spent.
    bool Charge(size_t bytes) const {
        work_ += bytes;
        if (work_ <= workLimit_) return true;
        failed_ = true;
        return false;
    }

    // Whether a selector found by its key matches the element: its other parts, each charged
    // to the work budget.
    bool Matches(const Selector& selector, std::string_view local, std::string_view id, std::string_view classes) const {
        const std::string_view text = selector.text;
        if (!Charge(text.size() + 8)) return false;
        size_t at = 0;
        while (at < text.size() && text[at] != '.' && text[at] != '#') at++;
        if (at > 0 && text[0] != '*' && text.substr(0, at) != local) return false;
        while (at < text.size()) {
            const size_t mark = at;
            for (at++; at < text.size() && IsNameChar(text[at]);) at++;
            const std::string_view name = text.substr(mark + 1, at - mark - 1);
            if (text[mark] == '#') {
                if (name != id) return false;
            } else {
                if (!Charge(classes.size() + 8) || !HasClass(classes, name)) return false;
            }
        }
        return true;
    }

    // Adds the selectors of a chain that match the element to matched_; stops when the work
    // budget is spent.
    void MatchChain(int index, std::string_view local, std::string_view id, std::string_view classes) const {
        for (; index >= 0 && !failed_; index = selectors_[static_cast<size_t>(index)].next) {
            if (Matches(selectors_[static_cast<size_t>(index)], local, id, classes)) matched_.push_back(static_cast<uint32_t>(index));
        }
    }

    // The same for the selectors filed under `key`.
    void Match(std::string_view key, std::string_view local, std::string_view id, std::string_view classes) const {
        if (!Charge(key.size() + 8)) return;
        const auto found = selectorIndex_.find(key);
        if (found != selectorIndex_.end()) MatchChain(found->second, local, id, classes);
    }

    // ----- Cascade

    // Applies one declaration. A value that does not parse is ignored (CSS), so it keeps what an
    // earlier declaration set.
    void Apply(Style& s, const Style& parent, Property property, std::string_view value) const {
        value = Trim(value);
        if (value == "initial" || value == "unset" || value == "revert" || value == "revert-layer") {
            // CSS-wide keywords: unset (and revert, without a user agent sheet) inherits an
            // inherited property and resets the others; initial resets.
            const bool inherited = property != kPropOpacity && property != kPropStopColor && property != kPropStopOpacity &&
                                   property != kPropDisplay && property != kPropClipPath && property != kPropOverflow;
            if (value != "initial" && inherited) Apply(s, parent, property, "inherit");
            else Apply(s, Style(), property, "inherit");
            return;
        }
        const bool inherit = value == "inherit";
        double number;
        switch (property) {
        case kPropFill:
            if (inherit) s.fill = parent.fill;
            else ParsePaint(value, s.fill);
            break;
        case kPropStroke:
            if (inherit) s.stroke = parent.stroke;
            else ParsePaint(value, s.stroke);
            break;
        case kPropFillOpacity:
            if (inherit) s.fillOpacity = parent.fillOpacity;
            else if (ParseOpacity(value, number)) s.fillOpacity = number;
            break;
        case kPropStrokeOpacity:
            if (inherit) s.strokeOpacity = parent.strokeOpacity;
            else if (ParseOpacity(value, number)) s.strokeOpacity = number;
            break;
        case kPropOpacity:
            if (inherit) s.opacity = parent.opacity;
            else if (ParseOpacity(value, number)) s.opacity = number;
            break;
        case kPropStopOpacity:
            if (inherit) s.stopOpacity = parent.stopOpacity;
            else if (ParseOpacity(value, number)) s.stopOpacity = number;
            break;
        case kPropFillRule:
            if (inherit) s.fillEvenOdd = parent.fillEvenOdd;
            else if (value == "evenodd" || value == "nonzero") s.fillEvenOdd = value == "evenodd";
            break;
        case kPropClipRule:
            if (inherit) s.clipEvenOdd = parent.clipEvenOdd;
            else if (value == "evenodd" || value == "nonzero") s.clipEvenOdd = value == "evenodd";
            break;
        case kPropStrokeWidth:
            if (inherit) s.strokeWidth = parent.strokeWidth;
            else if (Length(value, s.fontSize, kAxisOther, number) && number >= 0.0) s.strokeWidth = number;
            break;
        case kPropStrokeLinecap:
            if (inherit) s.cap = parent.cap;
            else if (value == "butt") s.cap = 0;
            else if (value == "round") s.cap = 1;
            else if (value == "square") s.cap = 2;
            break;
        case kPropStrokeLinejoin:
            if (inherit) s.join = parent.join;
            else if (value == "miter" || value == "miter-clip" || value == "arcs") s.join = 0;
            else if (value == "round") s.join = 1;
            else if (value == "bevel") s.join = 2;
            break;
        case kPropStrokeMiterlimit:
            if (inherit) s.miterLimit = parent.miterLimit;
            else if (ParseNumber(value, number) && number >= 1.0) s.miterLimit = number;
            break;
        case kPropStrokeDasharray:
            if (inherit) s.dashArray = parent.dashArray;
            else if (value == "none") s.dashArray = std::string_view();
            else if (ValidDashArray(value, s.fontSize)) s.dashArray = value;
            break;
        case kPropStrokeDashoffset:
            if (inherit) s.dashOffset = parent.dashOffset;
            else if (Length(value, s.fontSize, kAxisOther, number)) s.dashOffset = number;
            break;
        case kPropClipPath: {
            std::string_view id, rest;
            if (inherit) s.clip = parent.clip;
            else if (value == "none") s.clip = -1;
            else if (ParseUrl(value, id, rest)) {
                const int target = Lookup(id);
                s.clip = target >= 0 && TagAt(target) == kTagClipPath ? target : -1;
            }
            break;
        }
        case kPropDisplay:
            if (inherit) s.displayNone = parent.displayNone;
            else if (value == "none") s.displayNone = true;
            else if (IsDisplayKeyword(value)) s.displayNone = false;
            break;
        case kPropVisibility:
            if (inherit) s.hidden = parent.hidden;
            else if (value == "visible") s.hidden = false;
            else if (value == "hidden" || value == "collapse") s.hidden = true;
            break;
        case kPropColor: {
            double alpha;
            uint32_t rgb;
            if (inherit || EqualsIgnoreCase(value, "currentColor")) s.color = parent.color;
            else if (ParseColor(value, rgb, alpha)) s.color = rgb;
            break;
        }
        case kPropStopColor: {
            double alpha;
            uint32_t rgb;
            if (inherit) {
                s.stopColor = parent.stopColor;
                s.stopAlpha = parent.stopAlpha;
                s.stopCurrent = parent.stopCurrent;
            } else if (EqualsIgnoreCase(value, "currentColor")) {
                s.stopAlpha = 1.0;
                s.stopCurrent = true;
            } else if (ParseColor(value, rgb, alpha)) {
                s.stopColor = rgb;
                s.stopAlpha = alpha;
                s.stopCurrent = false;
            }
            break;
        }
        case kPropFontSize:
            if (inherit) s.fontSize = parent.fontSize;
            else ParseFontSize(value, parent.fontSize, s.fontSize);
            break;
        case kPropFontWeight:
            if (inherit) s.bold = parent.bold;
            else if (value == "bold" || value == "bolder") s.bold = true;
            else if (value == "normal" || value == "lighter") s.bold = false;
            else if (ParseNumber(value, number) && number >= 1.0 && number <= 1000.0) s.bold = number >= 600.0;
            break;
        case kPropTextAnchor:
            if (inherit) s.anchor = parent.anchor;
            else if (value == "start") s.anchor = kAnchorStart;
            else if (value == "middle") s.anchor = kAnchorMiddle;
            else if (value == "end") s.anchor = kAnchorEnd;
            break;
        case kPropWhiteSpace:
            if (inherit) s.preserveSpace = parent.preserveSpace;
            else if (value == "pre" || value == "pre-wrap" || value == "break-spaces" || value == "pre-line") s.preserveSpace = true;
            else if (value == "normal" || value == "nowrap") s.preserveSpace = false;
            break;
        case kPropOverflow:
            if (inherit) s.overflowVisible = parent.overflowVisible;
            else if (value == "visible" || value == "auto") s.overflowVisible = true;
            else if (value == "hidden" || value == "scroll" || value == "clip") s.overflowVisible = false;
            break;
        case kPropNone:
            break;
        }
    }

    // A dash list: non-negative lengths or percentages separated by commas or white space.
    bool ValidDashArray(std::string_view value, double fontSize) const {
        const char* at = value.data();
        const char* const end = at + value.size();
        size_t count = 0;
        while (true) {
            SkipCommaSpace(at, end);
            if (at == end) break;
            const char* const start = at;
            while (at < end && !IsSpace(*at) && *at != ',') at++;
            double length;
            if (!Length(std::string_view(start, static_cast<size_t>(at - start)), fontSize, kAxisOther, length) || length < 0.0) return false;
            count++;
        }
        return count > 0;
    }

    // A display value other than none: the element is displayed.
    static bool IsDisplayKeyword(std::string_view value) {
        static constexpr char kKeywords[] =
            "inline block contents flow flow-root list-item run-in table flex grid ruby inline-block inline-table "
            "inline-flex inline-grid inline-list-item table-row-group table-header-group table-footer-group table-row "
            "table-cell table-column-group table-column table-caption ruby-base ruby-text ruby-base-container "
            "ruby-text-container math";
        static constexpr std::array<Word, 29> kWords = MakeWords<29>(kKeywords);
        static_assert(kWords[28].size == 4, "29 keywords");
        return FindWord(kKeywords, kWords.data(), kWords.size(), value) != 0;
    }

    static bool ParseOpacity(std::string_view value, double& out) {
        if (!value.empty() && value.back() == '%') {
            if (!ParseNumber(value.substr(0, value.size() - 1), out)) return false;
            out /= 100.0;
        } else if (!ParseNumber(value, out)) {
            return false;
        }
        out = std::clamp(out, 0.0, 1.0);
        return true;
    }

    bool ParseFontSize(std::string_view value, double parentSize, double& out) const {
        // The absolute size keywords, smallest first, and their sizes in px.
        static constexpr char kKeywords[] = "xx-small x-small small medium large x-large xx-large xxx-large";
        static constexpr std::array<Word, 8> kWords = MakeWords<8>(kKeywords);
        static constexpr uint8_t kSizes[] = { 9, 10, 13, 16, 18, 24, 32, 48 };
        if (const size_t keyword = FindWord(kKeywords, kWords.data(), kWords.size(), value)) {
            out = kSizes[keyword - 1];
            return true;
        }
        if (value == "larger") {
            out = parentSize * 1.2;
            return true;
        }
        if (value == "smaller") {
            out = parentSize / 1.2;
            return true;
        }
        double size;
        if (!value.empty() && value.back() == '%') {
            if (!ParseNumber(value.substr(0, value.size() - 1), size)) return false;
            size = parentSize * size / 100.0;
        } else if (!Length(value, parentSize, kAxisOther, size)) {
            return false;
        }
        if (!(size >= 0.0) || size > 1e6) return false;
        out = size;
        return true;
    }

    // none, a color, currentColor, or url(#id) of a gradient or pattern with an optional fallback.
    bool ParsePaint(std::string_view value, Paint& out) const {
        std::string_view id, rest;
        if (ParseUrl(value, id, rest)) {
            const int target = Lookup(id);
            if (target >= 0) {
                const Tag tag = TagAt(target);
                if (tag == kTagLinearGradient || tag == kTagRadialGradient || tag == kTagPattern) {
                    out = { kPaintServer, 0, 1.0, target };
                    return true;
                }
            }
            if (rest.empty()) {
                out = Paint();
                return true;
            }
            value = rest;
        }
        if (value == "none") {
            out = Paint();
            return true;
        }
        if (EqualsIgnoreCase(value, "currentColor")) {
            out = { kPaintCurrent, 0, 1.0, -1 };
            return true;
        }
        uint32_t rgb;
        double alpha;
        if (!ParseColor(value, rgb, alpha)) return false;
        out = { kPaintColor, rgb, alpha, -1 };
        return true;
    }

    // Applies the declarations of the matched rules, in cascade order, of one importance.
    void ApplyRules(Style& s, const Style& parent, bool important) const {
        for (uint32_t index : matched_) {
            const Selector& selector = selectors_[index];
            for (uint32_t at = selector.begin; at < selector.end; at++) {
                const SheetDeclaration& declaration = declarations_[at];
                if (!Charge(declaration.value.size() + 32)) return;   // about the cost of applying it
                if (declaration.important == important) Apply(s, parent, declaration.property, declaration.value);
            }
        }
    }

    // The cascade: inherited values, presentation attributes, style sheet rules by specificity
    // and order, the style attribute, then !important rules and declarations.
    void ComputeStyle(int index, const Style& parent, Style& s) const {
        s = parent;
        s.opacity = 1.0;
        s.stopOpacity = 1.0;
        s.stopAlpha = 1.0;
        s.stopColor = 0;
        s.stopCurrent = false;
        s.displayNone = false;
        s.overflowVisible = false;
        s.clip = -1;
        if (failed_) return;
        const Node& node = doc_.nodes[static_cast<size_t>(index)];
        std::string_view styleAttribute, id, classes;
        bool hasStyle = false;
        if (!Charge(8)) return;
        for (uint32_t at = node.firstAttribute; at < node.firstAttribute + node.attributeCount; at++) {
            const Attribute& attribute = doc_.attributes[at];
            if (!Charge(attribute.value.size() + 8)) return;
            if (attribute.name == "style") {
                styleAttribute = attribute.value;
                hasStyle = true;
            } else if (attribute.name == "xml:space") {
                const std::string_view space = Trim(attribute.value);
                if (space == "preserve" || space == "default") s.preserveSpace = space == "preserve";
            } else if (attribute.name == "id") {
                id = attribute.value;
            } else if (attribute.name == "class") {
                classes = attribute.value;
            } else {
                const Property property = PropertyOf(attribute.name);
                if (property != kPropNone) Apply(s, parent, property, attribute.value);
            }
        }
        matched_.clear();
        if (!selectors_.empty()) {
            const std::string_view local = LocalName(node.name);
            MatchChain(universal_, local, id, classes);
            if (keyKinds_ & kTypeKeys) Match(local, local, id, classes);
            if (!id.empty() && (keyKinds_ & kIdKeys)) {
                key_.assign(1, '#').append(id);
                Match(key_, local, id, classes);
            }
            for (size_t at = 0; (keyKinds_ & kClassKeys) && at < classes.size() && !failed_;) {
                while (at < classes.size() && IsSpace(classes[at])) at++;
                size_t end = at;
                while (end < classes.size() && !IsSpace(classes[end])) end++;
                if (end > at) {
                    key_.assign(1, '.').append(classes.substr(at, end - at));
                    Match(key_, local, id, classes);
                }
                at = end;
            }
            if (failed_) return;
            // Cascade order: specificity, then document order.
            if (matched_.size() > 1) {
                std::sort(matched_.begin(), matched_.end(), [this](uint32_t a, uint32_t b) {
                    return selectors_[a].specificity != selectors_[b].specificity ? selectors_[a].specificity < selectors_[b].specificity : a < b;
                });
            }
            matched_.erase(std::unique(matched_.begin(), matched_.end()), matched_.end());
            ApplyRules(s, parent, false);
        }
        if (hasStyle && styleAttribute.find("/*") != std::string_view::npos) {
            // CSS comments in the style attribute become spaces, as in a style sheet: once per
            // attribute, kept, since computed values (a dash list) point into it.
            const auto found = blankedIndex_.emplace(styleAttribute, static_cast<int>(blanked_.size()));
            if (found.second) {
                blanked_.emplace_back(styleAttribute);
                BlankComments(blanked_.back());
            }
            styleAttribute = blanked_[static_cast<size_t>(found.first->second)];
        }
        if (hasStyle) {
            ForEachDeclaration(styleAttribute, [&](Property property, std::string_view value, bool isImportant) {
                if (!isImportant) Apply(s, parent, property, value);
            });
        }
        if (importantRules_) ApplyRules(s, parent, true);
        if (hasStyle && styleAttribute.find('!') != std::string_view::npos) {
            ForEachDeclaration(styleAttribute, [&](Property property, std::string_view value, bool isImportant) {
                if (isImportant) Apply(s, parent, property, value);
            });
        }
        if (work_ > workLimit_) failed_ = true;
    }

    // ----- Rendering the tree

    static bool IsRendered(Tag tag) {
        return tag == kTagSvg || tag == kTagG || tag == kTagA || tag == kTagSwitch || tag == kTagUse ||
               tag == kTagText || tag == kTagImage || IsShape(tag);
    }

    void Render(int index, const Style& parent, const Matrix& parentCtm, const Context& context, int depth) {
        if (failed_) return;
        const Tag tag = TagAt(index);
        if (!IsRendered(tag) && !(tag == kTagSymbol && context.use >= 0)) return;   // a symbol only as a use's instance
        if (context.mode == kModeClip && tag == kTagImage) return;
        if (depth > kMaxRenderDepth || ++instantiated_ > budget_ || work_ > workLimit_) {
            failed_ = true;
            return;
        }
        Style s;
        ComputeStyle(index, parent, s);
        if (s.displayNone) return;
        Matrix ctm = parentCtm;
        std::string_view text;
        if (index != doc_.root && Attr(index, "transform", text)) {
            Matrix transform;
            if (ParseTransform(text, transform)) ctm = Multiply(ctm, transform);
        }
        Matrix viewport;   // the space of a nested viewport, where its clip rectangle lies
        double viewportSize[2] = { 0.0, 0.0 };
        bool clipViewport = false;
        if (tag == kTagUse) {
            ctm = Multiply(ctm, Translation(LengthAttr(index, "x", kAxisX, s.fontSize, 0.0),
                                            LengthAttr(index, "y", kAxisY, s.fontSize, 0.0)));
        } else if ((tag == kTagSvg || tag == kTagSymbol) && index != doc_.root) {
            // A viewport: the use instancing it sets its width and height. It clips what
            // overflows it (overflow: hidden) unless overflow is visible or auto.
            ctm = Multiply(ctm, Translation(LengthAttr(index, "x", kAxisX, s.fontSize, 0.0),
                                            LengthAttr(index, "y", kAxisY, s.fontSize, 0.0)));
            double width = LengthAttr(index, "width", kAxisX, s.fontSize, viewportWidth_);
            double height = LengthAttr(index, "height", kAxisY, s.fontSize, viewportHeight_);
            if (context.use >= 0) {
                width = LengthAttr(context.use, "width", kAxisX, s.fontSize, width);
                height = LengthAttr(context.use, "height", kAxisY, s.fontSize, height);
            }
            if (!(width > 0.0 && height > 0.0)) return;   // a viewport without area shows nothing
            viewport = ctm;
            viewportSize[0] = width;
            viewportSize[1] = height;
            clipViewport = !s.overflowVisible && context.mode == kModePaint;
            double box[4];
            if (Attr(index, "viewBox", text) && ParseViewBox(text, box)) {
                if (box[2] <= 0.0 || box[3] <= 0.0) return;
                AspectRatio aspect;
                if (Attr(index, "preserveAspectRatio", text)) ParseAspectRatio(text, aspect);
                ctm = Multiply(ctm, ViewBoxTransform(box, width, height, aspect));
            }
        }
        if (!IsUsable(ctm)) return;
        Context inner = context;
        inner.use = -1;
        if (context.mode == kModePaint) {
            inner.opacity *= s.opacity;
            if (inner.opacity <= 0.0) return;
        }
        active_[static_cast<size_t>(index)]++;
        // A viewport clips only content that reaches outside it.
        if (clipViewport) clipViewport = Overflows(index, s, ctm, inner, depth, viewport, viewportSize[0], viewportSize[1]);
        if (context.mode == kModePaint && (s.clip >= 0 || clipViewport)) {
            const size_t mark = out_.size();
            const size_t paints = paintCount_;
            // A clip-path and the viewport clip share one q.
            const bool clipped = s.clip >= 0 ? BeginClip(s.clip, index, s, ctm, inner, depth) : BeginViewportClip();
            if (clipped) {
                if (clipViewport) ClipToViewport(viewport, viewportSize[0], viewportSize[1]);
                RenderContent(index, s, ctm, inner, depth);
                EndClip();
                if (paintCount_ == paints && !failed_) out_.resize(mark);   // nothing drawn: drop the clip too
            }
        } else {
            RenderContent(index, s, ctm, inner, depth);
        }
        active_[static_cast<size_t>(index)]--;
    }

    void RenderContent(int index, const Style& s, const Matrix& ctm, const Context& context, int depth) {
        const Tag tag = TagAt(index);
        switch (tag) {
        case kTagSvg:
        case kTagSymbol:
        case kTagG:
        case kTagA:
            for (int child = doc_.nodes[static_cast<size_t>(index)].firstChild; child >= 0 && !failed_;
                 child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                Render(child, s, ctm, context, depth + 1);
            }
            break;
        case kTagSwitch:
            for (int child = doc_.nodes[static_cast<size_t>(index)].firstChild; child >= 0;
                 child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                if (IsRendered(TagAt(child))) {
                    Render(child, s, ctm, context, depth + 1);
                    break;
                }
            }
            break;
        case kTagUse: {
            const int target = Reference(Href(index));
            if (target < 0 || active_[static_cast<size_t>(target)] != 0) return;   // missing, or a cycle
            if (context.useDepth >= kMaxUseDepth) {
                failed_ = true;
                return;
            }
            Context instance = context;
            instance.useDepth++;
            instance.use = index;
            Render(target, s, ctm, instance, depth + 1);
            break;
        }
        case kTagText:
            PaintText(index, s, ctm, context, depth);
            break;
        case kTagImage:
            PaintImage(index, s, ctm, context);
            break;
        default:
            if (IsShape(tag)) PaintShape(index, s, ctm, context);
            break;
        }
    }

    // ----- Graphics state

    // The bytes the form has used: its content stream and the resource dictionaries (with the
    // copies kept to deduplicate them), which share kMaxContentBytes.
    size_t Used() const { return out_.size() + resourceBytes_; }

    // The bytes the form has left under its limit: what a path may take.
    size_t Remaining() const {
        return Used() < kMaxContentBytes ? kMaxContentBytes - Used() : 0;
    }

    // Charges a new resource of `bytes` to the form's budget; false, failing, past it.
    bool ChargeResource(size_t bytes) {
        resourceBytes_ += bytes;
        if (Used() <= kMaxContentBytes) return true;
        failed_ = true;
        return false;
    }

    bool Push() {
        if (states_.size() > kMaxSaveDepth) {
            failed_ = true;
            return false;
        }
        states_.push_back(states_.back());
        out_ += "q\n";
        return true;
    }

    void Pop() {
        states_.pop_back();
        out_ += "Q\n";
    }

    void SetFillColor(uint32_t rgb) {
        if (states_.back().fill == rgb) return;
        states_.back().fill = rgb;
        AppendColor(out_, rgb);
        out_ += " rg\n";
    }

    void SetStrokeColor(uint32_t rgb) {
        if (states_.back().stroke == rgb) return;
        states_.back().stroke = rgb;
        AppendColor(out_, rgb);
        out_ += " RG\n";
    }

    // Fill and stroke alpha through a shared ExtGState; a negative value keeps the current one.
    void SetAlpha(double fill, double stroke) {
        GraphicsState& state = states_.back();
        const int fillKey = fill < 0.0 ? state.fillAlpha : Milli(fill);
        const int strokeKey = stroke < 0.0 ? state.strokeAlpha : Milli(stroke);
        if (fillKey == state.fillAlpha && strokeKey == state.strokeAlpha) return;
        state.fillAlpha = fillKey;
        state.strokeAlpha = strokeKey;
        const uint32_t key = static_cast<uint32_t>(fillKey) * 1001u + static_cast<uint32_t>(strokeKey);
        auto found = extGStates_.find(key);
        if (found == extGStates_.end()) {
            if (!ChargeResource(64)) return;   // the entry and its key
            found = extGStates_.emplace(key, static_cast<int>(extGStates_.size()) + 1).first;
            form_.extGStates += "/GS";
            form_.extGStates += std::to_string(found->second);
            form_.extGStates += " << /ca ";
            AppendMilli(form_.extGStates, fillKey);
            form_.extGStates += " /CA ";
            AppendMilli(form_.extGStates, strokeKey);
            form_.extGStates += " >>\n";
        }
        out_ += "/GS";
        out_ += std::to_string(found->second);
        out_ += " gs\n";
    }

    // `width` must Fit: callers drop what they draw otherwise.
    void SetLineWidth(double width) {
        const int64_t key = static_cast<int64_t>(width * 10000.0 + 0.5);
        if (states_.back().width == key) return;
        states_.back().width = key;
        AppendFixed4(out_, width);
        out_ += " w\n";
    }

    // The stroke state of `s` in a space where its lengths scale by `scale`; false when the
    // dash list passes its limit.
    bool SetStrokeState(const Style& s, double scale) {
        GraphicsState& state = states_.back();
        SetLineWidth(s.strokeWidth * scale);
        if (state.cap != s.cap) {
            state.cap = s.cap;
            out_ += static_cast<char>('0' + s.cap);
            out_ += " J\n";
        }
        if (state.join != s.join) {
            state.join = s.join;
            out_ += static_cast<char>('0' + s.join);
            out_ += " j\n";
        }
        const double miter = std::min(s.miterLimit, kMaxReal);
        const int64_t miterKey = static_cast<int64_t>(miter * 10000.0 + 0.5);
        if (state.miter != miterKey) {
            state.miter = miterKey;
            AppendFixed4(out_, miter);
            out_ += " M\n";
        }
        if (!BuildDash(s, scale, dash_)) return false;
        if (state.dash != dash_) {
            state.dash = dash_;
            out_ += dash_;
            out_ += " d\n";
        }
        return true;
    }

    // "[on off ...] phase": an odd list twice; solid for none, a negative entry or a zero sum.
    bool BuildDash(const Style& s, double scale, std::string& out) {
        out = "[] 0";
        if (s.dashArray.empty()) return true;
        double values[kMaxDashes];
        size_t count = 0;
        double sum = 0.0;
        const char* at = s.dashArray.data();
        const char* end = at + s.dashArray.size();
        while (true) {
            SkipCommaSpace(at, end);
            if (at == end) break;
            const char* start = at;
            while (at < end && !IsSpace(*at) && *at != ',') at++;
            double value;
            if (!Length(std::string_view(start, static_cast<size_t>(at - start)), s.fontSize, kAxisOther, value) || value < 0.0) {
                return true;
            }
            if (count == kMaxDashes) {
                failed_ = true;
                return false;
            }
            values[count++] = value * scale;
            sum += value * scale;
        }
        if (count == 0 || !(sum >= 0.0001) || !Fits(sum)) return true;
        const double phase = s.dashOffset * scale;
        if (!Fits(phase)) return true;
        out = "[";
        for (int repeat = 0; repeat < (count % 2 == 1 ? 2 : 1); repeat++) {
            for (size_t index = 0; index < count; index++) {
                if (out.size() > 1) out += ' ';
                AppendFixed4(out, values[index]);
            }
        }
        out += "] ";
        AppendFixed4(out, phase);
        return true;
    }

    // ----- Paint servers

    // The gradient `index` with its href chain resolved: attributes and stops from the first
    // element in the chain that has them.
    void ResolveGradient(int index) {
        if (index == gradientNode_) return;   // the gradient resolved last
        gradientNode_ = -1;
        Gradient& g = gradient_;
        int chain[kMaxGradientHops + 1];
        int count = 0;
        for (int node = index; node >= 0;) {
            const Tag tag = TagAt(node);
            if (tag != kTagLinearGradient && tag != kTagRadialGradient) break;
            bool seen = false;
            for (int at = 0; at < count; at++) seen = seen || chain[at] == node;
            if (seen) break;
            if (count > kMaxGradientHops) {
                failed_ = true;
                return;
            }
            chain[count++] = node;
            node = Reference(Href(node));
        }
        g.radial = TagAt(index) == kTagRadialGradient;
        g.userSpace = false;
        g.transform = Matrix();
        g.stops.clear();
        std::string_view text;
        for (int at = 0; at < count; at++) {
            if (Attr(chain[at], "gradientUnits", text)) {
                g.userSpace = Trim(text) == "userSpaceOnUse";
                break;
            }
        }
        for (int at = 0; at < count; at++) {
            if (Attr(chain[at], "gradientTransform", text)) {
                ParseTransform(text, g.transform);
                break;
            }
        }
        double* values[4] = { &g.x1, &g.y1, &g.x2, &g.y2 };
        const double defaults[4] = { 0.0, 0.0, g.userSpace ? viewportWidth_ : 1.0, 0.0 };
        for (int coordinate = 0; coordinate < 4; coordinate++) {
            *values[coordinate] = defaults[coordinate];
            const char name[2] = { coordinate % 2 == 0 ? 'x' : 'y', coordinate < 2 ? '1' : '2' };
            for (int at = 0; at < count; at++) {
                if (TagAt(chain[at]) != kTagLinearGradient || !Attr(chain[at], std::string_view(name, 2), text)) continue;
                const Axis axis = coordinate % 2 == 0 ? kAxisX : kAxisY;
                double value;
                text = Trim(text);
                if (!g.userSpace && !text.empty() && text.back() == '%') {
                    if (ParseNumber(text.substr(0, text.size() - 1), value)) *values[coordinate] = value / 100.0;
                } else if (g.userSpace ? Length(text, 16.0, axis, value) : ParseNumber(text, value)) {
                    *values[coordinate] = value;
                }
                break;
            }
        }
        for (int at = 0; at < count; at++) {
            const Node& node = doc_.nodes[static_cast<size_t>(chain[at])];
            double previous = 0.0;
            // Stops inherit from their gradient and its ancestors, not from the painted element.
            Style parent;
            bool styled = false;
            for (int child = node.firstChild; child >= 0; child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                if (TagAt(child) != kTagStop) continue;
                if (g.stops.size() == kMaxStops) {
                    failed_ = true;
                    return;
                }
                if (!styled) {
                    ElementStyle(chain[at], parent);
                    styled = true;
                }
                double offset = 0.0;
                if (Attr(child, "offset", text)) {
                    text = Trim(text);
                    if (!text.empty() && text.back() == '%') {
                        if (ParseNumber(text.substr(0, text.size() - 1), offset)) offset /= 100.0;
                    } else {
                        ParseNumber(text, offset);
                    }
                }
                offset = std::max(std::clamp(offset, 0.0, 1.0), previous);
                previous = offset;
                Style stop;
                ComputeStyle(child, parent, stop);
                g.stops.push_back({ offset, stop.stopCurrent ? stop.color : stop.stopColor, stop.stopOpacity * stop.stopAlpha });
            }
            if (!g.stops.empty()) break;
        }
        if (!failed_) gradientNode_ = index;
    }

    // The computed style of an element from its ancestors in the document (what a gradient's
    // stops and a pattern's content inherit), not from where it is used.
    void ElementStyle(int index, Style& s) const {
        int chain[kMaxDepth + 1];
        int count = 0;
        for (int node = index; node >= 0 && count <= static_cast<int>(kMaxDepth); node = doc_.nodes[static_cast<size_t>(node)].parent) {
            chain[count++] = node;
        }
        s = Style();
        while (count > 0 && !failed_) {
            const Style parent = s;
            ComputeStyle(chain[--count], parent, s);
        }
    }

    // The fill or stroke of `s` as drawn, its alpha without the opacity properties.
    void ResolvePaint(const Paint& paint, const Style& s, bool stroke, ResolvedPaint& out) {
        out = ResolvedPaint();
        switch (paint.kind) {
        case kPaintNone:
            return;
        case kPaintColor:
            out = { ResolvedPaint::kSolid, paint.rgb, paint.alpha, -1 };
            return;
        case kPaintCurrent:
            out = { ResolvedPaint::kSolid, s.color, 1.0, -1 };
            return;
        case kPaintServer:
            break;
        }
        const Tag tag = TagAt(paint.server);
        if (tag == kTagPattern) {
            // A pattern paints the color of its first rect (a hatch shows its face color).
            for (int child = doc_.nodes[static_cast<size_t>(paint.server)].firstChild; child >= 0;
                 child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                if (TagAt(child) != kTagRect) continue;
                Style rect;
                ElementStyle(child, rect);
                if (rect.fill.kind == kPaintColor) out = { ResolvedPaint::kSolid, rect.fill.rgb, rect.fill.alpha * rect.fillOpacity, -1 };
                else if (rect.fill.kind == kPaintCurrent) out = { ResolvedPaint::kSolid, rect.color, rect.fillOpacity, -1 };
                break;
            }
            return;
        }
        ResolveGradient(paint.server);
        const std::vector<GradientStop>& stops = gradient_.stops;
        if (stops.empty() || failed_) return;
        double alpha = 0.0;
        for (const GradientStop& stop : stops) alpha += stop.alpha;
        alpha /= static_cast<double>(stops.size());
        if (stops.size() == 1) {
            out = { ResolvedPaint::kSolid, stops[0].rgb, stops[0].alpha, -1 };
        } else if (gradient_.radial) {
            // A radial gradient paints the average of its stops.
            double sums[3] = { 0.0, 0.0, 0.0 };
            for (const GradientStop& stop : stops) {
                for (int channel = 0; channel < 3; channel++) sums[channel] += (stop.rgb >> (16 - 8 * channel)) & 0xFFu;
            }
            const double n = static_cast<double>(stops.size());
            out = { ResolvedPaint::kSolid, PackRgb(sums[0] / n, sums[1] / n, sums[2] / n), alpha, -1 };
        } else if (stroke) {
            // A stroke paints the gradient's color at its middle.
            out = { ResolvedPaint::kSolid, ColorAt(stops, 0.5), alpha, -1 };
        } else {
            out = { ResolvedPaint::kLinear, 0, alpha, paint.server };
        }
    }

    static uint32_t ColorAt(const std::vector<GradientStop>& stops, double t) {
        if (t <= stops.front().offset) return stops.front().rgb;
        for (size_t index = 1; index < stops.size(); index++) {
            const GradientStop& a = stops[index - 1];
            const GradientStop& b = stops[index];
            if (t > b.offset) continue;
            const double span = b.offset - a.offset;
            const double u = span > 0.0 ? (t - a.offset) / span : 1.0;
            double channels[3];
            for (int channel = 0; channel < 3; channel++) {
                const int shift = 16 - 8 * channel;
                channels[channel] = ((a.rgb >> shift) & 0xFFu) * (1.0 - u) + ((b.rgb >> shift) & 0xFFu) * u;
            }
            return PackRgb(channels[0], channels[1], channels[2]);
        }
        return stops.back().rgb;
    }

    // The /Shading entry of the resolved linear gradient: an axial shading over its stops,
    // padded to 0 and 1, extended at both ends. Returns its number, deduplicated.
    int ShadingFor(const Gradient& g) {
        std::vector<GradientStop> stops;
        stops.reserve(g.stops.size() + 2);
        if (g.stops.front().offset > 0.0) stops.push_back({ 0.0, g.stops.front().rgb, 1.0 });
        stops.insert(stops.end(), g.stops.begin(), g.stops.end());
        if (stops.back().offset < 1.0) stops.push_back({ 1.0, stops.back().rgb, 1.0 });
        std::string dict = "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [";
        AppendFixed4(dict, g.x1);
        dict += ' ';
        AppendFixed4(dict, g.y1);
        dict += ' ';
        AppendFixed4(dict, g.x2);
        dict += ' ';
        AppendFixed4(dict, g.y2);
        dict += "] /Extend [true true] /Function ";
        auto exponential = [](std::string& out, uint32_t from, uint32_t to) {
            out += "<< /FunctionType 2 /Domain [0 1] /C0 [";
            AppendColor(out, from);
            out += "] /C1 [";
            AppendColor(out, to);
            out += "] /N 1 >>";
        };
        std::vector<size_t> segments;   // stops that start a segment of nonzero width
        for (size_t index = 0; index + 1 < stops.size(); index++) {
            if (stops[index + 1].offset > stops[index].offset) segments.push_back(index);
        }
        if (segments.size() <= 1) {
            const size_t first = segments.empty() ? 0 : segments[0];
            exponential(dict, stops[first].rgb, stops[segments.empty() ? stops.size() - 1 : first + 1].rgb);
        } else {
            dict += "<< /FunctionType 3 /Domain [0 1] /Functions [";
            for (size_t segment : segments) {
                exponential(dict, stops[segment].rgb, stops[segment + 1].rgb);
                dict += ' ';
            }
            dict += "] /Bounds [";
            for (size_t index = 1; index < segments.size(); index++) {
                if (index > 1) dict += ' ';
                AppendFixed4(dict, stops[segments[index]].offset);
            }
            dict += "] /Encode [";
            for (size_t index = 0; index < segments.size(); index++) dict += index == 0 ? "0 1" : " 0 1";
            dict += "] >>";
        }
        dict += " >>";
        auto found = shadings_.find(dict);
        if (found != shadings_.end()) return found->second;
        // The dictionary is kept twice: in the resources and as the key that deduplicates it.
        if (!ChargeResource(2 * dict.size() + 64)) return -1;
        const int number = static_cast<int>(shadings_.size()) + 1;
        form_.shadings += "/Sh";
        form_.shadings += std::to_string(number);
        form_.shadings += ' ';
        form_.shadings += dict;
        form_.shadings += '\n';
        shadings_.emplace(std::move(dict), number);
        return number;
    }

    // Fills geom_ with gradient_ under `space` (form space, or the cm space of a local path).
    bool PaintGradient(const Box& box, const Matrix& space, bool evenOdd, double alpha) {
        const Gradient& g = gradient_;
        Matrix matrix = space;
        if (!g.userSpace) {
            const double width = box.x1 - box.x0;
            const double height = box.y1 - box.y0;
            if (box.Empty() || width <= 0.0 || height <= 0.0) return false;
            matrix = Multiply(matrix, Matrix{ width, 0.0, 0.0, height, box.x0, box.y0 });
        }
        matrix = Multiply(matrix, g.transform);
        if (!IsUsable(matrix) || !MatrixFits(matrix)) return false;
        if (!Fits(g.x1) || !Fits(g.y1) || !Fits(g.x2) || !Fits(g.y2)) return false;
        if (g.x1 == g.x2 && g.y1 == g.y2) {
            // A gradient without length paints its last stop.
            SetAlpha(alpha, -1.0);
            SetFillColor(g.stops.back().rgb);
            out_ += geom_;
            out_ += evenOdd ? "f*\n" : "f\n";
            return true;
        }
        const int shading = ShadingFor(g);
        if (shading < 0) return false;
        if (!Push()) return false;
        out_ += geom_;
        out_ += evenOdd ? "W* n\n" : "W n\n";
        SetAlpha(alpha, -1.0);
        AppendMatrix(out_, matrix);
        out_ += "/Sh";
        out_ += std::to_string(shading);
        out_ += " sh\n";
        Pop();
        return true;
    }

    // ----- Shapes

    void BuildShape(int index, const Style& s, PathWriter& writer) {
        const Tag tag = TagAt(index);
        std::string_view text;
        const double fs = s.fontSize;
        switch (tag) {
        case kTagPath:
            if (Attr(index, "d", text) && !ParsePathData(text, writer, pathNumbers_)) failed_ = true;
            break;
        case kTagPolyline:
        case kTagPolygon:
            if (Attr(index, "points", text) && !ParsePoints(text, writer, tag == kTagPolygon, pathNumbers_)) failed_ = true;
            break;
        case kTagLine: {
            writer.MoveTo(LengthAttr(index, "x1", kAxisX, fs, 0.0), LengthAttr(index, "y1", kAxisY, fs, 0.0));
            writer.LineTo(LengthAttr(index, "x2", kAxisX, fs, 0.0), LengthAttr(index, "y2", kAxisY, fs, 0.0));
            break;
        }
        case kTagRect: {
            const double x = LengthAttr(index, "x", kAxisX, fs, 0.0);
            const double y = LengthAttr(index, "y", kAxisY, fs, 0.0);
            const double width = LengthAttr(index, "width", kAxisX, fs, 0.0);
            const double height = LengthAttr(index, "height", kAxisY, fs, 0.0);
            if (!(width > 0.0 && height > 0.0)) return;
            double rx = LengthAttr(index, "rx", kAxisX, fs, -1.0);
            double ry = LengthAttr(index, "ry", kAxisY, fs, -1.0);
            if (rx < 0.0) rx = ry;
            if (ry < 0.0) ry = rx;
            rx = std::min(std::max(rx, 0.0), width / 2.0);
            ry = std::min(std::max(ry, 0.0), height / 2.0);
            if (rx <= 0.0 || ry <= 0.0) {
                writer.Rect(x, y, width, height);
                return;
            }
            const double kx = 0.5522847498 * rx, ky = 0.5522847498 * ry;
            const double right = x + width, bottom = y + height;
            writer.MoveTo(x + rx, y);
            writer.LineTo(right - rx, y);
            writer.CubicTo(right - rx + kx, y, right, y + ry - ky, right, y + ry);
            writer.LineTo(right, bottom - ry);
            writer.CubicTo(right, bottom - ry + ky, right - rx + kx, bottom, right - rx, bottom);
            writer.LineTo(x + rx, bottom);
            writer.CubicTo(x + rx - kx, bottom, x, bottom - ry + ky, x, bottom - ry);
            writer.LineTo(x, y + ry);
            writer.CubicTo(x, y + ry - ky, x + rx - kx, y, x + rx, y);
            writer.Close();
            break;
        }
        case kTagCircle:
        case kTagEllipse: {
            const double cx = LengthAttr(index, "cx", kAxisX, fs, 0.0);
            const double cy = LengthAttr(index, "cy", kAxisY, fs, 0.0);
            double rx, ry;
            if (tag == kTagCircle) {
                rx = ry = LengthAttr(index, "r", kAxisOther, fs, 0.0);
            } else {
                rx = LengthAttr(index, "rx", kAxisX, fs, -1.0);
                ry = LengthAttr(index, "ry", kAxisY, fs, -1.0);
                if (rx < 0.0) rx = ry;
                if (ry < 0.0) ry = rx;
            }
            if (!(rx > 0.0 && ry > 0.0)) return;
            const double kx = 0.5522847498 * rx, ky = 0.5522847498 * ry;
            writer.MoveTo(cx + rx, cy);
            writer.CubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
            writer.CubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
            writer.CubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
            writer.CubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
            writer.Close();
            break;
        }
        default:
            break;
        }
    }

    void PaintShape(int index, const Style& s, const Matrix& ctm, const Context& context) {
        if (context.mode == kModeMeasure) {
            PathWriter writer(nullptr, ctm, false, kTransformedBox);
            BuildShape(index, s, writer);
            if (measureStrokes_ && s.stroke.kind != kPaintNone && s.strokeWidth > 0.0 && !writer.box.Empty()) {
                // A stroke reaches past the geometry by half its width (up to the miter limit, or
                // a square cap's corner) in user space; the pen's circle maps to an ellipse whose
                // extents along x and y come from the rows of the matrix.
                const double reach = 0.5 * s.strokeWidth * (s.join == 0 ? std::max(s.miterLimit, 1.5) : 1.5);
                const double reachX = reach * std::sqrt(ctm.a * ctm.a + ctm.c * ctm.c);
                const double reachY = reach * std::sqrt(ctm.b * ctm.b + ctm.d * ctm.d);
                writer.box.Add(writer.box.x0 - reachX, writer.box.y0 - reachY);
                writer.box.Add(writer.box.x1 + reachX, writer.box.y1 + reachY);
            }
            measureBox_.Add(writer.box);
            return;
        }
        if (s.hidden) return;
        if (context.mode == kModeClip) {
            const size_t mark = clipPath_.size();
            PathWriter writer(&clipPath_, ctm, false, kNoBox, Remaining());
            BuildShape(index, s, writer);
            if (writer.full) failed_ = true;
            if (writer.bad) clipPath_.resize(mark);
            else if (clipPath_.size() > mark && !s.clipEvenOdd) clipEvenOdd_ = false;
            return;
        }
        // The stroke first: a gradient stroke resolves to a color, the fill may keep gradient_.
        ResolvedPaint fill, stroke;
        if (s.strokeWidth > 0.0) ResolvePaint(s.stroke, s, true, stroke);
        if (TagAt(index) != kTagLine) ResolvePaint(s.fill, s, false, fill);
        if (failed_) return;
        fill.alpha *= s.fillOpacity * context.opacity;
        stroke.alpha *= s.strokeOpacity * context.opacity;
        if (fill.alpha < 0.0005) fill.kind = ResolvedPaint::kNone;
        if (stroke.alpha < 0.0005) stroke.kind = ResolvedPaint::kNone;
        const bool fills = fill.kind != ResolvedPaint::kNone;
        const bool strokes = stroke.kind != ResolvedPaint::kNone;
        if (!fills && !strokes) return;

        // Strokes under a transform that is not a similarity are drawn under cm, so that the pen
        // is transformed the way SVG transforms it.
        const bool local = strokes && !IsSimilarity(ctm);
        geom_.clear();
        PathWriter writer(&geom_, local ? Matrix() : ctm, local, fill.kind == ResolvedPaint::kLinear ? kLocalBox : kNoBox, Remaining());
        BuildShape(index, s, writer);
        if (writer.full) failed_ = true;
        if (writer.bad || geom_.empty() || failed_) return;
        if (local && !MatrixFits(ctm)) return;
        const double scale = local ? 1.0 : std::sqrt(std::fabs(Determinant(ctm)));
        if (strokes && !Fits(s.strokeWidth * scale)) return;   // like a coordinate past the limit
        if (local) {
            if (!Push()) return;
            AppendMatrix(out_, ctm);
        }
        const char* op = nullptr;
        bool painted = true;
        if (fills && fill.kind == ResolvedPaint::kLinear) {
            painted = PaintGradient(writer.box, local ? Matrix() : ctm, s.fillEvenOdd, fill.alpha);
            if (strokes) {
                op = "S\n";
                painted = true;
            }
        } else if (fills && strokes) {
            op = s.fillEvenOdd ? "B*\n" : "B\n";
        } else if (fills) {
            op = s.fillEvenOdd ? "f*\n" : "f\n";
        } else {
            op = "S\n";
        }
        if (op != nullptr) {
            const bool stroking = op[0] != 'f';
            const bool filling = op[0] != 'S';
            SetAlpha(filling ? fill.alpha : -1.0, stroking ? stroke.alpha : -1.0);
            if (filling) SetFillColor(fill.rgb);
            if (stroking) {
                SetStrokeColor(stroke.rgb);
                if (!SetStrokeState(s, scale)) return;
            }
            out_ += geom_;
            out_ += op;
        }
        if (local) Pop();
        if (painted) paintCount_++;
        if (Used() > kMaxContentBytes) failed_ = true;
    }

    // ----- Clipping

    // Opens a q scope clipped to `clip` (and the clipPaths that clip it), in the user space
    // of `element`. False when the clip is empty: the element is not drawn.
    bool BeginClip(int clip, int element, const Style& s, const Matrix& ctm, const Context& context, int depth) {
        if (clipDepth_ >= kMaxClipDepth) {
            failed_ = true;
            return false;
        }
        int chain[kMaxClipChain];
        int count = 0;
        bool boundingBox = false;
        std::string_view text;
        for (int node = clip; node >= 0;) {
            for (int at = 0; at < count; at++) {
                if (chain[at] == node) node = -1;
            }
            if (node < 0) break;
            if (count == kMaxClipChain) {
                failed_ = true;
                return false;
            }
            chain[count++] = node;
            if (Attr(node, "clipPathUnits", text) && Trim(text) == "objectBoundingBox") boundingBox = true;
            Style clipStyle;
            ComputeStyle(node, Style(), clipStyle);
            node = clipStyle.clip;
        }
        Box box;
        if (boundingBox) {
            const Box saved = measureBox_;
            measureBox_ = Box();
            RenderContent(element, s, Matrix(), Context{ kModeMeasure, 1.0, context.useDepth }, depth);
            box = measureBox_;
            measureBox_ = saved;
            if (failed_ || box.Empty() || box.x1 <= box.x0 || box.y1 <= box.y0) return false;
        }
        if (!Push()) return false;
        for (int at = 0; at < count; at++) {
            const int node = chain[at];
            Style clipStyle;
            ComputeStyle(node, Style(), clipStyle);
            Matrix matrix = ctm;
            if (Attr(node, "clipPathUnits", text) && Trim(text) == "objectBoundingBox") {
                matrix = Multiply(matrix, Matrix{ box.x1 - box.x0, 0.0, 0.0, box.y1 - box.y0, box.x0, box.y0 });
            }
            Matrix transform;
            if (Attr(node, "transform", text) && ParseTransform(text, transform)) matrix = Multiply(matrix, transform);
            clipPath_.clear();
            clipEvenOdd_ = true;
            if (IsUsable(matrix)) {
                for (int child = doc_.nodes[static_cast<size_t>(node)].firstChild; child >= 0 && !failed_;
                     child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
                    Render(child, clipStyle, matrix, Context{ kModeClip, 1.0, context.useDepth }, depth + 1);
                }
            }
            if (failed_ || clipPath_.empty()) {
                Pop();
                return false;
            }
            out_ += clipPath_;
            out_ += clipEvenOdd_ ? "W* n\n" : "W n\n";
        }
        clipDepth_++;
        return true;
    }

    // Whether the content of a viewport reaches outside it, (0, 0, width, height) in `space`:
    // its bounds, strokes included. A viewport not axis-aligned in the form always clips.
    bool Overflows(int index, const Style& s, const Matrix& ctm, const Context& context, int depth,
                   const Matrix& space, double width, double height) {
        if (space.b != 0.0 || space.c != 0.0) return true;
        const Box saved = measureBox_;
        const bool savedStrokes = measureStrokes_;
        measureBox_ = Box();
        measureStrokes_ = true;
        RenderContent(index, s, ctm, Context{ kModeMeasure, 1.0, context.useDepth }, depth);
        const Box box = measureBox_;
        measureBox_ = saved;
        measureStrokes_ = savedStrokes;
        if (box.Empty()) return false;
        const double ax = space.e, bx = space.a * width + space.e;
        const double ay = space.f, by = space.d * height + space.f;
        const double tolerance = 0.005;   // below what the content stream prints
        return box.x0 < std::min(ax, bx) - tolerance || box.x1 > std::max(ax, bx) + tolerance ||
               box.y0 < std::min(ay, by) - tolerance || box.y1 > std::max(ay, by) + tolerance;
    }

    // Opens a q scope for a viewport clip alone; false past the clip nesting limit.
    bool BeginViewportClip() {
        if (clipDepth_ >= kMaxClipDepth) {
            failed_ = true;
            return false;
        }
        if (!Push()) return false;
        clipDepth_++;
        return true;
    }

    // Clips the open q scope to a viewport, (0, 0, width, height) in `space`. A rectangle past
    // the content stream's limits is not written: the form's box clips there anyway.
    void ClipToViewport(const Matrix& space, double width, double height) {
        const size_t mark = out_.size();
        PathWriter writer(&out_, space, false, kNoBox, out_.size() + Remaining());
        writer.Rect(0.0, 0.0, width, height);
        if (writer.bad) out_.resize(mark);
        else out_ += "W n\n";
    }

    void EndClip() {
        clipDepth_--;
        Pop();
    }

    // ----- Text

    // Collects the runs of a text element and its tspans: white space handled, positions
    // (first x, y, dx, dy of each element) attached to the next character.
    void CollectText(int index, const Style& s, const Context& context, int depth) {
        if (depth > kMaxRenderDepth) {
            failed_ = true;
            return;
        }
        const double x = FirstLength(index, "x", kAxisX, s.fontSize);
        const double y = FirstLength(index, "y", kAxisY, s.fontSize);
        const double dx = FirstLength(index, "dx", kAxisX, s.fontSize);
        const double dy = FirstLength(index, "dy", kAxisY, s.fontSize);
        if (!std::isnan(x)) pendingX_ = x;
        if (!std::isnan(y)) pendingY_ = y;
        if (!std::isnan(dx)) pendingDx_ = (std::isnan(pendingDx_) ? 0.0 : pendingDx_) + dx;
        if (!std::isnan(dy)) pendingDy_ = (std::isnan(pendingDy_) ? 0.0 : pendingDy_) + dy;
        ResolvedPaint fill;
        if (context.mode == kModePaint) {
            ResolvePaint(s.fill, s, true, fill);
            fill.alpha *= s.fillOpacity * context.opacity;
            if (fill.alpha < 0.0005) fill.kind = ResolvedPaint::kNone;
        }
        for (int child = doc_.nodes[static_cast<size_t>(index)].firstChild; child >= 0 && !failed_;
             child = doc_.nodes[static_cast<size_t>(child)].nextSibling) {
            const Tag tag = TagAt(child);
            if (tag == kTagTextNode) {
                const std::string_view raw = doc_.nodes[static_cast<size_t>(child)].name;
                work_ += raw.size();
                std::string text;
                for (size_t at = 0; at < raw.size(); at++) {
                    const char ch = raw[at];
                    if (ch == '\r' && at + 1 < raw.size() && raw[at + 1] == '\n') continue;   // XML reads CR LF as LF
                    if (s.preserveSpace) {
                        text += IsSpace(ch) ? ' ' : ch;
                    } else if (IsSpace(ch)) {
                        // CSS white-space: normal: line breaks and tabs are spaces, runs collapse.
                        if (!lastSpace_) text += ' ';
                        lastSpace_ = true;
                    } else {
                        text += ch;
                        lastSpace_ = false;
                    }
                }
                if (s.preserveSpace && !text.empty()) lastSpace_ = text.back() == ' ';
                if (text.empty()) continue;
                textBytes_ += text.size();
                textElementBytes_ += text.size();
                if (textBytes_ > kMaxTextTotal || textElementBytes_ > kMaxTextPerElement) {
                    failed_ = true;
                    return;
                }
                TextRun run;
                run.text = std::move(text);
                run.absoluteX = pendingX_;
                run.absoluteY = pendingY_;
                run.dx = pendingDx_;
                run.dy = pendingDy_;
                pendingX_ = pendingY_ = pendingDx_ = pendingDy_ = NAN;
                run.fontSize = s.fontSize;
                run.bold = s.bold;
                run.hidden = s.hidden;
                run.preserveSpace = s.preserveSpace;
                run.anchor = s.anchor;
                run.fill = fill;
                runs_.push_back(std::move(run));
            } else if (tag == kTagTspan || tag == kTagTextPath || tag == kTagA) {
                if (++instantiated_ > budget_ || work_ > workLimit_) {
                    failed_ = true;
                    return;
                }
                Style childStyle;
                ComputeStyle(child, s, childStyle);
                if (childStyle.displayNone) continue;
                CollectText(child, childStyle, context, depth + 1);
            }
        }
    }

    void PaintText(int index, const Style& s, const Matrix& ctm, const Context& context, int depth) {
        runs_.clear();
        pendingX_ = pendingY_ = pendingDx_ = pendingDy_ = NAN;
        lastSpace_ = true;
        textElementBytes_ = 0;
        CollectText(index, s, context, depth);
        if (failed_ || runs_.empty()) return;
        // Trailing white space goes (default xml:space).
        if (!runs_.back().preserveSpace && !runs_.back().text.empty() && runs_.back().text.back() == ' ') runs_.back().text.pop_back();

        // Layout: each absolute x or y starts a text chunk, anchored as a whole.
        double penX = 0.0, penY = 0.0;
        size_t chunk = 0;
        auto anchorChunk = [&](size_t begin, size_t end) {
            if (begin >= end || runs_[begin].anchor == kAnchorStart) return;
            const double width = runs_[end - 1].x + runs_[end - 1].width - runs_[begin].x;
            const double shift = runs_[begin].anchor == kAnchorMiddle ? -width / 2.0 : -width;
            for (size_t at = begin; at < end; at++) runs_[at].x += shift;
        };
        for (size_t at = 0; at < runs_.size(); at++) {
            TextRun& run = runs_[at];
            const bool start = !std::isnan(run.absoluteX) || !std::isnan(run.absoluteY);
            if (start && at > chunk) {
                anchorChunk(chunk, at);
                chunk = at;
            }
            if (!std::isnan(run.absoluteX)) penX = run.absoluteX;
            if (!std::isnan(run.absoluteY)) penY = run.absoluteY;
            if (!std::isnan(run.dx)) penX += run.dx;
            if (!std::isnan(run.dy)) penY += run.dy;
            run.x = penX;
            run.y = penY;
            // Measured in the face drawn: faux bold is the regular face with a stroke.
            run.width = run.text.empty() || run.fontSize <= 0.0
                ? 0.0 : host_.TextWidth(run.text, run.fontSize, run.bold && host_.HasBoldFace());
            if (!std::isfinite(run.width)) run.width = 0.0;
            penX += run.width;
        }
        anchorChunk(chunk, runs_.size());

        if (context.mode != kModePaint) {
            // Clips and bounding boxes take the text's box.
            for (const TextRun& run : runs_) {
                if (run.text.empty() || run.fontSize <= 0.0 || (context.mode == kModeClip && run.hidden)) continue;
                const size_t mark = clipPath_.size();
                PathWriter writer(context.mode == kModeClip ? &clipPath_ : nullptr, ctm, false,
                                  context.mode == kModeMeasure ? kTransformedBox : kNoBox, Remaining());
                writer.Rect(run.x, run.y - 0.8 * run.fontSize, std::max(run.width, 0.0), run.fontSize);
                if (context.mode == kModeMeasure) measureBox_.Add(writer.box);
                else if (writer.bad) clipPath_.resize(mark);
                if (writer.full) failed_ = true;
            }
            return;
        }
        for (const TextRun& run : runs_) {
            if (run.hidden || run.text.empty() || run.fontSize <= 0.0 || run.fill.kind == ResolvedPaint::kNone) continue;
            const Matrix tm = Multiply(Multiply(ctm, Translation(run.x, run.y)), Matrix{ 1.0, 0.0, 0.0, -1.0, 0.0, 0.0 });
            const bool face = run.bold && host_.HasBoldFace();
            const bool faux = run.bold && !face;
            const double width = faux ? 0.03 * run.fontSize * std::sqrt(std::fabs(Determinant(tm))) : 0.0;
            if (!MatrixFits(tm) || !Fits(run.fontSize) || !Fits(width)) continue;
            SetAlpha(run.fill.alpha, faux ? run.fill.alpha : -1.0);
            SetFillColor(run.fill.rgb);
            if (faux) {
                SetStrokeColor(run.fill.rgb);
                SetLineWidth(width);
                states_.back().textRender = 2;
                out_ += "2 Tr\n";
            }
            out_ += face ? "BT\n/F2 " : "BT\n/F1 ";
            AppendNumber(out_, run.fontSize);
            out_ += " Tf\n";
            AppendFixed4(out_, tm.a);
            out_ += ' ';
            AppendFixed4(out_, tm.b);
            out_ += ' ';
            AppendFixed4(out_, tm.c);
            out_ += ' ';
            AppendFixed4(out_, tm.d);
            out_ += ' ';
            AppendNumber(out_, tm.e);
            out_ += ' ';
            AppendNumber(out_, tm.f);
            out_ += " Tm\n";
            host_.AppendText(out_, run.text, face);
            out_ += " Tj\nET\n";
            if (faux) {
                states_.back().textRender = 0;
                out_ += "0 Tr\n";
            }
            form_.text = true;
            paintCount_++;
        }
        if (Used() > kMaxContentBytes) failed_ = true;
    }

    // ----- Images

    void PaintImage(int index, const Style& s, const Matrix& ctm, const Context& context) {
        const double fs = s.fontSize;
        const double x = LengthAttr(index, "x", kAxisX, fs, 0.0);
        const double y = LengthAttr(index, "y", kAxisY, fs, 0.0);
        if (context.mode == kModeMeasure) {
            PathWriter writer(nullptr, ctm, false, kTransformedBox);
            writer.Rect(x, y, LengthAttr(index, "width", kAxisX, fs, 0.0), LengthAttr(index, "height", kAxisY, fs, 0.0));
            measureBox_.Add(writer.box);
            return;
        }
        if (s.hidden) return;
        // Only data: URIs of PNG and JPEG images; a file or URL is never read.
        const std::string_view href = Href(index);
        const size_t comma = href.find(',');
        if (comma == std::string_view::npos || !StartsWithIgnoreCase(href, "data:")) return;
        const std::string_view header = href.substr(5, comma - 5);
        const bool base64 = header.size() >= 7 && EqualsIgnoreCase(header.substr(header.size() - 7), ";base64");
        const std::string_view mime = header.substr(0, header.find(';'));
        if (!base64 || !(EqualsIgnoreCase(mime, "image/png") || EqualsIgnoreCase(mime, "image/jpeg") || EqualsIgnoreCase(mime, "image/jpg"))) {
            return;
        }
        auto cached = images_.find(href);
        if (cached == images_.end()) {
            std::array<int, 3> image = { -1, 0, 0 };
            std::string bytes;
            if (DecodeBase64(href.substr(comma + 1), bytes)) image[0] = host_.AddImage(bytes, image[1], image[2]);
            cached = images_.emplace(href, image).first;
        }
        const int image = cached->second[0];
        const double pixelWidth = cached->second[1];
        const double pixelHeight = cached->second[2];
        if (image < 0 || pixelWidth <= 0.0 || pixelHeight <= 0.0) return;
        const double width = LengthAttr(index, "width", kAxisX, fs, pixelWidth);
        const double height = LengthAttr(index, "height", kAxisY, fs, pixelHeight);
        if (!(width > 0.0 && height > 0.0)) return;
        AspectRatio aspect;
        std::string_view text;
        if (Attr(index, "preserveAspectRatio", text)) ParseAspectRatio(text, aspect);
        const double box[4] = { 0.0, 0.0, pixelWidth, pixelHeight };
        const Matrix fit = ViewBoxTransform(box, width, height, aspect);
        // The image XObject is the unit square, its first row at the top.
        const Matrix placement = Multiply(Translation(x, y), Multiply(fit, Matrix{ pixelWidth, 0.0, 0.0, -pixelHeight, 0.0, pixelHeight }));
        const Matrix matrix = Multiply(ctm, placement);
        if (!IsUsable(matrix) || !MatrixFits(matrix)) return;
        const double alpha = context.opacity;
        if (alpha < 0.0005) return;
        geom_.clear();
        PathWriter clip(&geom_, ctm, false, kNoBox, Remaining());
        if (aspect.slice) {
            clip.Rect(x, y, width, height);
            if (clip.bad) return;
        }
        if (!Push()) return;
        if (aspect.slice) {
            out_ += geom_;
            out_ += "W n\n";
        }
        SetAlpha(alpha, -1.0);
        AppendMatrix(out_, matrix);
        out_ += "/Im";
        out_ += std::to_string(image + 1);
        out_ += " Do\n";
        Pop();
        if (std::find(form_.images.begin(), form_.images.end(), image) == form_.images.end()) form_.images.push_back(image);
        paintCount_++;
        if (Used() > kMaxContentBytes) failed_ = true;
    }

    const Document& doc_;
    SvgHost& host_;
    SvgForm& form_;
    std::string& out_;

    std::unordered_map<std::string_view, int> ids_;
    std::deque<std::string> sheets_;
    std::vector<SheetDeclaration> declarations_;
    std::vector<Selector> selectors_;   // in document order
    std::unordered_map<std::string_view, int> selectorIndex_;   // the first selector filed under a key
    mutable std::vector<uint32_t> matched_;   // the selectors that match the element being styled
    mutable std::string key_;
    mutable std::unordered_map<std::string_view, int> blankedIndex_;   // style attributes with comments ...
    mutable std::deque<std::string> blanked_;                          // ... and their text without them
    int universal_ = -1;      // the first "*" selector
    enum : uint8_t { kTypeKeys = 1, kIdKeys = 2, kClassKeys = 4 };
    uint8_t keyKinds_ = 0;    // which kinds of keys selectors are filed under
    size_t ruleCount_ = 0;
    bool styleDependent_ = false;
    bool importantRules_ = false;   // a style sheet declaration is !important

    double viewportWidth_ = 0.0, viewportHeight_ = 0.0, viewportDiagonal_ = 0.0;
    std::vector<GraphicsState> states_;
    std::unordered_map<uint32_t, int> extGStates_;
    std::unordered_map<std::string, int> shadings_;
    std::unordered_map<std::string_view, std::array<int, 3>> images_;   // data: URI → host index, width, height
    std::vector<uint8_t> active_;   // elements being rendered, for use cycles
    size_t instantiated_ = 0;
    size_t budget_ = 0;            // elements instantiated: 4 per element of the markup and 100,000
    mutable size_t work_ = 0;      // bytes of attributes, declarations and text read while instantiating
    size_t workLimit_;             // 12 per byte of the markup and 16 MiB (a 20k-point matplotlib scatter reads 6.3)
    size_t pathNumbers_ = 0;
    size_t paintCount_ = 0;
    size_t textBytes_ = 0;
    size_t textElementBytes_ = 0;
    size_t resourceBytes_ = 0;      // the resource dictionaries and their deduplication keys
    int clipDepth_ = 0;
    mutable bool failed_ = false;

    std::string geom_;       // the path of the leaf being painted
    std::string clipPath_;   // the geometry of the clip being built
    bool clipEvenOdd_ = true;
    Box measureBox_;
    bool measureStrokes_ = false;   // measures add the reach of strokes (viewport overflow tests)
    std::string dash_;
    Gradient gradient_;
    int gradientNode_ = -1;  // the element gradient_ was resolved from
    std::vector<TextRun> runs_;
    double pendingX_ = NAN, pendingY_ = NAN, pendingDx_ = NAN, pendingDy_ = NAN;
    bool lastSpace_ = true;
};

} // namespace

bool LooksLikeSvg(std::string_view bytes) {
    size_t at = bytes.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    while (true) {
        while (at < bytes.size() && IsSpace(bytes[at])) at++;
        const std::string_view rest = bytes.substr(at);
        if (rest.substr(0, 4) == "<!--") {
            const size_t close = bytes.find("-->", at + 4);
            if (close == std::string_view::npos) return false;
            at = close + 3;
        } else if (rest.substr(0, 2) == "<?") {
            const size_t close = bytes.find("?>", at + 2);
            if (close == std::string_view::npos) return false;
            at = close + 2;
        } else if (StartsWithIgnoreCase(rest, "<!DOCTYPE")) {
            char quote = 0;
            int brackets = 0;
            for (at += 9; at < bytes.size(); at++) {
                const char ch = bytes[at];
                if (quote != 0) {
                    if (ch == quote) quote = 0;
                } else if (ch == '"' || ch == '\'') {
                    quote = ch;
                } else if (ch == '[') {
                    brackets++;
                } else if (ch == ']') {
                    brackets--;
                } else if (ch == '>' && brackets <= 0) {
                    break;
                }
            }
            if (at >= bytes.size()) return false;
            at++;
        } else {
            break;
        }
    }
    if (bytes.substr(at, 4) != "<svg" || at + 4 >= bytes.size()) return false;
    const char next = bytes[at + 4];
    return IsSpace(next) || next == '>' || next == '/';
}

bool ConvertSvg(std::string_view svg, SvgHost& host, SvgForm& form) {
    form = SvgForm();
    if (svg.size() > kMaxSvgBytes) return false;
    Document document;
    if (!XmlReader(svg, document).Read()) return false;
    if (Converter(document, svg.size(), host, form).Run()) return true;
    form = SvgForm();
    return false;
}

} // namespace TinyPdf::Internal
