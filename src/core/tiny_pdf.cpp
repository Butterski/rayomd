#include "rayomd/tiny_pdf.h"
#include "export_options.h"
#include "rayomd_pdf_source.h"
#include "inline_markdown.h"
#include "../common/profiling.h"
#include "../common/text_utils.h"
#include "markdown_parser.h"
#include "math_layout.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef RAYOMD_USE_CURL
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <curl/curl.h>
#endif

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif
#ifdef RAYOMD_USE_ZLIB

#include <zlib.h>
#endif

#ifdef RAYOMD_USE_SIMDUTF
#include "simdutf.h"
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define FAST_MD_SSE2 1
#endif

namespace TinyPdf {

// ForEachCodepoint sits on the per-glyph path of every Unicode document (text width,
// hex strings). Keep it inlined into its callers whatever else comes to call them.
#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_HOT_INLINE inline __attribute__((always_inline))
#else
#define RAYOMD_HOT_INLINE inline
#endif

using CidList = std::vector<uint16_t>;

static bool IsAllAscii(std::string_view str) {
    const char* at = str.data();
    size_t left = str.size();
    for (; left >= 8; at += 8, left -= 8) {
        uint64_t chunk;
        memcpy(&chunk, at, 8);
        if (chunk & 0x8080808080808080ull) return false;
    }
    for (; left > 0; at++, left--) {
        if ((unsigned char)*at >= 128) return false;
    }
    return true;
}

static std::wstring Utf8ToWideFallback(std::string_view str) {
    if (str.empty()) return L"";
#ifdef _WIN32
    // UTF-16 never needs more code units than UTF-8 needs bytes, so one call suffices.
    std::wstring wstr(str.size(), 0);
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstr[0], (int)wstr.size());
    if (len <= 0) return L"";
    wstr.resize((size_t)len);
    return wstr;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str.data(), str.data() + str.size());
#endif
}

static std::wstring Utf8ToWide(std::string_view str) {
    if (str.empty()) return L"";
    // ASCII, the usual case for a span or a table cell, needs no decoder.
    if (IsAllAscii(str)) return std::wstring(str.begin(), str.end());
#ifdef RAYOMD_USE_SIMDUTF
#if defined(_WIN32)
    const size_t units = simdutf::utf16_length_from_utf8(str.data(), str.size());
    if (units == 0) return Utf8ToWideFallback(str);
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be UTF-16 sized");
    std::wstring wstr(units, 0);
    size_t written = simdutf::convert_valid_utf8_to_utf16le(
        str.data(), str.size(), reinterpret_cast<char16_t*>(&wstr[0]));
    if (written == 0) return Utf8ToWideFallback(str);
    wstr.resize(written);
    return wstr;
#else
    const size_t units = simdutf::utf16_length_from_utf8(str.data(), str.size());
    if (units == 0) return Utf8ToWideFallback(str);
    std::u16string utf16(units, 0);
    size_t written = simdutf::convert_valid_utf8_to_utf16le(str.data(), str.size(), &utf16[0]);
    if (written == 0) return Utf8ToWideFallback(str);
    std::wstring wstr(written, 0);
    for (size_t i = 0; i < written; i++) {
        wstr[i] = (wchar_t)utf16[i];
    }
    return wstr;
#endif
#else
    return Utf8ToWideFallback(str);
#endif
}

// Appends Utf8ToWide(str) to `out`. ASCII and, on Windows, one decoder call are written
// straight into `out`, so a caller that reuses the string converts without allocating.
static void AppendUtf8ToWide(std::wstring& out, std::string_view str) {
    if (str.empty()) return;
    if (IsAllAscii(str)) {
        out.append(str.begin(), str.end());
        return;
    }
#if defined(_WIN32) && !defined(RAYOMD_USE_SIMDUTF)
    const size_t base = out.size();
    out.resize(base + str.size());
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &out[base], (int)str.size());
    out.resize(base + (len > 0 ? (size_t)len : 0));
#else
    out += Utf8ToWide(str);
#endif
}

#ifdef _WIN32
static std::string WideToUtf8(std::wstring_view str) {
    if (str.empty()) return {};
    int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, str.data(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8((size_t)length, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, str.data(), (int)str.size(),
            &utf8[0], length, nullptr, nullptr) != length) return {};
    return utf8;
}
#endif

constexpr double PAGE_W = 595.0;  // A4, points
constexpr double PAGE_H = 842.0;
static thread_local int g_lastError = 0;

int GetLastError() {
    return g_lastError;
}

static double ResolveMarginPoints(const PdfMargin& margin) {
    return Internal::ResolveMarginPoints(margin);
}

using Internal::Block;
using Internal::BlockType;
using Internal::NormalizeSymbols;
using Internal::ParseMarkdown;
using Internal::SplitLines;
using Internal::StripInlineMarkdown;

static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}
static uint16_t ReadU16(const std::vector<uint8_t>& data, size_t off) {
    if (off + 2 > data.size()) return 0;
    return (uint16_t)((data[off] << 8) | data[off + 1]);
}

static int16_t ReadS16(const std::vector<uint8_t>& data, size_t off) {
    return (int16_t)ReadU16(data, off);
}

static uint32_t ReadU32(const std::vector<uint8_t>& data, size_t off) {
    if (off + 4 > data.size()) return 0;
    return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
        ((uint32_t)data[off + 2] << 8) | data[off + 3];
}

static bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    if (size <= 0 || size > 64LL * 1024LL * 1024LL) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize((size_t)size);
    return (bool)file.read((char*)bytes.data(), size);
}

#ifdef _WIN32
static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 64LL * 1024LL * 1024LL) {
        CloseHandle(hFile);
        return false;
    }

    bytes.resize((size_t)size.QuadPart);
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - offset, 1024 * 1024);
        DWORD read = 0;
        if (!ReadFile(hFile, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            CloseHandle(hFile);
            return false;
        }
        offset += read;
    }

    CloseHandle(hFile);
    return true;
}
#endif

struct TtfFont {
    struct Table {
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    std::vector<uint8_t> bytes;
    std::map<std::string, Table> tables;
    std::vector<uint16_t> glyphForBmp;   // glyph index by BMP code point, 0 = no glyph
    std::vector<uint16_t> advances;
    std::vector<uint32_t> loca;
    mutable std::array<std::atomic<uint16_t>, 65536> widthCache{};
    uint16_t unitsPerEm = 1000;
    uint16_t glyphCount = 0;
    uint16_t metricCount = 0;
    int16_t indexToLocFormat = 1;
    int16_t ascent = 900;
    int16_t descent = -220;
    int16_t xMin = 0;
    int16_t yMin = -220;
    int16_t xMax = 1000;
    int16_t yMax = 900;
    bool loaded = false;

    bool Load() {
#ifdef _WIN32
        wchar_t winDir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;

        std::vector<std::wstring> candidates;
        std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";
        candidates.push_back(fontsDir + L"segoeui.ttf");
        candidates.push_back(fontsDir + L"arial.ttf");
        candidates.push_back(fontsDir + L"tahoma.ttf");
#else
        std::vector<std::string> candidates;
        if (const char* explicitFont = std::getenv("RAYOMD_FONT")) {
            candidates.push_back(explicitFont);
        }
        candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        candidates.push_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
        candidates.push_back("/usr/share/fonts/truetype/freefont/FreeSans.ttf");
        candidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
        candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
#endif

        for (const auto& path : candidates) {
            bytes.clear();
            tables.clear();
            glyphForBmp.clear();
            advances.clear();
            loca.clear();
            for (auto& width : widthCache) width.store(0, std::memory_order_relaxed);
            if (ReadWholeFile(path, bytes) && Parse()) {
                loaded = true;
                return true;
            }
        }
        return false;
    }

    bool Parse() {
        if (bytes.size() < 12) return false;
        uint16_t numTables = ReadU16(bytes, 4);
        if (12 + (size_t)numTables * 16 > bytes.size()) return false;

        for (uint16_t i = 0; i < numTables; i++) {
            size_t off = 12 + (size_t)i * 16;
            std::string tag((const char*)bytes.data() + off, 4);
            Table t{ ReadU32(bytes, off + 8), ReadU32(bytes, off + 12) };
            if ((size_t)t.offset + t.length <= bytes.size()) tables[tag] = t;
        }

        if (!tables.count("head") || !tables.count("hhea") || !tables.count("maxp") ||
            !tables.count("hmtx") || !tables.count("cmap") || !tables.count("loca") ||
            !tables.count("glyf")) {
            return false;
        }

        Table head = tables["head"];
        unitsPerEm = ReadU16(bytes, head.offset + 18);
        xMin = ReadS16(bytes, head.offset + 36);
        yMin = ReadS16(bytes, head.offset + 38);
        xMax = ReadS16(bytes, head.offset + 40);
        yMax = ReadS16(bytes, head.offset + 42);
        indexToLocFormat = ReadS16(bytes, head.offset + 50);
        if (unitsPerEm == 0) unitsPerEm = 1000;

        Table hhea = tables["hhea"];
        ascent = ReadS16(bytes, hhea.offset + 4);
        descent = ReadS16(bytes, hhea.offset + 6);
        metricCount = ReadU16(bytes, hhea.offset + 34);

        Table maxp = tables["maxp"];
        glyphCount = ReadU16(bytes, maxp.offset + 4);
        if (glyphCount == 0 || metricCount == 0) return false;

        Table hmtx = tables["hmtx"];
        advances.assign(glyphCount, 500);
        uint16_t lastAdvance = 500;
        for (uint16_t i = 0; i < glyphCount; i++) {
            if (i < metricCount) {
                size_t off = hmtx.offset + (size_t)i * 4;
                if (off + 2 > bytes.size()) return false;
                lastAdvance = ReadU16(bytes, off);
                advances[i] = lastAdvance;
            } else {
                advances[i] = lastAdvance;
            }
        }

        if (!ParseCmap()) return false;
        if (!ParseLoca()) return false;
        return GlyphFor('A') != 0 && GlyphFor(' ') != 0;
    }

    bool ParseCmap() {
        glyphForBmp.assign(65536, 0);
        Table cmap = tables["cmap"];
        if (cmap.offset + 4 > bytes.size()) return false;
        uint16_t count = ReadU16(bytes, cmap.offset + 2);
        size_t chosen = 0;

        for (uint16_t i = 0; i < count; i++) {
            size_t rec = cmap.offset + 4 + (size_t)i * 8;
            if (rec + 8 > bytes.size()) return false;
            uint16_t platform = ReadU16(bytes, rec);
            uint16_t encoding = ReadU16(bytes, rec + 2);
            uint32_t subOffset = ReadU32(bytes, rec + 4);
            size_t sub = cmap.offset + subOffset;
            if (sub + 2 > bytes.size()) continue;
            uint16_t format = ReadU16(bytes, sub);
            if (format == 4 && platform == 3 && (encoding == 1 || encoding == 10)) {
                chosen = sub;
                break;
            }
            if (format == 4 && chosen == 0) chosen = sub;
        }

        if (chosen == 0) return false;
        uint16_t length = ReadU16(bytes, chosen + 2);
        uint16_t segCount = ReadU16(bytes, chosen + 6) / 2;
        if (segCount == 0 || chosen + length > bytes.size()) return false;

        size_t endCode = chosen + 14;
        size_t startCode = endCode + (size_t)segCount * 2 + 2;
        size_t idDelta = startCode + (size_t)segCount * 2;
        size_t idRangeOffset = idDelta + (size_t)segCount * 2;
        if (idRangeOffset + (size_t)segCount * 2 > chosen + length) return false;

        for (uint16_t i = 0; i < segCount; i++) {
            uint16_t start = ReadU16(bytes, startCode + (size_t)i * 2);
            uint16_t end = ReadU16(bytes, endCode + (size_t)i * 2);
            int16_t delta = ReadS16(bytes, idDelta + (size_t)i * 2);
            uint16_t range = ReadU16(bytes, idRangeOffset + (size_t)i * 2);
            if (start > end) continue;

            for (uint32_t cp = start; cp <= end && cp < 65536; cp++) {
                uint16_t glyph = 0;
                if (range == 0) {
                    glyph = (uint16_t)((cp + delta) & 0xffff);
                } else {
                    size_t glyphOff = idRangeOffset + (size_t)i * 2 + range + (size_t)(cp - start) * 2;
                    if (glyphOff + 2 <= chosen + length) {
                        glyph = ReadU16(bytes, glyphOff);
                        if (glyph != 0) glyph = (uint16_t)((glyph + delta) & 0xffff);
                    }
                }
                if (glyph != 0) glyphForBmp[(uint16_t)cp] = glyph;
                if (cp == 0xffff) break;
            }
        }

        return true;
    }

    bool ParseLoca() {
        Table table = tables["loca"];
        loca.assign((size_t)glyphCount + 1, 0);
        if (indexToLocFormat == 0) {
            if ((size_t)table.offset + (size_t)(glyphCount + 1) * 2 > bytes.size()) return false;
            for (uint32_t i = 0; i <= glyphCount; i++) {
                loca[i] = (uint32_t)ReadU16(bytes, table.offset + (size_t)i * 2) * 2u;
            }
        } else {
            if ((size_t)table.offset + (size_t)(glyphCount + 1) * 4 > bytes.size()) return false;
            for (uint32_t i = 0; i <= glyphCount; i++) {
                loca[i] = ReadU32(bytes, table.offset + (size_t)i * 4);
            }
        }

        Table glyf = tables["glyf"];
        for (uint32_t off : loca) {
            if (off > glyf.length) return false;
        }
        return true;
    }

    uint16_t GlyphFor(uint32_t cp) const {
        if (cp < glyphForBmp.size()) {
            uint16_t glyph = glyphForBmp[cp];
            if (glyph != 0) return glyph;
        }
        if (cp != '?') return GlyphFor('?');
        return 0;
    }

    uint16_t WidthForCid(uint16_t cid) const {
        uint16_t cached = widthCache[cid].load(std::memory_order_relaxed);
        if (cached != 0) return cached;
        uint16_t glyph = GlyphFor(cid);
        uint16_t width = glyph < advances.size()
            ? (uint16_t)((advances[glyph] * 1000u + unitsPerEm / 2u) / unitsPerEm)
            : (uint16_t)500;
        widthCache[cid].store(width, std::memory_order_relaxed);
        return width;
    }

    int Metric(int value) const {
        return (int)((value * 1000.0) / unitsPerEm);
    }
};

template<typename Fn>
static RAYOMD_HOT_INLINE void ForEachCodepoint(std::wstring_view text, Fn fn) {
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t cp = (uint16_t)text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            uint32_t lo = (uint16_t)text[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        fn(cp);
    }
}

static uint16_t CidForCodepoint(const TtfFont& font, uint32_t cp) {
    return (cp <= 0xffff && font.GlyphFor(cp) != 0) ? (uint16_t)cp : (uint16_t)'?';
}

static double CodepointWidth(const TtfFont& font, uint32_t cp, double scale) {
    return font.WidthForCid(CidForCodepoint(font, cp)) * scale;
}

static double TextWidth(const TtfFont& font, std::wstring_view text, double size) {
    double w = 0.0;
    const double scale = size * 0.001;
    ForEachCodepoint(text, [&](uint32_t cp) {
        w += CodepointWidth(font, cp, scale);
    });
    return w;
}

static bool IsWideSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r';
}

// Appends the words of `text`, which starts and ends with a word, one space apart.
static void AppendSingleSpaced(std::wstring& out, std::wstring_view text) {
    for (size_t i = 0; i < text.size();) {
        size_t end = i;
        while (end < text.size() && !IsWideSpace(text[end])) end++;
        if (i != 0) out.push_back(L' ');
        out.append(text.data() + i, end - i);
        i = end;
        while (i < text.size() && IsWideSpace(text[i])) i++;
    }
}

// Greedy word wrap of text without inline styles: emit(line) receives every line. A line
// is the piece of `raw` from its first to its last word; only when two of its words are
// not exactly one space apart is it rebuilt in a scratch string. The view passed to emit
// is valid until emit returns.
template <typename Emit>
static void WrapWideWords(const TtfFont& font, std::wstring_view raw, double maxWidth, double size, Emit emit) {
    const double spaceWidth = TextWidth(font, L" ", size);
    std::wstring respaced;
    size_t lineStart = 0;
    size_t lineEnd = 0;         // the line is raw[lineStart, lineEnd); empty when they are equal
    bool singleSpaced = true;
    double lineWidth = 0.0;
    bool emitted = false;
    auto flush = [&]() {
        std::wstring_view line = raw.substr(lineStart, lineEnd - lineStart);
        if (!singleSpaced) {
            respaced.clear();
            AppendSingleSpaced(respaced, line);
            line = respaced;
        }
        emit(line);
        emitted = true;
    };
    auto startLine = [&](size_t start, size_t end, double width) {
        lineStart = start;
        lineEnd = end;
        lineWidth = width;
        singleSpaced = true;
    };

    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && IsWideSpace(raw[i])) i++;
        const size_t start = i;
        while (i < raw.size() && !IsWideSpace(raw[i])) i++;
        if (i == start) continue;
        const std::wstring_view word = raw.substr(start, i - start);
        const double wordWidth = TextWidth(font, word, size);
        const bool lineEmpty = lineEnd == lineStart;

        if (!lineEmpty && lineWidth + spaceWidth + wordWidth <= maxWidth) {
            if (start - lineEnd != 1 || raw[lineEnd] != L' ') singleSpaced = false;
            lineEnd = i;
            lineWidth += spaceWidth + wordWidth;
            continue;
        }
        if (lineEmpty && wordWidth <= maxWidth) {
            startLine(start, i, wordWidth);
            continue;
        }
        if (!lineEmpty) {
            flush();
            lineStart = lineEnd = 0;
            lineWidth = 0.0;
        }
        if (wordWidth <= maxWidth) {
            startLine(start, i, wordWidth);
            continue;
        }

        // A word wider than the line is cut into pieces; the last piece stays open.
        size_t partStart = start;
        double partWidth = 0.0;
        const double scale = size * 0.001;
        for (size_t at = start; at < i; at++) {
            const double chWidth = CodepointWidth(font, (uint16_t)raw[at], scale);
            if (at > partStart && partWidth + chWidth > maxWidth) {
                emit(raw.substr(partStart, at - partStart));
                emitted = true;
                partStart = at;
                partWidth = 0.0;
            }
            partWidth += chWidth;
        }
        startLine(partStart, i, partWidth);
    }
    if (lineEnd != lineStart) flush();
    if (!emitted) emit(std::wstring_view(L""));
}

static std::vector<std::wstring> WrapText(const TtfFont& font, std::wstring_view raw, double maxWidth, double size) {
    std::vector<std::wstring> lines;
    lines.reserve(std::max<size_t>(1, raw.size() / 72));
    WrapWideWords(font, raw, maxWidth, size, [&](std::wstring_view line) {
        lines.emplace_back(line.data(), line.size());
    });
    return lines;
}

static std::vector<std::wstring> WrapCodeLine(const TtfFont& font, std::wstring_view line, double maxWidth, double size) {
    std::vector<std::wstring> lines;
    std::wstring part;
    part.reserve(std::min<size_t>(line.size(), 256));
    double partWidth = 0.0;
    const double scale = size * 0.001;
    for (wchar_t ch : line) {
        if (ch == L'\t') ch = L' ';
        double chWidth = CodepointWidth(font, (uint16_t)ch, scale);
        if (!part.empty() && partWidth + chWidth > maxWidth) {
            lines.push_back(part);
            part.clear();
            partWidth = 0.0;
        }
        part.push_back(ch);
        partWidth += chWidth;
    }
    lines.push_back(part);
    return lines;
}

static void AppendF(std::string& out, double v) {
    RayoMd::Text::AppendFixed2(out, v);
}

static std::string F(double v) {
    std::string s;
    s.reserve(16);
    AppendF(s, v);
    return s;
}

static void AppendInt(std::string& out, int value) {
    char buf[24];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec == std::errc()) out.append(buf, (size_t)(result.ptr - buf));
}

static void AppendSize(std::string& out, size_t value) {
    char buf[32];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec == std::errc()) out.append(buf, (size_t)(result.ptr - buf));
}

// Grows a content stream once by an upper bound, lets one drawing operation write its
// operators through a raw pointer, and trims to what was written. The per-span paint
// functions used a dozen small appends each; this is one growth and no temporaries.
class TailWriter {
public:
    TailWriter(std::string& target, size_t maxBytes) : out(target) {
        const size_t base = out.size();
        out.resize(base + maxBytes);
        cursor = &out[base];
    }
    ~TailWriter() { out.resize((size_t)(cursor - out.data())); }
    TailWriter(const TailWriter&) = delete;
    TailWriter& operator=(const TailWriter&) = delete;

    template <size_t N>
    void Lit(const char (&text)[N]) {
        memcpy(cursor, text, N - 1);
        cursor += N - 1;
    }
    void Bytes(const char* text, size_t size) {
        memcpy(cursor, text, size);
        cursor += size;
    }
    void Fixed(double value) { cursor = RayoMd::Text::WriteFixed2(cursor, value); }
    void Number(size_t value) { cursor = std::to_chars(cursor, cursor + 20, value).ptr; }
    char* cursor = nullptr;

private:
    std::string& out;
};

// Room for one operand written by TailWriter::Fixed, with its separator.
constexpr size_t kOperandBytes = RayoMd::Text::kFixed2MaxChars + 1;

static char* WriteHex4(char* out, uint16_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    out[0] = digits[(value >> 12) & 0xf];
    out[1] = digits[(value >> 8) & 0xf];
    out[2] = digits[(value >> 4) & 0xf];
    out[3] = digits[value & 0xf];
    return out + 4;
}

static void AppendHex4(std::string& out, uint16_t value) {
    char hex[4];
    WriteHex4(hex, value);
    out.append(hex, 4);
}

class UsedCidSet {
public:
    void Add(uint16_t cid) {
        uint64_t mask = 1ull << (cid & 63);
        uint64_t& word = bits[cid >> 6];
        if ((word & mask) != 0) return;
        word |= mask;
        values.push_back(cid);
        sorted = false;
    }

    const CidList& Values() const {
        if (!sorted) {
            std::sort(values.begin(), values.end());
            sorted = true;
        }
        return values;
    }

private:
    std::array<uint64_t, 1024> bits{};
    mutable CidList values;
    mutable bool sorted = true;
};

// Writes the hex string "<....>" of `text` (four digits per CID) and records the CIDs.
// `out` must have room for HexTextBytes(text) characters; returns the end.
static size_t HexTextBytes(std::wstring_view text) {
    return 2 + text.size() * 4;
}

static char* WriteHexText(char* out, const TtfFont& font, std::wstring_view text, UsedCidSet& usedCids) {
    *out++ = '<';
    ForEachCodepoint(text, [&](uint32_t cp) {
        uint16_t cid = CidForCodepoint(font, cp);
        usedCids.Add(cid);
        out = WriteHex4(out, cid);
    });
    *out++ = '>';
    return out;
}

static std::string HexText(const TtfFont& font, const std::wstring& text, UsedCidSet& usedCids) {
    std::string out(HexTextBytes(text), '\0');
    out.resize((size_t)(WriteHexText(&out[0], font, text, usedCids) - out.data()));
    return out;
}

static void AppendU16(std::string& out, uint16_t v) {
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

static void AppendU32(std::string& out, uint32_t v) {
    out.push_back((char)((v >> 24) & 0xff));
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

static void SetU16(std::string& out, size_t off, uint16_t v) {
    if (off + 2 > out.size()) return;
    out[off] = (char)((v >> 8) & 0xff);
    out[off + 1] = (char)(v & 0xff);
}

static void SetU32(std::string& out, size_t off, uint32_t v) {
    if (off + 4 > out.size()) return;
    out[off] = (char)((v >> 24) & 0xff);
    out[off + 1] = (char)((v >> 16) & 0xff);
    out[off + 2] = (char)((v >> 8) & 0xff);
    out[off + 3] = (char)(v & 0xff);
}

static uint32_t ChecksumBytes(const char* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 4) {
        uint32_t word = 0;
        word |= (uint32_t)(unsigned char)data[i] << 24;
        if (i + 1 < size) word |= (uint32_t)(unsigned char)data[i + 1] << 16;
        if (i + 2 < size) word |= (uint32_t)(unsigned char)data[i + 2] << 8;
        if (i + 3 < size) word |= (uint32_t)(unsigned char)data[i + 3];
        sum += word;
    }
    return sum;
}

static uint32_t ChecksumString(const std::string& s) {
    return ChecksumBytes(s.data(), s.size());
}

static std::string MakeCidKey(const CidList& used) {
    std::string key;
    key.reserve(used.size() * 2);
    for (uint16_t cid : used) {
        key.push_back((char)((cid >> 8) & 0xff));
        key.push_back((char)(cid & 0xff));
    }
    return key;
}

static size_t DecimalDigits(size_t value) {
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

class PdfObjects {
public:
    int Reserve() {
        objects.emplace_back();
        return (int)objects.size();
    }

    int Add(const std::string& body) {
        objects.emplace_back();
        objects.back().head = body;
        return (int)objects.size();
    }

    int Add(std::string&& body) {
        objects.emplace_back();
        objects.back().head = std::move(body);
        return (int)objects.size();
    }

    void Set(int id, std::string&& body) {
        if (Object* object = At(id)) {
            *object = Object();
            object->head = std::move(body);
        }
    }

    // The View forms below reference their payload instead of copying it. The caller keeps
    // that memory alive and unchanged until BuildInto has run: image data and the font file
    // are then written exactly once, straight into the final buffer.

    // The whole object body is `body`.
    void SetView(int id, std::string_view body) {
        if (Object* object = At(id)) {
            *object = Object();
            object->view = body;
        }
    }

    // A stream object whose dictionary text, up to and including "stream\n", is `head`.
    void SetStreamView(int id, std::string&& head, std::string_view data) {
        if (Object* object = At(id)) {
            *object = Object();
            object->head = std::move(head);
            object->view = data;
            object->stream = true;
        }
    }

    int AddStreamView(const std::string& dict, std::string_view data) {
        objects.emplace_back();
        Object& object = objects.back();
        object.head = StreamHead(dict, data.size());
        object.view = data;
        object.stream = true;
        return (int)objects.size();
    }

    // A stream object whose payload already lies in the buffer BuildInto assembles the file
    // in, at [offset, offset + length). Page content is rendered straight into that buffer.
    // The payloads are added in the order they lie in the buffer.
    int AddStreamInPlace(const std::string& dict, size_t offset, size_t length) {
        objects.emplace_back();
        Object& object = objects.back();
        object.head = StreamHead(dict, length);
        object.inPlaceOffset = offset;
        object.inPlaceBytes = length;
        object.inPlace = true;
        object.stream = true;
        return (int)objects.size();
    }

    // Copies `data`; for small payloads that do not outlive the call.
    int AddStream(const std::string& dict, const std::string& data) {
        std::string body = StreamHead(dict, data.size());
        body.reserve(body.size() + data.size() + 10);
        body += data;
        body += "\nendstream";
        return Add(std::move(body));
    }

    // Assembles the file in `pdf`, which holds the in-place payloads and nothing else.
    //
    // Every object ends up at or after the place its payload was rendered to, because the
    // header and the objects before it only add bytes in front. Writing the objects from
    // the last to the first therefore moves each payload once, towards the end of the
    // buffer, without ever overwriting one that has not been moved yet. A buffer the
    // caller reuses for the next export is assembled without any allocation.
    void BuildInto(int rootId, int infoId, std::string& pdf, bool pdf20 = false) const {
        constexpr size_t kHeaderBytes = 15;
        constexpr size_t kObjectOpenBytes = 7;      // " 0 obj\n"
        constexpr size_t kStreamCloseBytes = 10;    // "\nendstream"
        constexpr size_t kObjectCloseBytes = 8;     // "\nendobj\n"
        const size_t count = objects.size();

        std::vector<size_t> offsets(count + 1, 0);
        size_t position = kHeaderBytes;
        size_t unmoved = 0;         // end of the in-place payloads of the objects seen so far
        bool movable = true;
        for (size_t i = 0; i < count; i++) {
            const Object& object = objects[i];
            offsets[i + 1] = position;
            const size_t payloadAt = position + DecimalDigits(i + 1) + kObjectOpenBytes + object.head.size();
            if (object.inPlace) {
                const size_t payloadEnd = object.inPlaceOffset + object.inPlaceBytes;
                if (object.inPlaceOffset < unmoved || payloadAt < object.inPlaceOffset || payloadEnd > pdf.size()) {
                    movable = false;
                }
                unmoved = payloadEnd;
            }
            position = payloadAt + PayloadBytes(object) + (object.stream ? kStreamCloseBytes : 0) + kObjectCloseBytes;
        }
        const size_t xref = position;
        const size_t total = xref + WriteTail(nullptr, offsets, xref, rootId, infoId);

        // A buffer far larger than this file needs is given back, and payloads that do not
        // lie in rendering order cannot be moved in place: both cases assemble the file in
        // a new buffer and read the payloads from the old one.
        constexpr size_t kRetentionFloor = 4u * 1024u * 1024u;
        const size_t wanted = total + 64 * 1024;
        std::string rendered;
        const bool relocate = !movable || (pdf.capacity() > kRetentionFloor && wanted < pdf.capacity() / 4);
        if (relocate) {
            rendered.swap(pdf);
            pdf.reserve(wanted);
        }
        pdf.resize(total);
        char* const base = &pdf[0];
        const char* const payloads = relocate ? rendered.data() : base;
        const size_t payloadLimit = relocate ? rendered.size() : total;

        WriteTail(base + xref, offsets, xref, rootId, infoId);
        for (size_t i = count; i-- > 0;) {
            const Object& object = objects[i];
            char* const at = base + offsets[i + 1];
            const size_t idDigits = DecimalDigits(i + 1);
            char* const payload = at + idDigits + kObjectOpenBytes + object.head.size();
            const size_t payloadBytes = PayloadBytes(object);
            // The payload moves first: its old place may overlap the object's opening text.
            if (object.inPlace) {
                if (object.inPlaceOffset + payloadBytes <= payloadLimit) {
                    memmove(payload, payloads + object.inPlaceOffset, payloadBytes);
                } else {
                    memset(payload, ' ', payloadBytes);
                }
            } else if (payloadBytes != 0) {
                memcpy(payload, object.view.data(), payloadBytes);
            }
            std::to_chars(at, at + idDigits, i + 1);
            memcpy(at + idDigits, " 0 obj\n", kObjectOpenBytes);
            memcpy(at + idDigits + kObjectOpenBytes, object.head.data(), object.head.size());
            char* cursor = payload + payloadBytes;
            if (object.stream) {
                memcpy(cursor, "\nendstream", kStreamCloseBytes);
                cursor += kStreamCloseBytes;
            }
            memcpy(cursor, "\nendobj\n", kObjectCloseBytes);
        }
        memcpy(base, pdf20 ? "%PDF-2.0\n%\xE2\xE3\xCF\xD3\n" : "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n", kHeaderBytes);
    }

private:
    struct Object {
        std::string head;           // owned text of the object, or the stream dictionary
        std::string_view view;      // referenced payload written after `head`
        size_t inPlaceOffset = 0;   // in-place payload: where it lies in the output buffer
        size_t inPlaceBytes = 0;
        bool inPlace = false;
        bool stream = false;        // "\nendstream" follows the payload
    };

    Object* At(int id) {
        return id > 0 && id <= (int)objects.size() ? &objects[(size_t)id - 1] : nullptr;
    }

    static size_t PayloadBytes(const Object& object) {
        return object.inPlace ? object.inPlaceBytes : object.view.size();
    }

    static std::string StreamHead(const std::string& dict, size_t length) {
        std::string head;
        head.reserve(dict.size() + 48);
        head += "<< ";
        head += dict;
        head += " /Length ";
        AppendSize(head, length);
        head += " >>\nstream\n";
        return head;
    }

    // The cross-reference table and the trailer. Returns their size; with `out` null
    // nothing is written, so one function both measures and writes them.
    size_t WriteTail(char* out, const std::vector<size_t>& offsets, size_t xref, int rootId, int infoId) const {
        size_t size = 0;
        auto text = [&](std::string_view value) {
            if (out) memcpy(out + size, value.data(), value.size());
            size += value.size();
        };
        auto number = [&](size_t value) {
            const size_t digits = DecimalDigits(value);
            if (out) std::to_chars(out + size, out + size + digits, value);
            size += digits;
        };
        const size_t count = objects.size();
        text("xref\n0 ");
        number(count + 1);
        text("\n0000000000 65535 f \n");
        // One fixed-width entry per object: ten digits of the offset, then " 00000 n \n".
        if (out) {
            char* entry = out + size;
            for (size_t i = 1; i <= count; i++, entry += 20) {
                size_t offset = offsets[i];
                for (int digit = 9; digit >= 0; digit--) {
                    entry[digit] = (char)('0' + offset % 10);
                    offset /= 10;
                }
                memcpy(entry + 10, " 00000 n \n", 10);
            }
        }
        size += count * 20;
        text("trailer\n<< /Size ");
        number(count + 1);
        text(" /Root ");
        number((size_t)rootId);
        text(" 0 R /Info ");
        number((size_t)infoId);
        text(" 0 R >>\nstartxref\n");
        number(xref);
        text("\n%%EOF\n");
        return size;
    }

    std::vector<Object> objects;
};

constexpr size_t kMaxImageBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxDecodedImageBytes = 96u * 1024u * 1024u;
static void AddReversibleSource(PdfObjects& pdf, std::string& catalog, const std::string& markdown) {
    std::string sourceDictionary = "/Type /EmbeddedFile /Subtype /text#2Fmarkdown /Params << /Size ";
    AppendSize(sourceDictionary, markdown.size());
    sourceDictionary += " >>";
    int sourceId = pdf.AddStreamView(sourceDictionary, markdown);

    std::string fileSpec;
    fileSpec.reserve(144);
    fileSpec += "<< /Type /Filespec /F (source.md) /UF (source.md) /EF << /F ";
    AppendInt(fileSpec, sourceId);
    fileSpec += " 0 R /UF ";
    AppendInt(fileSpec, sourceId);
    fileSpec += " 0 R >> /AFRelationship /Source >>";
    int fileSpecId = pdf.Add(std::move(fileSpec));

    std::string metadata = RayoMd::PdfSource::BuildXmpMetadata(markdown, RAYOMD_VERSION);
    int metadataId = pdf.AddStream("/Type /Metadata /Subtype /XML", metadata);

    catalog += " /Metadata ";
    AppendInt(catalog, metadataId);
    catalog += " 0 R /Names << /EmbeddedFiles << /Names [(source.md) ";
    AppendInt(catalog, fileSpecId);
    catalog += " 0 R] >> >> /AF [";
    AppendInt(catalog, fileSpecId);
    catalog += " 0 R]";
}


struct PdfImage {
    std::string stream;
    std::string maskStream;
    std::string colorSpace = "/DeviceRGB";
    std::string filter;
    std::string decodeParms;
    std::string maskFilter;
    std::string maskDecodeParms;
    int width = 0;
    int height = 0;
    int bitsPerComponent = 8;
};

using SharedPdfImage = std::shared_ptr<const PdfImage>;

struct LinkRect {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    std::string url;
};

static bool IsHttpUrl(std::string_view src) {
    return StartsWith(src, "http://") || StartsWith(src, "https://");
}

static std::string EscapeLiteral(const std::string& s);

static std::filesystem::path PathFromUtf8(std::string_view s) {
    return std::filesystem::u8path(s.begin(), s.end());
}

static std::string PathToUtf8(const std::filesystem::path& path) {
    return path.u8string();
}

#ifdef _WIN32
struct WinLocalImageFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    size_t size = 0;

    WinLocalImageFile() = default;
    WinLocalImageFile(const WinLocalImageFile&) = delete;
    WinLocalImageFile& operator=(const WinLocalImageFile&) = delete;

    WinLocalImageFile(WinLocalImageFile&& other) noexcept
        : handle(other.handle), size(other.size) {
        other.handle = INVALID_HANDLE_VALUE;
        other.size = 0;
    }

    WinLocalImageFile& operator=(WinLocalImageFile&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        handle = other.handle;
        size = other.size;
        other.handle = INVALID_HANDLE_VALUE;
        other.size = 0;
        return *this;
    }

    ~WinLocalImageFile() { Reset(); }

    void Reset() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        size = 0;
    }
};

enum class DirectLocalImageResult {
    NotApplicable,
    Rejected,
    Opened,
};

static bool GetFinalNtPath(HANDLE handle, std::wstring& path) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_NT;
    std::array<wchar_t, 512> stack{};
    DWORD length = GetFinalPathNameByHandleW(handle, stack.data(), (DWORD)stack.size(), flags);
    if (length == 0) return false;
    if (length < stack.size()) {
        path.assign(stack.data(), length);
        return true;
    }

    if (length > 32767) return false;
    std::vector<wchar_t> buffer((size_t)length + 1, L'\0');
    length = GetFinalPathNameByHandleW(handle, buffer.data(), (DWORD)buffer.size(), flags);
    if (length == 0 || length >= buffer.size()) return false;
    path.assign(buffer.data(), length);
    return true;
}

static bool ExpectedNtPathForDirectDosPath(
    const std::filesystem::path& absolute, std::wstring& expected) {
    std::wstring drive = absolute.root_name().native();
    if (drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') ||
          (drive[0] >= L'a' && drive[0] <= L'z'))) return false;

    std::vector<wchar_t> targets(4096, L'\0');
    DWORD length = QueryDosDeviceW(drive.c_str(), targets.data(), (DWORD)targets.size());
    if (length == 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        targets.assign(32768, L'\0');
        length = QueryDosDeviceW(drive.c_str(), targets.data(), (DWORD)targets.size());
    }
    if (length == 0) return false;

    std::wstring device(targets.data());
    if (device.compare(0, 8, L"\\Device\\") != 0) return false;
    std::wstring relative = absolute.relative_path().native();
    std::replace(relative.begin(), relative.end(), L'/', L'\\');
    while (!relative.empty() && relative.front() == L'\\') relative.erase(relative.begin());

    expected = std::move(device);
    if (!relative.empty()) {
        expected.push_back(L'\\');
        expected += relative;
    }
    while (!expected.empty() && expected.back() == L'\\') expected.pop_back();
    return !expected.empty();
}

static bool TryApprovedNtRootForPolicy(
    const std::filesystem::path& absolute, std::wstring& approved) {
    std::wstring expected;
    if (!ExpectedNtPathForDirectDosPath(absolute, expected)) return false;

    HANDLE handle = CreateFileW(absolute.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    std::wstring finalPath;
    bool valid = GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        GetFinalNtPath(handle, finalPath) && finalPath == expected;
    CloseHandle(handle);
    if (!valid) return false;
    approved = std::move(expected);
    return true;
}

static DirectLocalImageResult TryOpenDirectLocalImage(
    const std::filesystem::path& path, const std::wstring& approvedRoot,
    std::string& key, WinLocalImageFile& file) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? DirectLocalImageResult::Rejected
            : DirectLocalImageResult::NotApplicable;
    }

    if (GetFileType(handle) != FILE_TYPE_DISK) {
        CloseHandle(handle);
        return DirectLocalImageResult::Rejected;
    }

    std::wstring finalPath;
    if (!GetFinalNtPath(handle, finalPath)) {
        CloseHandle(handle);
        return DirectLocalImageResult::Rejected;
    }
    if (finalPath.size() <= approvedRoot.size() ||
        finalPath.compare(0, approvedRoot.size(), approvedRoot) != 0 ||
        finalPath[approvedRoot.size()] != L'\\') {
        CloseHandle(handle);
        return DirectLocalImageResult::Rejected;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !GetFileSizeEx(handle, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)kMaxImageBytes) {
        CloseHandle(handle);
        return DirectLocalImageResult::Rejected;
    }

    std::string finalUtf8 = WideToUtf8(finalPath);
    if (finalUtf8.empty()) {
        CloseHandle(handle);
        return DirectLocalImageResult::Rejected;
    }
    key = "file-nt:" + finalUtf8;
    file.Reset();
    file.handle = handle;
    file.size = (size_t)size.QuadPart;
    return DirectLocalImageResult::Opened;
}

static bool ReadLocalImageHandle(WinLocalImageFile& file, std::vector<uint8_t>& bytes) {
    if (file.handle == INVALID_HANDLE_VALUE || file.size == 0 || file.size > kMaxImageBytes) return false;
    bytes.resize(file.size);
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - offset, 1024 * 1024);
        DWORD read = 0;
        if (!ReadFile(file.handle, bytes.data() + offset, chunk, &read, nullptr) || read == 0) return false;
        offset += read;
    }
    return true;
}
#endif

static bool HasUriScheme(std::string_view src) {
    if (src.empty() || !std::isalpha((unsigned char)src[0])) return false;
    for (size_t i = 1; i < src.size(); i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ':') return true;
        if (c == '/' || c == '\\' || c == '?' || c == '#') return false;
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
    }
    return false;
}

static bool HasUnsafeWindowsPathPrefix(std::string_view src) {
    if (src.size() >= 1 && (src[0] == '/' || src[0] == '\\')) return true;
    if (src.size() >= 2 && std::isalpha((unsigned char)src[0]) && src[1] == ':') return true;
    return false;
}

static bool IsUnsafeLocalImageSource(std::string_view src) {
    if (src.empty() || src.find('\0') != std::string_view::npos) return true;
    if (HasUnsafeWindowsPathPrefix(src) || HasUriScheme(src)) return true;
    std::filesystem::path path = PathFromUtf8(src);
    return path.is_absolute();
}

static bool TryCanonicalForPolicy(
    const std::filesystem::path& path, std::filesystem::path& normalized) {
    std::error_code ec;
    std::filesystem::path absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec) return false;

    normalized = std::filesystem::canonical(absolute, ec);
    if (ec) return false;
    normalized = normalized.lexically_normal();
    return true;
}

static bool PathPartEqualForPolicy(
    const std::filesystem::path& a, const std::filesystem::path& b, bool exact) {
    std::string left = a.u8string();
    std::string right = b.u8string();
#ifdef _WIN32
    if (!exact) {
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
#endif
    return left == right;
}

struct DirectRootCacheEntry {
    std::filesystem::path root;
    std::wstring approvedNtRoot;
};

struct DirectRootCache {
    std::mutex mutex;
    std::array<DirectRootCacheEntry, 4> entries;
    size_t next = 0;
};

static DirectRootCache& RootCache() {
    static DirectRootCache cache;
    return cache;
}

static bool TryCanonicalRootForPolicy(
    const std::filesystem::path& path, std::filesystem::path& normalized,
    bool& direct, std::wstring& approvedNtRoot) {
    direct = false;
    approvedNtRoot.clear();
    std::error_code ec;
    std::filesystem::path absolute =
        path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec) return false;
    absolute = absolute.lexically_normal();

    DirectRootCache& cache = RootCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (const DirectRootCacheEntry& entry : cache.entries) {
            if (!entry.root.empty() && entry.root == absolute) {
                normalized = absolute;
                direct = true;
                approvedNtRoot = entry.approvedNtRoot;
                return true;
            }
        }
    }

    std::filesystem::path canonical;
    if (!TryCanonicalForPolicy(absolute, canonical)) return false;
    std::wstring approved;
    if (canonical == absolute) {
#ifdef _WIN32
        TryApprovedNtRootForPolicy(absolute, approved);
#endif
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (const DirectRootCacheEntry& entry : cache.entries) {
            if (!entry.root.empty() && entry.root == absolute) {
                normalized = absolute;
                direct = true;
                approvedNtRoot = entry.approvedNtRoot;
                return true;
            }
        }
        DirectRootCacheEntry& entry = cache.entries[cache.next++ % cache.entries.size()];
        entry.root = absolute;
        entry.approvedNtRoot = approved;
    }
    normalized = std::move(canonical);
    direct = normalized == absolute;
    if (direct) approvedNtRoot = std::move(approved);
    return true;
}

static bool IsPathContainedInRoot(
    const std::filesystem::path& child, const std::filesystem::path& root, bool exact) {
    std::filesystem::path childNorm = child.lexically_normal();
    std::filesystem::path rootNorm = root.lexically_normal();
    auto childIt = childNorm.begin();
    for (auto rootIt = rootNorm.begin(); rootIt != rootNorm.end(); ++rootIt, ++childIt) {
        if (childIt == childNorm.end() || !PathPartEqualForPolicy(*childIt, *rootIt, exact)) return false;
    }
    return true;
}

class LocalImagePolicy {
public:
    explicit LocalImagePolicy(const PdfOptions& options)
        : allowUnsafe(options.allowUnsafeLocalImages),
          hasSourcePath(!options.sourcePath.empty()) {
        if (!hasSourcePath) return;

        sourceBase = PathFromUtf8(options.sourcePath);
        if (sourceBase.has_filename()) sourceBase = sourceBase.parent_path();

        safeBase = sourceBase;
        if (safeBase.empty()) safeBase = ".";
    }

#ifdef _WIN32
    DirectLocalImageResult TryOpenDirect(
        const std::string& src, std::string& key, WinLocalImageFile& file) {
        if (src.empty() || IsHttpUrl(src)) return DirectLocalImageResult::Rejected;
        if (allowUnsafe) return DirectLocalImageResult::NotApplicable;
        if (!hasSourcePath || IsUnsafeLocalImageSource(src)) return DirectLocalImageResult::Rejected;

        const std::filesystem::path& root = SafeRoot();
        if (!safeRootValid) return DirectLocalImageResult::Rejected;
        if (!safeRootDirect || safeRootNt.empty()) return DirectLocalImageResult::NotApplicable;

        std::filesystem::path path = (root / PathFromUtf8(src)).lexically_normal();
        return TryOpenDirectLocalImage(path, safeRootNt, key, file);
    }
#endif

    bool Resolve(const std::string& src, std::string& key, std::string& pathUtf8) {
        if (src.empty() || IsHttpUrl(src)) return false;
        if (allowUnsafe) return ResolveUnsafe(src, key, pathUtf8);
        if (!hasSourcePath || IsUnsafeLocalImageSource(src)) return false;

        const std::filesystem::path& root = SafeRoot();
        if (!safeRootValid) return false;
        std::filesystem::path normalized;
        if (!TryCanonicalForPolicy(root / PathFromUtf8(src), normalized) ||
            !IsPathContainedInRoot(normalized, root, safeRootDirect)) return false;

        pathUtf8 = PathToUtf8(normalized);
        key = "file:" + pathUtf8;
        return true;
    }

private:
    const std::filesystem::path& SafeRoot() {
        if (!safeRootReady) {
            safeRootValid = TryCanonicalRootForPolicy(
                safeBase, safeRoot, safeRootDirect, safeRootNt);
            safeRootReady = true;
        }
        return safeRoot;
    }

    bool ResolveUnsafe(const std::string& src, std::string& key, std::string& pathUtf8) const {
        std::filesystem::path path = PathFromUtf8(src);
        if (path.is_relative() && hasSourcePath) path = sourceBase / path;

        std::filesystem::path normalized;
        if (!TryCanonicalForPolicy(path, normalized)) return false;
        pathUtf8 = PathToUtf8(normalized);
        key = "file:" + pathUtf8;
        return true;
    }

    bool allowUnsafe = false;
    bool hasSourcePath = false;
    bool safeRootReady = false;
    bool safeRootValid = false;
    bool safeRootDirect = false;
    std::filesystem::path sourceBase;
    std::filesystem::path safeBase;
    std::filesystem::path safeRoot;
    std::wstring safeRootNt;
};

static bool ReadLocalImageFile(const std::string& pathUtf8, std::vector<uint8_t>& bytes) {
#ifdef _WIN32
    return ReadWholeFile(Utf8ToWide(pathUtf8), bytes);
#else
    return ReadWholeFile(pathUtf8, bytes);
#endif
}

#if defined(_WIN32) || defined(RAYOMD_USE_CURL)
static bool IsUnsafeIpv4Address(uint32_t hostOrder) {
    return (hostOrder >> 24) == 0 ||
        (hostOrder >> 24) == 10 ||
        (hostOrder >> 24) == 127 ||
        (hostOrder >> 16) == 0xA9FEu ||
        (hostOrder >> 20) == 0xAC1u ||
        (hostOrder >> 16) == 0xC0A8u ||
        (hostOrder >> 22) == 0x0192u ||
        (hostOrder >> 28) == 0xEu ||
        (hostOrder >> 28) == 0xFu ||
        hostOrder == 0xFFFFFFFFu;
}

static bool IsUnsafeIpv6Address(const unsigned char* b) {
    bool allZero = true;
    for (int i = 0; i < 16; i++) allZero = allZero && b[i] == 0;
    if (allZero) return true;

    bool loopback = true;
    for (int i = 0; i < 15; i++) loopback = loopback && b[i] == 0;
    if (loopback && b[15] == 1) return true;

    bool mappedV4 = true;
    for (int i = 0; i < 10; i++) mappedV4 = mappedV4 && b[i] == 0;
    if (mappedV4 && b[10] == 0xff && b[11] == 0xff) {
        uint32_t mapped = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
            ((uint32_t)b[14] << 8) | (uint32_t)b[15];
        return IsUnsafeIpv4Address(mapped);
    }

    return (b[0] & 0xfe) == 0xfc ||
        (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) ||
        b[0] == 0xff;
}

static bool IsUnsafeSocketAddress(const sockaddr* address) {
    if (!address) return true;
    if (address->sa_family == AF_INET) {
        const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        return IsUnsafeIpv4Address(ntohl(ipv4->sin_addr.s_addr));
    }
    if (address->sa_family == AF_INET6) {
        const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        return IsUnsafeIpv6Address(reinterpret_cast<const unsigned char*>(&ipv6->sin6_addr));
    }
    return true;
}
#endif

#ifdef _WIN32
static bool EnsureWinsockReady() {
    static bool ready = []() {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

static bool IsFetchHostAllowed(const std::wstring& host) {
    if (host.empty() || !EnsureWinsockReady()) return false;

    ADDRINFOW hints = {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ADDRINFOW* result = nullptr;
    int rc = GetAddrInfoW(host.c_str(), nullptr, &hints, &result);
    if (rc != 0 || !result) return false;

    bool sawAddress = false;
    bool allowed = true;
    for (ADDRINFOW* current = result; current; current = current->ai_next) {
        sawAddress = true;
        if (IsUnsafeSocketAddress(current->ai_addr)) {
            allowed = false;
            break;
        }
    }
    FreeAddrInfoW(result);
    return sawAddress && allowed;
}

static std::wstring ResolveRedirectUrl(const std::wstring& currentUrl, const std::wstring& location) {
    if (location.empty()) return L"";
    if (location.rfind(L"http://", 0) == 0 || location.rfind(L"https://", 0) == 0) return location;

    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(currentUrl.c_str(), (DWORD)currentUrl.size(), 0, &parts)) return L"";

    std::wstring base;
    base.assign(parts.lpszScheme, parts.dwSchemeLength);
    base += L"://";
    base.append(parts.lpszHostName, parts.dwHostNameLength);
    if ((parts.nScheme == INTERNET_SCHEME_HTTP && parts.nPort != 80) ||
        (parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort != 443)) {
        base += L":";
        wchar_t port[16];
        swprintf(port, 16, L"%u", (unsigned)parts.nPort);
        base += port;
    }

    if (location[0] == L'/') return base + location;

    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    size_t slash = path.find_last_of(L'/');
    if (slash == std::wstring::npos) path = L"/";
    else path.resize(slash + 1);
    return base + path + location;
}

static bool FetchUrlBytesPlatform(const std::string& url, std::vector<uint8_t>& bytes) {
    std::wstring currentUrl = Utf8ToWide(url);
    if (currentUrl.empty()) return false;

    HINTERNET session = WinHttpOpen(L"RayoMD/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 15000);

    bool ok = false;
    for (int redirect = 0; redirect < 6 && !ok; redirect++) {
        URL_COMPONENTSW parts = {};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = (DWORD)-1;
        parts.dwHostNameLength = (DWORD)-1;
        parts.dwUrlPathLength = (DWORD)-1;
        parts.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(currentUrl.c_str(), (DWORD)currentUrl.size(), 0, &parts)) break;
        if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) break;

        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        if (!IsFetchHostAllowed(host)) break;

        HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        if (!connect) break;

        DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            break;
        }

        DWORD disableRedirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &disableRedirects, sizeof(disableRedirects));

        bool gotResponse = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (gotResponse) {
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        }

        if (status >= 200 && status < 300) {
            bytes.clear();
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) break;
                if (available == 0) {
                    ok = !bytes.empty();
                    break;
                }
                if (bytes.size() + available > kMaxImageBytes) break;
                size_t old = bytes.size();
                bytes.resize(old + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, bytes.data() + old, available, &read) || read == 0) break;
                bytes.resize(old + read);
            }
        } else if (status >= 300 && status < 400) {
            DWORD locationSize = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                nullptr, &locationSize, WINHTTP_NO_HEADER_INDEX);
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (locationSize > 0 && WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX, &location[0], &locationSize, WINHTTP_NO_HEADER_INDEX)) {
                location.resize(wcslen(location.c_str()));
                std::wstring next = ResolveRedirectUrl(currentUrl, location);
                if (!next.empty() && next != currentUrl) currentUrl = std::move(next);
                else redirect = 6;
            } else {
                redirect = 6;
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
    }

    WinHttpCloseHandle(session);
    return ok;
}
#elif defined(RAYOMD_USE_CURL)
struct CurlImageBuffer {
    std::vector<uint8_t>* bytes = nullptr;
    bool tooLarge = false;
};

static size_t CurlWriteImageBytes(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t count = size * nmemb;
    CurlImageBuffer* buffer = static_cast<CurlImageBuffer*>(userdata);
    if (!buffer || !buffer->bytes) return 0;
    if (buffer->bytes->size() + count > kMaxImageBytes) {
        buffer->tooLarge = true;
        return 0;
    }
    buffer->bytes->insert(buffer->bytes->end(), ptr, ptr + count);
    return count;
}

static curl_socket_t CurlOpenSocketChecked(void*, curlsocktype purpose, struct curl_sockaddr* address) {
    if (!address) return CURL_SOCKET_BAD;
    if (purpose == CURLSOCKTYPE_IPCXN && IsUnsafeSocketAddress(&address->addr)) {
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

static bool FetchUrlBytesPlatform(const std::string& url, std::vector<uint8_t>& bytes) {
    static bool curlReady = []() { return curl_global_init(CURL_GLOBAL_DEFAULT) == 0; }();
    if (!curlReady) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    bytes.clear();
    CurlImageBuffer buffer{ &bytes, false };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, CurlOpenSocketChecked);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RayoMD/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteImageBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && !buffer.tooLarge && status >= 200 && status < 300 && !bytes.empty();
}
#else
static bool FetchUrlBytesPlatform(const std::string&, std::vector<uint8_t>&) {
    return false;
}
#endif

static bool FetchUrlBytes(const std::string& src, const PdfOptions& options, std::vector<uint8_t>& bytes) {
    if (!options.enableUrlImages || !IsHttpUrl(src)) return false;
    return FetchUrlBytesPlatform(src, bytes);
}

static void AppendHexByte(std::string& out, uint8_t value) {
    static const char* h = "0123456789ABCDEF";
    out.push_back(h[(value >> 4) & 0xf]);
    out.push_back(h[value & 0xf]);
}

static std::string HexBytes(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2 + 2);
    out.push_back('<');
    for (unsigned char c : bytes) AppendHexByte(out, c);
    out.push_back('>');
    return out;
}

static bool ParseJpegImage(const std::vector<uint8_t>& bytes, PdfImage& image) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return false;

    size_t i = 2;
    while (i + 3 < bytes.size()) {
        while (i < bytes.size() && bytes[i] != 0xff) i++;
        while (i < bytes.size() && bytes[i] == 0xff) i++;
        if (i >= bytes.size()) break;

        uint8_t marker = bytes[i++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (i + 2 > bytes.size()) return false;

        uint16_t length = (uint16_t)((bytes[i] << 8) | bytes[i + 1]);
        if (length < 2 || i + length > bytes.size()) return false;

        bool sof = (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc);
        if (sof) {
            if (length < 8) return false;
            int precision = bytes[i + 2];
            int height = (bytes[i + 3] << 8) | bytes[i + 4];
            int width = (bytes[i + 5] << 8) | bytes[i + 6];
            int components = bytes[i + 7];
            if (precision != 8 || width <= 0 || height <= 0) return false;
            if (components == 1) image.colorSpace = "/DeviceGray";
            else if (components == 3) image.colorSpace = "/DeviceRGB";
            else return false;

            image.width = width;
            image.height = height;
            image.bitsPerComponent = 8;
            image.filter = "/DCTDecode";
            image.stream.assign((const char*)bytes.data(), bytes.size());
            return true;
        }

        i += length;
    }

    return false;
}

struct PngInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    int bitDepth = 0;
    int colorType = 0;
    int compression = 0;
    int filter = 0;
    int interlace = 0;
    std::string idat;
    std::string palette;
    std::vector<uint8_t> transparency;
};

static bool ParsePngChunks(const std::vector<uint8_t>& bytes, PngInfo& png) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (bytes.size() < 33 || memcmp(bytes.data(), sig, sizeof(sig)) != 0) return false;

    size_t pos = 8;
    bool seenIhdr = false;
    while (pos + 12 <= bytes.size()) {
        uint32_t length = ReadU32(bytes, pos);
        if (length > kMaxImageBytes || pos + 12 + (size_t)length > bytes.size()) return false;
        const char* type = (const char*)bytes.data() + pos + 4;
        size_t data = pos + 8;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) return false;
            png.width = ReadU32(bytes, data);
            png.height = ReadU32(bytes, data + 4);
            png.bitDepth = bytes[data + 8];
            png.colorType = bytes[data + 9];
            png.compression = bytes[data + 10];
            png.filter = bytes[data + 11];
            png.interlace = bytes[data + 12];
            seenIhdr = png.width > 0 && png.height > 0;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            png.palette.assign((const char*)bytes.data() + data, length);
        } else if (memcmp(type, "tRNS", 4) == 0) {
            png.transparency.assign(bytes.begin() + data, bytes.begin() + data + length);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (png.idat.size() + length > kMaxImageBytes) return false;
            png.idat.append((const char*)bytes.data() + data, length);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }

        pos += 12 + (size_t)length;
    }

    return seenIhdr && !png.idat.empty() && png.compression == 0 && png.filter == 0 && png.width <= 20000 && png.height <= 20000;
}

static bool BuildFastPngImage(const PngInfo& png, PdfImage& image) {
    if (png.interlace != 0 || !png.transparency.empty()) return false;

    int colors = 0;
    std::string colorSpace;
    if (png.colorType == 0) {
        colors = 1;
        colorSpace = "/DeviceGray";
    } else if (png.colorType == 2 && png.bitDepth == 8) {
        colors = 3;
        colorSpace = "/DeviceRGB";
    } else if (png.colorType == 3 && !png.palette.empty() &&
        (png.bitDepth == 1 || png.bitDepth == 2 || png.bitDepth == 4 || png.bitDepth == 8)) {
        int entries = (int)(png.palette.size() / 3);
        if (entries <= 0 || entries > 256) return false;
        colors = 1;
        colorSpace.reserve(png.palette.size() * 2 + 48);
        colorSpace += "[/Indexed /DeviceRGB ";
        AppendInt(colorSpace, entries - 1);
        colorSpace += " ";
        colorSpace += HexBytes(png.palette);
        colorSpace += "]";
    } else {
        return false;
    }

    image.width = (int)png.width;
    image.height = (int)png.height;
    image.bitsPerComponent = png.bitDepth;
    image.colorSpace = std::move(colorSpace);
    image.filter = "/FlateDecode";
    image.decodeParms.reserve(96);
    image.decodeParms += "<< /Predictor 15 /Colors ";
    AppendInt(image.decodeParms, colors);
    image.decodeParms += " /BitsPerComponent ";
    AppendInt(image.decodeParms, png.bitDepth);
    image.decodeParms += " /Columns ";
    AppendInt(image.decodeParms, (int)png.width);
    image.decodeParms += " >>";
    image.stream = png.idat;
    return true;
}

static uint8_t PngPaeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = std::abs(p - (int)a);
    int pb = std::abs(p - (int)b);
    int pc = std::abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static bool PngUnfilter(const std::vector<uint8_t>& filtered, size_t width, size_t height,
    size_t rowBytes, size_t bpp, std::vector<uint8_t>& rows) {
    if (height == 0 || rowBytes == 0 || bpp == 0) return false;
    if (filtered.size() != (rowBytes + 1) * height) return false;

    rows.assign(rowBytes * height, 0);
    for (size_t y = 0; y < height; y++) {
        const uint8_t* src = filtered.data() + y * (rowBytes + 1);
        uint8_t filter = src[0];
        const uint8_t* raw = src + 1;
        uint8_t* cur = rows.data() + y * rowBytes;
        const uint8_t* prev = y == 0 ? nullptr : rows.data() + (y - 1) * rowBytes;

        for (size_t x = 0; x < rowBytes; x++) {
            uint8_t left = x >= bpp ? cur[x - bpp] : 0;
            uint8_t up = prev ? prev[x] : 0;
            uint8_t upLeft = (prev && x >= bpp) ? prev[x - bpp] : 0;
            uint8_t value = raw[x];
            switch (filter) {
            case 0: break;
            case 1: value = (uint8_t)(value + left); break;
            case 2: value = (uint8_t)(value + up); break;
            case 3: value = (uint8_t)(value + (uint8_t)(((int)left + (int)up) >> 1)); break;
            case 4: value = (uint8_t)(value + PngPaeth(left, up, upLeft)); break;
            default: return false;
            }
            cur[x] = value;
        }
    }
    return true;
}

#ifdef RAYOMD_USE_ZLIB
static bool InflatePngRows(const PngInfo& png, size_t rowBytes, std::vector<uint8_t>& filtered) {
    if (png.height == 0 || rowBytes == 0) return false;
    size_t expected = (rowBytes + 1) * (size_t)png.height;
    if (expected > kMaxDecodedImageBytes) return false;
    filtered.assign(expected, 0);
    uLongf destLen = (uLongf)filtered.size();
    int result = uncompress(filtered.data(), &destLen,
        reinterpret_cast<const Bytef*>(png.idat.data()), (uLong)png.idat.size());
    if (result != Z_OK || destLen != filtered.size()) return false;
    return true;
}

static uint64_t PngPredictorCost(const uint8_t* row, const uint8_t* prev, size_t rowBytes,
    size_t bpp, int filter) {
    uint64_t cost = 0;
    for (size_t x = 0; x < rowBytes; x++) {
        uint8_t left = x >= bpp ? row[x - bpp] : 0;
        uint8_t up = prev ? prev[x] : 0;
        uint8_t upLeft = (prev && x >= bpp) ? prev[x - bpp] : 0;
        uint8_t predictor = 0;
        switch (filter) {
        case 1: predictor = left; break;
        case 2: predictor = up; break;
        case 3: predictor = (uint8_t)(((int)left + (int)up) >> 1); break;
        case 4: predictor = PngPaeth(left, up, upLeft); break;
        default: predictor = 0; break;
        }
        uint8_t residual = (uint8_t)(row[x] - predictor);
        cost += residual < 128 ? residual : 256 - residual;
    }
    return cost;
}

static bool BuildPngPredictorRows(const std::string& raw, size_t width, size_t height,
    size_t components, std::string& predicted) {
    if (width == 0 || height == 0 || components == 0) return false;
    size_t rowBytes = width * components;
    if (rowBytes == 0 || raw.size() != rowBytes * height) return false;

    predicted.clear();
    predicted.resize((rowBytes + 1) * height);
    for (size_t y = 0; y < height; y++) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(raw.data()) + y * rowBytes;
        const uint8_t* prev = y ? reinterpret_cast<const uint8_t*>(raw.data()) + (y - 1) * rowBytes : nullptr;
        uint8_t* out = reinterpret_cast<uint8_t*>(&predicted[0]) + y * (rowBytes + 1);

        int bestFilter = 0;
        uint64_t bestCost = PngPredictorCost(row, prev, rowBytes, components, 0);
        for (int filter = 1; filter <= 4; filter++) {
            uint64_t cost = PngPredictorCost(row, prev, rowBytes, components, filter);
            if (cost < bestCost) {
                bestCost = cost;
                bestFilter = filter;
            }
        }

        out[0] = (uint8_t)bestFilter;
        for (size_t x = 0; x < rowBytes; x++) {
            uint8_t left = x >= components ? row[x - components] : 0;
            uint8_t up = prev ? prev[x] : 0;
            uint8_t upLeft = (prev && x >= components) ? prev[x - components] : 0;
            uint8_t predictor = 0;
            switch (bestFilter) {
            case 1: predictor = left; break;
            case 2: predictor = up; break;
            case 3: predictor = (uint8_t)(((int)left + (int)up) >> 1); break;
            case 4: predictor = PngPaeth(left, up, upLeft); break;
            default: predictor = 0; break;
            }
            out[x + 1] = (uint8_t)(row[x] - predictor);
        }
    }
    return true;
}

static bool CompressFlate(const std::string& input, std::string& output) {
    if (input.empty()) return false;
    uLong sourceLen = (uLong)input.size();
    uLongf destLen = compressBound(sourceLen);
    output.assign((size_t)destLen, '\0');
    int result = compress2(reinterpret_cast<Bytef*>(&output[0]), &destLen,
        reinterpret_cast<const Bytef*>(input.data()), sourceLen, Z_BEST_SPEED);
    if (result != Z_OK) return false;
    output.resize((size_t)destLen);
    return true;
}

static std::string BuildPngPredictorDecodeParms(size_t columns, size_t components) {
    std::string parms;
    parms.reserve(80);
    parms += "<< /Predictor 15 /Colors ";
    AppendSize(parms, components);
    parms += " /BitsPerComponent 8 /Columns ";
    AppendSize(parms, columns);
    parms += " >>";
    return parms;
}

static bool BuildDecodedPngImageWithZlib(const PngInfo& png, PdfImage& image) {
    if (png.interlace != 0 || png.bitDepth != 8) return false;
    if (png.width == 0 || png.height == 0) return false;

    size_t rowBytes = 0;
    size_t bpp = 0;
    if (png.colorType == 6) {
        rowBytes = (size_t)png.width * 4;
        bpp = 4;
    } else if (png.colorType == 4) {
        rowBytes = (size_t)png.width * 2;
        bpp = 2;
    } else if (png.colorType == 3 && !png.palette.empty()) {
        rowBytes = (size_t)png.width;
        bpp = 1;
    } else if (png.colorType == 2 && png.transparency.size() >= 6) {
        rowBytes = (size_t)png.width * 3;
        bpp = 3;
    } else if (png.colorType == 0 && png.transparency.size() >= 2) {
        rowBytes = (size_t)png.width;
        bpp = 1;
    } else {
        return false;
    }

    std::vector<uint8_t> filtered;
    if (!InflatePngRows(png, rowBytes, filtered)) return false;
    std::vector<uint8_t> rows;
    if (!PngUnfilter(filtered, png.width, png.height, rowBytes, bpp, rows)) return false;

    size_t pixels = (size_t)png.width * (size_t)png.height;
    if (pixels == 0 || pixels * 4 > kMaxDecodedImageBytes) return false;

    std::string color;
    std::string alpha;
    bool hasAlpha = false;

    if (png.colorType == 6) {
        color.reserve(pixels * 3);
        alpha.reserve(pixels);
        for (size_t i = 0; i < pixels; i++) {
            uint8_t r = rows[i * 4 + 0];
            uint8_t g = rows[i * 4 + 1];
            uint8_t b = rows[i * 4 + 2];
            uint8_t a = rows[i * 4 + 3];
            color.push_back((char)r);
            color.push_back((char)g);
            color.push_back((char)b);
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }
        image.colorSpace = "/DeviceRGB";
    } else if (png.colorType == 4) {
        color.reserve(pixels);
        alpha.reserve(pixels);
        for (size_t i = 0; i < pixels; i++) {
            uint8_t gray = rows[i * 2 + 0];
            uint8_t a = rows[i * 2 + 1];
            color.push_back((char)gray);
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }
        image.colorSpace = "/DeviceGray";
    } else if (png.colorType == 3) {
        size_t entries = png.palette.size() / 3;
        color.reserve(pixels * 3);
        alpha.reserve(pixels);
        for (size_t i = 0; i < pixels; i++) {
            uint8_t index = rows[i];
            if (index >= entries) return false;
            color.push_back(png.palette[index * 3 + 0]);
            color.push_back(png.palette[index * 3 + 1]);
            color.push_back(png.palette[index * 3 + 2]);
            uint8_t a = index < png.transparency.size() ? png.transparency[index] : 255;
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }
        image.colorSpace = "/DeviceRGB";
    } else if (png.colorType == 2) {
        uint16_t tr = (uint16_t)((png.transparency[0] << 8) | png.transparency[1]);
        uint16_t tg = (uint16_t)((png.transparency[2] << 8) | png.transparency[3]);
        uint16_t tb = (uint16_t)((png.transparency[4] << 8) | png.transparency[5]);
        color.reserve(pixels * 3);
        alpha.reserve(pixels);
        for (size_t i = 0; i < pixels; i++) {
            uint8_t r = rows[i * 3 + 0];
            uint8_t g = rows[i * 3 + 1];
            uint8_t b = rows[i * 3 + 2];
            color.push_back((char)r);
            color.push_back((char)g);
            color.push_back((char)b);
            uint8_t a = (r == tr && g == tg && b == tb) ? 0 : 255;
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }
        image.colorSpace = "/DeviceRGB";
    } else {
        uint16_t transparent = (uint16_t)((png.transparency[0] << 8) | png.transparency[1]);
        color.reserve(pixels);
        alpha.reserve(pixels);
        for (size_t i = 0; i < pixels; i++) {
            uint8_t gray = rows[i];
            color.push_back((char)gray);
            uint8_t a = gray == transparent ? 0 : 255;
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }
        image.colorSpace = "/DeviceGray";
    }

    image.width = (int)png.width;
    image.height = (int)png.height;
    image.bitsPerComponent = 8;

    size_t colorComponents = image.colorSpace == "/DeviceRGB" ? 3 : 1;
    std::string predictedColor;
    std::string compressedColor;
    if (!BuildPngPredictorRows(color, png.width, png.height, colorComponents, predictedColor)) return false;
    if (!CompressFlate(predictedColor, compressedColor)) return false;
    image.stream = std::move(compressedColor);
    image.filter = "/FlateDecode";
    image.decodeParms = BuildPngPredictorDecodeParms(png.width, colorComponents);

    if (hasAlpha) {
        std::string predictedAlpha;
        std::string compressedAlpha;
        if (!BuildPngPredictorRows(alpha, png.width, png.height, 1, predictedAlpha)) return false;
        if (!CompressFlate(predictedAlpha, compressedAlpha)) return false;
        image.maskStream = std::move(compressedAlpha);
        image.maskFilter = "/FlateDecode";
        image.maskDecodeParms = BuildPngPredictorDecodeParms(png.width, 1);
    }
    return true;
}
#else
static bool BuildDecodedPngImageWithZlib(const PngInfo&, PdfImage&) {
    return false;
}
#endif

static bool ParsePngImage(const std::vector<uint8_t>& bytes, PdfImage& image);

#ifdef _WIN32
template<typename T>
static void ReleaseCom(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static bool EncodeWicPngBytes(IWICImagingFactory* factory, UINT width, UINT height,
    const WICPixelFormatGUID& format, UINT stride, const std::vector<uint8_t>& pixels,
    std::vector<uint8_t>& pngBytes) {
    if (!factory || width == 0 || height == 0 || pixels.empty()) return false;
    if (pixels.size() > UINT32_MAX) return false;

    IWICBitmap* bitmap = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HGLOBAL global = nullptr;

    HRESULT hr = factory->CreateBitmapFromMemory(width, height, format, stride,
        (UINT)pixels.size(), (BYTE*)pixels.data(), &bitmap);
    if (SUCCEEDED(hr)) {
        hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(width, height);
    }
    WICPixelFormatGUID frameFormat = format;
    if (SUCCEEDED(hr)) {
        hr = frame->SetPixelFormat(&frameFormat);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WriteSource(bitmap, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = GetHGlobalFromStream(stream, &global);
    }

    bool ok = false;
    if (SUCCEEDED(hr) && global) {
        SIZE_T size = GlobalSize(global);
        void* data = GlobalLock(global);
        if (data && size > 0 && size <= kMaxImageBytes) {
            pngBytes.assign((uint8_t*)data, (uint8_t*)data + size);
            ok = true;
        }
        if (data) GlobalUnlock(global);
    }

    ReleaseCom(frame);
    ReleaseCom(encoder);
    ReleaseCom(stream);
    ReleaseCom(bitmap);
    return ok;
}

static bool TryCompressWicDecodedImage(IWICImagingFactory* factory, UINT width, UINT height,
    const std::string& rgb, const std::string& alpha, bool hasAlpha, PdfImage& image) {
    if (!factory || width == 0 || height == 0 || rgb.empty()) return false;
    if (rgb.size() > UINT32_MAX || alpha.size() > UINT32_MAX) return false;

    std::vector<uint8_t> rgbBytes(rgb.begin(), rgb.end());
    std::vector<uint8_t> rgbPng;
    PdfImage compressedColor;
    if (!EncodeWicPngBytes(factory, width, height, GUID_WICPixelFormat24bppRGB,
        width * 3, rgbBytes, rgbPng)) {
        return false;
    }
    if (!ParsePngImage(rgbPng, compressedColor) || compressedColor.maskStream.size() != 0) return false;

    image.width = (int)width;
    image.height = (int)height;
    image.bitsPerComponent = compressedColor.bitsPerComponent;
    image.colorSpace = compressedColor.colorSpace;
    image.filter = compressedColor.filter;
    image.decodeParms = compressedColor.decodeParms;
    image.stream = std::move(compressedColor.stream);

    if (hasAlpha) {
        std::vector<uint8_t> alphaBytes(alpha.begin(), alpha.end());
        std::vector<uint8_t> alphaPng;
        PdfImage compressedMask;
        if (!EncodeWicPngBytes(factory, width, height, GUID_WICPixelFormat8bppGray,
            width, alphaBytes, alphaPng)) {
            return false;
        }
        if (!ParsePngImage(alphaPng, compressedMask) || compressedMask.maskStream.size() != 0) return false;
        image.maskStream = std::move(compressedMask.stream);
        image.maskFilter = compressedMask.filter;
        image.maskDecodeParms = compressedMask.decodeParms;
    }

    return true;
}

static bool BuildDecodedImageWithWic(const std::vector<uint8_t>& bytes, PdfImage& image) {
    if (bytes.empty() || bytes.size() > kMaxImageBytes) return false;

    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninitialize = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size());
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);

    std::vector<uint8_t> rgba;
    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        size_t stride = (size_t)width * 4;
        size_t total = stride * (size_t)height;
        if (total > kMaxDecodedImageBytes) {
            hr = E_FAIL;
        } else {
            rgba.assign(total, 0);
            hr = converter->CopyPixels(nullptr, (UINT)stride, (UINT)rgba.size(), rgba.data());
        }
    }

    bool ok = false;
    if (SUCCEEDED(hr) && !rgba.empty()) {
        size_t pixels = (size_t)width * (size_t)height;
        std::string rgb;
        std::string alpha;
        rgb.reserve(pixels * 3);
        alpha.reserve(pixels);
        bool hasAlpha = false;
        for (size_t i = 0; i < pixels; i++) {
            uint8_t r = rgba[i * 4 + 0];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t b = rgba[i * 4 + 2];
            uint8_t a = rgba[i * 4 + 3];
            rgb.push_back((char)r);
            rgb.push_back((char)g);
            rgb.push_back((char)b);
            alpha.push_back((char)a);
            hasAlpha = hasAlpha || a != 255;
        }

        if (!TryCompressWicDecodedImage(factory, width, height, rgb, alpha, hasAlpha, image)) {
            image.width = (int)width;
            image.height = (int)height;
            image.bitsPerComponent = 8;
            image.colorSpace = "/DeviceRGB";
            image.stream = std::move(rgb);
            if (hasAlpha) image.maskStream = std::move(alpha);
        }
        ok = true;
    }

    ReleaseCom(converter);
    ReleaseCom(frame);
    ReleaseCom(decoder);
    ReleaseCom(stream);
    ReleaseCom(factory);
    if (uninitialize) CoUninitialize();
    return ok;
}
#else
static bool BuildDecodedImageWithWic(const std::vector<uint8_t>&, PdfImage&) {
    return false;
}
#endif

static bool ParsePngImage(const std::vector<uint8_t>& bytes, PdfImage& image) {
    PngInfo png;
    if (!ParsePngChunks(bytes, png)) return false;
    if (BuildFastPngImage(png, image)) return true;
    if (BuildDecodedPngImageWithZlib(png, image)) return true;
    return BuildDecodedImageWithWic(bytes, image);
}

static bool DecodeImageBytes(const std::vector<uint8_t>& bytes, PdfImage& image) {
    PdfImage parsed;
    if (ParseJpegImage(bytes, parsed) || ParsePngImage(bytes, parsed) || BuildDecodedImageWithWic(bytes, parsed)) {
        image = std::move(parsed);
        return image.width > 0 && image.height > 0 && !image.stream.empty();
    }
    return false;
}

class ImageRegistry {
public:
    explicit ImageRegistry(const PdfOptions& opts) : options(opts), localPolicy(opts) {}

    bool Resolve(const std::string& src, const std::string& alt, int& index) {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Image);
        // Reuse raw sources only after this registry has accepted them through
        // its immutable policy; cross-build caches stay canonical-keyed.
        std::string sourceKey = "source:";
        sourceKey += src;
        auto source = indexByKey.find(sourceKey);
        if (source != indexByKey.end()) {
            index = source->second;
            return true;
        }

        std::string key;
        std::string localPathUtf8;
#ifdef _WIN32
        WinLocalImageFile localFile;
        bool directLocal = false;
#endif
        bool isUrl = IsHttpUrl(src);
        if (isUrl) {
            if (!options.enableUrlImages) return false;
            key = "url:" + src;
        } else {
#ifdef _WIN32
            DirectLocalImageResult direct = localPolicy.TryOpenDirect(src, key, localFile);
            if (direct == DirectLocalImageResult::Rejected) return false;
            directLocal = direct == DirectLocalImageResult::Opened;
            if (!directLocal && !localPolicy.Resolve(src, key, localPathUtf8)) return false;
#else
            if (!localPolicy.Resolve(src, key, localPathUtf8)) return false;
#endif
        }

        auto local = indexByKey.find(key);
        if (local != indexByKey.end()) {
            index = local->second;
            indexByKey.emplace(std::move(sourceKey), index);
            return true;
        }

        SharedPdfImage sharedImage = LoadDecodedImageFromCache(key);
        if (!sharedImage && IsKnownFailure(key)) return false;
        if (!sharedImage) {
            PdfImage image;
            std::vector<uint8_t> bytes;
            bool loaded = false;
            if (isUrl) {
                loaded = FetchUrlBytes(src, options, bytes);
            }
#ifdef _WIN32
            else if (directLocal) {
                loaded = ReadLocalImageHandle(localFile, bytes);
            }
#endif
            else {
                loaded = ReadLocalImageFile(localPathUtf8, bytes) && bytes.size() <= kMaxImageBytes;
            }
            if (!loaded || !DecodeImageBytes(bytes, image)) {
                StoreFailure(key);
                return false;
            }
            sharedImage = StoreDecodedImageInCache(key, std::move(image));
        }

        index = (int)images.size();
        indexByKey[key] = index;
        indexByKey.emplace(std::move(sourceKey), index);
        images.push_back(std::move(sharedImage));
        (void)alt;
        return true;
    }

    const PdfImage& Get(int index) const {
        return *images[(size_t)index];
    }

    const std::vector<SharedPdfImage>& Images() const { return images; }

private:
    const PdfOptions& options;
    LocalImagePolicy localPolicy;
    std::vector<SharedPdfImage> images;
    std::unordered_map<std::string, int> indexByKey;
    static std::unordered_map<std::string, SharedPdfImage>& Cache() {
        static std::unordered_map<std::string, SharedPdfImage> cache;
        return cache;
    }

    static std::mutex& CacheMutex() {
        static std::mutex mutex;
        return mutex;
    }

    static size_t& CacheBytes() {
        static size_t bytes = 0;
        return bytes;
    }

    static std::unordered_map<std::string, bool>& FailureCache() {
        static std::unordered_map<std::string, bool> failures;
        return failures;
    }

    static size_t ImageCacheCost(const PdfImage& image) {
        return image.stream.size() + image.maskStream.size() + image.colorSpace.size() +
            image.filter.size() + image.decodeParms.size() + 128;
    }

    static SharedPdfImage LoadDecodedImageFromCache(const std::string& key) {
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto& cache = Cache();
        auto it = cache.find(key);
        return it == cache.end() ? SharedPdfImage{} : it->second;
    }

    static SharedPdfImage StoreDecodedImageInCache(const std::string& key, PdfImage image) {
        size_t cost = ImageCacheCost(image);
        SharedPdfImage candidate = std::make_shared<PdfImage>(std::move(image));
        if (cost > kMaxImageBytes) return candidate;

        std::lock_guard<std::mutex> lock(CacheMutex());
        auto& cache = Cache();
        if (cache.find(key) != cache.end()) return candidate;
        size_t& cacheBytes = CacheBytes();
        if (cacheBytes + cost > 64u * 1024u * 1024u) {
            cache.clear();
            cacheBytes = 0;
        }
        cacheBytes += cost;
        cache.emplace(key, candidate);
        return candidate;
    }

    static bool IsKnownFailure(const std::string& key) {
        if (key.empty()) return false;
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto& failures = FailureCache();
        return failures.find(key) != failures.end();
    }

    static void StoreFailure(const std::string& key) {
        if (key.empty() || key.size() > 4096) return;
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto& failures = FailureCache();
        if (failures.size() >= 256) failures.clear();
        failures[key] = true;
    }
};

static std::string BuildImageStreamDict(const PdfImage& image, int smaskId) {
    std::string dict;
    dict.reserve(256 + image.colorSpace.size() + image.filter.size() + image.decodeParms.size());
    dict += "/Type /XObject /Subtype /Image /Width ";
    AppendInt(dict, image.width);
    dict += " /Height ";
    AppendInt(dict, image.height);
    dict += " /ColorSpace ";
    dict += image.colorSpace;
    dict += " /BitsPerComponent ";
    AppendInt(dict, image.bitsPerComponent);
    if (!image.filter.empty()) {
        dict += " /Filter ";
        dict += image.filter;
    }
    if (!image.decodeParms.empty()) {
        dict += " /DecodeParms ";
        dict += image.decodeParms;
    }
    if (smaskId > 0) {
        dict += " /SMask ";
        AppendInt(dict, smaskId);
        dict += " 0 R";
    }
    return dict;
}

static std::string BuildMaskStreamDict(const PdfImage& image) {
    std::string dict;
    dict.reserve(128);
    dict += "/Type /XObject /Subtype /Image /Width ";
    AppendInt(dict, image.width);
    dict += " /Height ";
    AppendInt(dict, image.height);
    dict += " /ColorSpace /DeviceGray /BitsPerComponent 8";
    if (!image.maskFilter.empty()) {
        dict += " /Filter ";
        dict += image.maskFilter;
    }
    if (!image.maskDecodeParms.empty()) {
        dict += " /DecodeParms ";
        dict += image.maskDecodeParms;
    }
    return dict;
}

static std::vector<int> AddImageObjects(PdfObjects& pdf, const std::vector<SharedPdfImage>& images) {
    std::vector<int> ids;
    ids.reserve(images.size());
    for (const SharedPdfImage& shared : images) {
        const PdfImage& image = *shared;
        int smaskId = 0;
        if (!image.maskStream.empty()) {
            smaskId = pdf.AddStreamView(BuildMaskStreamDict(image), image.maskStream);
        }
        ids.push_back(pdf.AddStreamView(BuildImageStreamDict(image, smaskId), image.stream));
    }
    return ids;
}

static void AppendXObjectResources(std::string& page, const std::vector<int>& imageObjectIds) {
    if (imageObjectIds.empty()) return;
    page += " /XObject << ";
    for (size_t i = 0; i < imageObjectIds.size(); i++) {
        page += "/Im";
        AppendSize(page, i + 1);
        page += " ";
        AppendInt(page, imageObjectIds[i]);
        page += " 0 R ";
    }
    page += ">>";
}

static std::vector<std::vector<int>> AddLinkAnnotationObjects(PdfObjects& pdf,
    const std::vector<std::vector<LinkRect>>& linksByPage) {
    std::vector<std::vector<int>> idsByPage;
    idsByPage.reserve(linksByPage.size());
    for (const auto& pageLinks : linksByPage) {
        std::vector<int> ids;
        ids.reserve(pageLinks.size());
        for (const LinkRect& link : pageLinks) {
            if (link.url.empty() || link.x2 <= link.x1 || link.y2 <= link.y1) continue;
            std::string annot;
            annot.reserve(link.url.size() + 192);
            annot += "<< /Type /Annot /Subtype /Link /Rect [";
            AppendF(annot, link.x1);
            annot += " ";
            AppendF(annot, link.y1);
            annot += " ";
            AppendF(annot, link.x2);
            annot += " ";
            AppendF(annot, link.y2);
            annot += "] /Border [0 0 0] /A << /S /URI /URI (";
            annot += EscapeLiteral(link.url);
            annot += ") >> >>";
            ids.push_back(pdf.Add(std::move(annot)));
        }
        idsByPage.push_back(std::move(ids));
    }
    return idsByPage;
}

static void AppendPageAnnotations(std::string& page, const std::vector<int>& annotationIds) {
    if (annotationIds.empty()) return;
    page += " /Annots [";
    for (int id : annotationIds) {
        AppendInt(page, id);
        page += " 0 R ";
    }
    page += "]";
}

// ---- Native math integration (shared by both renderers) ----------------------

// Code that runs only for documents with formulas: optimise it for size and keep it
// away from the hot text path.
#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_MATH_COLD __attribute__((cold, noinline))
#else
#define RAYOMD_MATH_COLD
#endif


using Internal::MathFallbackFont;
using Internal::MathFormula;

// Formula-bearing text builds its span, word and line lists with this helper only. It must
// not share vector code (appending a whole element, reserve) or the small span helpers
// with the plain text path: every extra call site there stops the compiler from inlining
// them into the hot wrap functions, which costs a few percent on documents without math.
template <typename T>
RAYOMD_MATH_COLD static T& MathAppend(std::vector<T>& values) {
    values.emplace_back();
    return values.back();
}

constexpr double kMathSizeFactor = 1.08;      // Times at the text size looks small next to Helvetica / Segoe UI
constexpr double kMathLineTolerance = 0.10;   // a line grows only when a formula leaves its box by more than this (x text size)
constexpr double kMathFallbackAscent = 0.74;  // ink extents of fallback text, in em of the text size
constexpr double kMathFallbackDescent = 0.21;
constexpr double kDisplayMathAbove = 5.0;    // block display math: space above the formula
constexpr double kDisplayMathBelow = 9.0;    // and below it
constexpr double kDisplayMathLinePad = 3.0;  // $$...$$ inside a paragraph: extra room above and below
constexpr double kQuoteMathPad = 4.0;        // display math inside a block quote strip

// Extra room a line needs beyond today's fixed line box. Zero for every line
// without a formula and for formulas that fit the normal line box.
struct MathLineExtent {
    double above = 0.0;
    double below = 0.0;
};

// Lays out one formula at the text size `size` so that it fits maxWidth x maxHeight; the
// module shrinks it (re-layout to 75 %, then a uniform scale, never below 50 % overall).
// False: the formula is empty, hit a hard limit, or does not fit even then, and the caller
// shows the complete TeX source instead.
RAYOMD_MATH_COLD static bool LayoutMathToFit(std::string_view tex, double size, bool display, bool bold,
    double maxWidth, double maxHeight, const MathFallbackFont* fallback, MathFormula& formula) {
    formula = MathFormula::Layout(tex, size * kMathSizeFactor, display, fallback, maxWidth, bold, maxHeight);
    return !formula.Empty() && !formula.SourceFallback() && formula.Width() <= maxWidth &&
        formula.Ascent() + formula.Descent() <= maxHeight;
}

// The formulas of the paragraph, heading, or table row that is being laid out.
// Wrapped lines refer to them by index, so copying a line never copies a formula.
class MathPool {
public:
    bool Empty() const { return items.empty(); }
    // True when the text being laid out contains a formula or the source of one that did
    // not fit: such text takes the math wrap and paint functions.
    bool Active() const { return !items.empty() || sourceShown; }
    void Clear() { items.clear(); sourceShown = false; }
    bool Used() const { return used; }
    void MarkSourceShown() { sourceShown = true; }

    int Add(std::string_view tex, double size, bool display, bool bold, double maxWidth, double maxHeight,
        const MathFallbackFont* fallback) {
        Item item;
        item.display = display;
        if (!LayoutMathToFit(tex, size, display, bold, maxWidth, maxHeight, fallback, item.formula)) return -1;
        items.push_back(std::move(item));
        return (int)items.size() - 1;
    }

    const MathFormula& At(int index) const { return items[(size_t)index].formula; }
    bool IsDisplay(int index) const { return items[(size_t)index].display; }

    double Emit(int index, std::string& content, double x, double baseline, const char* rgb) {
        used = true;
        items[(size_t)index].formula.Emit(content, x, baseline, rgb);
        return items[(size_t)index].formula.Width();
    }

    void MarkUsed() { used = true; }

private:
    struct Item {
        MathFormula formula;
        bool display = false;
    };
    std::vector<Item> items;
    bool used = false;
    bool sourceShown = false;
};

template <typename SpanType>
static MathLineExtent MeasureMathLine(const MathPool& math, const std::vector<SpanType>& line,
    double size, double lineHeight) {
    MathLineExtent extent;
    for (const SpanType& span : line) {
        if (span.math < 0) continue;
        const MathFormula& formula = math.At(span.math);
        const double over = formula.Ascent() - size;
        const double under = formula.Descent() - (lineHeight - size);
        if (math.IsDisplay(span.math)) {
            extent.above = std::max(extent.above, over + kDisplayMathLinePad);
            extent.below = std::max(extent.below, under + kDisplayMathLinePad);
            continue;
        }
        // An inline formula that leaves the line box only slightly does not move the lines.
        const double tolerance = kMathLineTolerance * size;
        if (over > tolerance) extent.above = std::max(extent.above, over);
        if (under > tolerance) extent.below = std::max(extent.below, under);
    }
    return extent;
}

// True when the line is a single $$...$$ formula inside paragraph text.
template <typename SpanType>
static bool IsDisplayMathLine(const MathPool& math, const std::vector<SpanType>& line) {
    return line.size() == 1 && line[0].math >= 0 && math.IsDisplay(line[0].math);
}

// Source whitespace decides whether a formula touches its neighbours: "($x$)" and
// "$n$-th" stay tight, "a $x$ b" keeps its spaces. joins[i] != 0 means: no space
// and no line break between word i and word i - 1. Words that are not next to a
// formula get 0 and therefore today's spacing.
template <typename SpanType>
RAYOMD_MATH_COLD static std::vector<unsigned char> MarkMathJoins(const std::vector<SpanType>& spans, size_t wordCount) {
    std::vector<unsigned char> joins(wordCount, 0);
    size_t wordIndex = 0;
    bool gap = true;
    bool afterMath = false;
    for (const SpanType& span : spans) {
        if (span.math >= 0) {
            if (wordIndex < wordCount) joins[wordIndex] = !gap && wordIndex > 0;
            wordIndex++;
            gap = false;
            afterMath = true;
            continue;
        }
        bool inWord = false;
        for (auto ch : span.text) {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                gap = true;
                inWord = false;
                continue;
            }
            if (inWord) continue;
            if (afterMath && !gap && wordIndex < wordCount) joins[wordIndex] = 1;
            inWord = true;
            afterMath = false;
            gap = false;
            wordIndex++;
        }
    }
    return joins;
}

RAYOMD_MATH_COLD static std::string MathSourceText(const Internal::InlineSpan& span) {
    const char* delimiter = span.math == Internal::InlineMath::Display ? "$$" : "$";
    return delimiter + span.text + delimiter;
}

RAYOMD_MATH_COLD static std::vector<int> AddMathFontObjects(PdfObjects& pdf) {
    std::vector<int> ids;
    ids.reserve(Internal::kMathFontCount);
    int descriptorId = pdf.Add(Internal::MathSymbolDescriptorObject());
    for (int index = 0; index < Internal::kMathFontCount; index++) {
        ids.push_back(pdf.Add(Internal::MathFontObject(index, descriptorId)));
    }
    return ids;
}

RAYOMD_MATH_COLD static void AppendMathFontResources(std::string& page, const std::vector<int>& mathFontIds) {
    for (size_t i = 0; i < mathFontIds.size(); i++) {
        page += " /";
        page += Internal::kMathFonts[i].resourceName;
        page += " ";
        AppendInt(page, mathFontIds[i]);
        page += " 0 R";
    }
}

// A table that contains formulas. Rows are as tall as their tallest cell; cells
// without a formula wrap exactly as they do in RenderTable.
template <typename RendererType>
RAYOMD_MATH_COLD static void RenderMathTable(RendererType& renderer, const Block& block) {
    const std::vector<std::vector<std::string>>& rows = block.rows;
    if (rows.empty()) return;
    size_t columns = 0;
    for (const auto& row : rows) columns = std::max(columns, row.size());
    if (columns == 0) return;

    double tableWidth = PAGE_W - renderer.margin * 2.0;
    double colWidth = tableWidth / columns;
    double size = 9.6;
    double lh = size * 1.32;
    double pad = 5.0;
    double cellTextWidth = std::max(16.0, colWidth - pad * 2.0);
    static const std::string emptyCell;

    renderer.y -= 3.0;
    for (size_t r = 0; r < rows.size(); r++) {
        renderer.math.Clear();
        std::vector<decltype(renderer.WrapMathCell(emptyCell, 0.0, 0.0, false))> wrapped(columns);
        double contentHeight = lh;
        for (size_t c = 0; c < columns; c++) {
            const std::string& cell = c < rows[r].size() ? rows[r][c] : emptyCell;
            wrapped[c] = renderer.WrapMathCell(cell, cellTextWidth, size, r == 0);
            double height = 0.0;
            for (const auto& line : wrapped[c]) {
                MathLineExtent extent = MeasureMathLine(renderer.math, line, size, lh);
                height += lh + extent.above + extent.below;
            }
            contentHeight = std::max(contentHeight, height);
        }

        double rowHeight = contentHeight + pad * 2.0;
        renderer.Ensure(rowHeight + 5.0);
        double top = renderer.y;
        if (r == 0) renderer.TableFill(renderer.margin, top, tableWidth, rowHeight);
        for (size_t c = 0; c < columns; c++) {
            double cellX = renderer.margin + c * colWidth;
            renderer.TableStroke(cellX, top, colWidth, rowHeight);
            int align = c < block.aligns.size() ? block.aligns[c] : -1;
            double lineTop = top - pad;
            for (const auto& line : wrapped[c]) {
                MathLineExtent extent = MeasureMathLine(renderer.math, line, size, lh);
                double lineWidth = renderer.MathLineWidth(line, size, r == 0);
                double tx = cellX + pad;
                if (align == 0) tx = cellX + (colWidth - lineWidth) * 0.5;
                else if (align == 1) tx = cellX + colWidth - pad - lineWidth;
                renderer.PaintMathTextLine(line, tx, lineTop - extent.above - size, size,
                    renderer.TableTextColor(r == 0), r == 0);
                lineTop -= lh + extent.above + extent.below;
            }
        }
        renderer.y -= rowHeight;
    }
    renderer.y -= 9.0;
}

template <typename RendererType>
static void RenderBlocks(RendererType& renderer, const std::vector<Block>& blocks) {
    if (blocks.empty()) {
        renderer.RenderParagraph("Empty document");
        return;
    }
    bool pageBreakPending = false;
    for (const Block& block : blocks) {
        if (block.type == BlockType::PageBreak) { pageBreakPending = true; continue; }
        if (pageBreakPending) { renderer.RenderPageBreak(); pageBreakPending = false; }
        switch (block.type) {
        case BlockType::Heading: renderer.RenderHeading(block); break;
        case BlockType::Paragraph: renderer.RenderParagraph(block.text); break;
        case BlockType::Bullet: renderer.RenderBullet(block); break;
        case BlockType::Numbered: renderer.RenderNumbered(block); break;
        case BlockType::Quote: renderer.RenderQuote(block); break;
        case BlockType::Code: renderer.RenderCode(block.text); break;
        case BlockType::MathBlock: renderer.RenderMath(block.text); break;
        case BlockType::Table:
            if (block.hasMath) RenderMathTable(renderer, block);
            else renderer.RenderTable(block.rows, block.aligns);
            break;
        case BlockType::Rule: renderer.RenderRule(); break;
        case BlockType::PageBreak: break;
        case BlockType::Image: renderer.RenderImage(block); break;
        }
    }
}

class Renderer {
public:
    // The content streams of all pages are appended to `output`, one after another.
    Renderer(std::string& output, const TtfFont& f, int fontObject, PdfStyle styleValue, const PdfMargin& marginValue,
        ImageRegistry* imageRegistry)
        : font(f), fontId(fontObject), images(imageRegistry), style(styleValue), content(output) {
        margin = ResolveMarginPoints(marginValue);
        bodySize = style == PdfStyle::Tech ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        mathFallback = { this, &Renderer::MeasureMathFallback, &Renderer::EmitMathFallback,
            kMathFallbackAscent / kMathSizeFactor, kMathFallbackDescent / kMathSizeFactor };
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) { RenderBlocks(*this, blocks); }

    // Offset in the output buffer at which the content stream of each page starts.
    const std::vector<size_t>& PageStarts() const { return pageStarts; }
    const std::vector<std::vector<LinkRect>>& PageLinks() const { return pageLinks; }
    const CidList& UsedCids() const { return usedCids.Values(); }
    bool MathUsed() const { return math.Used(); }

private:
    template <typename RendererType>
    friend void RenderBlocks(RendererType&, const std::vector<Block>&);
    template <typename RendererType>
    friend void RenderMathTable(RendererType&, const Block&);

    const TtfFont& font;
    int fontId = 0;
    ImageRegistry* images = nullptr;
    PdfStyle style = PdfStyle::Elegant;
    double margin = 62.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    // The page being rendered is the tail of `content`, so painting is a plain append.
    std::string& content;
    std::vector<size_t> pageStarts;
    std::vector<std::vector<LinkRect>> pageLinks;
    UsedCidSet usedCids;
    MathPool math;
    MathFallbackFont mathFallback{};

    // Text the standard math fonts cannot show (\text{...} in another script) is
    // measured and painted with the embedded document font, at the text size the
    // formula stands in (the module works at kMathSizeFactor times that size).
    static double MeasureMathFallback(void* context, std::string_view utf8, double size) {
        const Renderer* self = static_cast<const Renderer*>(context);
        return TextWidth(self->font, Utf8ToWide(utf8), size / kMathSizeFactor);
    }

    static void EmitMathFallback(void* context, std::string& content, std::string_view utf8, double x,
        double baseline, double size, const char* rgb) {
        Renderer* self = static_cast<Renderer*>(context);
        content += "q ";
        content += rgb;
        content += " rg BT /F1 ";
        AppendF(content, size / kMathSizeFactor);
        content += " Tf 1 0 0 1 ";
        AppendF(content, x);
        content += " ";
        AppendF(content, baseline);
        content += " Tm ";
        content += HexText(self->font, Utf8ToWide(utf8), self->usedCids);
        content += " Tj ET Q\n";
    }

    double MaxMathHeight() const {
        return (PAGE_H - margin * 2.0) * 0.5;
    }

    void RenderBullet(const Block& block) {
        RenderListItem("- ", block);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderNumbered(const Block& block) {
        std::string marker;
        marker.reserve(8);
        AppendInt(marker, block.number);
        marker += ". ";
        RenderListItem(marker, block);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderIndentedBlocks(const std::vector<Block>& blocks, double inset) {
        double savedMargin = margin;
        margin += inset;
        RenderBlocks(*this, blocks);
        margin = savedMargin;
    }

    struct StyledSpan {
        std::wstring text;
        std::string url;
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
        int math = -1;   // index into `math` when the span is a formula; text is empty then
    };

    struct StyledWord {
        std::wstring text;
        std::string url;
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
        int math = -1;
    };

    void PushSpan(std::vector<StyledSpan>& spans, std::string_view text, bool bold, bool italic, bool strike,
        std::string&& url, bool code) {
        if (text.empty()) return;
        std::wstring wide = Utf8ToWide(text);
        if (!spans.empty() && spans.back().bold == bold && spans.back().italic == italic &&
            spans.back().strike == strike && spans.back().url == url && spans.back().code == code &&
            spans.back().math < 0) {
            spans.back().text += wide;
        } else {
            spans.emplace_back();
            StyledSpan& span = spans.back();
            span.text = std::move(wide);
            span.url = std::move(url);
            span.bold = bold;
            span.italic = italic;
            span.strike = strike;
            span.code = code;
        }
    }

    // A formula becomes one span that refers to the pool. A formula that is empty
    // or cannot fit the line is shown as its TeX source in code style instead.
    RAYOMD_MATH_COLD void PushMathSpan(std::vector<StyledSpan>& spans, const Internal::InlineSpan& span, double size,
        double width, bool bold) {
        int index = math.Add(span.text, size, span.math == Internal::InlineMath::Display, bold, width,
            MaxMathHeight(), &mathFallback);
        StyledSpan& added = MathAppend(spans);
        if (index < 0) {
            added.text = Utf8ToWide(MathSourceText(span));
            added.code = true;
            math.MarkSourceShown();
            return;
        }
        added.math = index;
    }

    std::vector<StyledSpan> ParseInlineStyled(const std::string& input, double size, double width) {
        std::vector<StyledSpan> spans;
        std::vector<Internal::InlineSpan> inlineSpans = Internal::ParseInlineSpans(input);
        spans.reserve(inlineSpans.size());
        for (Internal::InlineSpan& span : inlineSpans) {
            if (span.math != Internal::InlineMath::None) {
                PushMathSpan(spans, span, size, width, false);
                continue;
            }
            PushSpan(spans, span.text, span.bold, span.italic, span.strike, std::move(span.url), span.code);
        }
        return spans;
    }

    // Heading and table-cell text (Block::hasMath): plain text with formulas between
    // kMathTextOpen and kMathTextClose. No Markdown is interpreted here.
    RAYOMD_MATH_COLD std::vector<StyledSpan> MathTextSpans(const std::string& text, double size, double width, bool bold) {
        std::vector<StyledSpan> spans;
        Internal::ForEachMathTextSegment(text, [&](std::string_view segment, bool isMath) {
            if (!isMath) {
                if (!segment.empty()) MathAppend(spans).text = Utf8ToWide(segment);
                return;
            }
            Internal::InlineSpan span;
            span.text.assign(segment.data(), segment.size());
            span.math = Internal::InlineMath::Inline;
            PushMathSpan(spans, span, size, width, bold);
        });
        return spans;
    }

    // Word list of formula-bearing text: the words WrapStyled walks, plus one word
    // per formula.
    RAYOMD_MATH_COLD std::vector<StyledWord> SplitMathWords(const std::vector<StyledSpan>& spans) {
        std::vector<StyledWord> words;
        for (const StyledSpan& span : spans) {
            if (span.math >= 0) {
                MathAppend(words).math = span.math;
                continue;
            }
            const size_t length = span.text.size();
            for (size_t start = 0; start < length;) {
                while (start < length && IsWideSpace(span.text[start])) start++;
                size_t end = start;
                while (end < length && !IsWideSpace(span.text[end])) end++;
                if (end > start) {
                    StyledWord& word = MathAppend(words);
                    word.text.assign(span.text, start, end - start);
                    word.url = span.url;
                    word.bold = span.bold;
                    word.italic = span.italic;
                    word.strike = span.strike;
                    word.code = span.code;
                }
                start = end;
            }
        }
        return words;
    }

    // Appends one word to the line being filled, with a leading space when `spaced`. It is
    // merged into the previous span when the style is the same.
    void AppendStyledWord(std::vector<StyledSpan>& line, std::wstring_view word, bool spaced, bool bold, bool italic,
        bool strike, const std::string& url, bool code) {
        if (!line.empty()) {
            StyledSpan& last = line.back();
            if (last.bold == bold && last.italic == italic && last.strike == strike && last.url == url &&
                last.code == code) {
                if (spaced) last.text.push_back(L' ');
                last.text.append(word.data(), word.size());
                return;
            }
        }
        line.emplace_back();
        StyledSpan& span = line.back();
        span.text.reserve(word.size() + 1);
        if (spaced) span.text.push_back(L' ');
        span.text.append(word.data(), word.size());
        span.url = url;
        span.bold = bold;
        span.italic = italic;
        span.strike = strike;
        span.code = code;
    }

    bool IsClosingPunctuation(std::wstring_view text) {
        return text.size() == 1 && (text[0] == L'.' || text[0] == L',' || text[0] == L';' ||
            text[0] == L':' || text[0] == L'!' || text[0] == L'?' || text[0] == L')');
    }

    // Line filling for text that contains at least one formula. Same greedy rule as
    // WrapStyled, plus: a formula is one unbreakable word, words glued to a formula
    // (joinPrev) stay on its line, and a $$...$$ formula gets a line of its own.
    RAYOMD_MATH_COLD std::vector<std::vector<StyledSpan>> WrapMathWords(const std::vector<StyledWord>& words,
        const std::vector<unsigned char>& joins, double width, double size) {
        std::vector<std::vector<StyledSpan>> lines;
        std::vector<StyledSpan> line;
        double lineWidth = 0.0;
        double spaceWidth = TextWidth(font, L" ", size);
        std::vector<double> widths(words.size());
        for (size_t index = 0; index < words.size(); index++) {
            const StyledWord& word = words[index];
            widths[index] = word.math >= 0 ? math.At(word.math).Width() : TextWidth(font, word.text, size);
        }
        const std::string noUrl;
        // Appends text to the line: merged into the previous span when the style is the
        // same (never into a formula), as AppendStyledWord does for plain text.
        auto appendText = [&](std::wstring text, const StyledWord& word, bool styled) {
            const std::string& url = styled ? word.url : noUrl;
            const bool bold = styled && word.bold;
            const bool italic = styled && word.italic;
            const bool strike = styled && word.strike;
            const bool code = styled && word.code;
            if (!line.empty()) {
                StyledSpan& last = line.back();
                if (last.math < 0 && last.bold == bold && last.italic == italic && last.strike == strike &&
                    last.code == code && last.url == url) {
                    last.text += text;
                    return;
                }
            }
            StyledSpan& added = MathAppend(line);
            added.text = std::move(text);
            added.url = url;
            added.bold = bold;
            added.italic = italic;
            added.strike = strike;
            added.code = code;
        };
        auto flush = [&]() {
            if (!line.empty()) MathAppend(lines).swap(line);
            line.clear();
            lineWidth = 0.0;
        };

        for (size_t index = 0; index < words.size(); index++) {
            const StyledWord& word = words[index];
            if (word.math >= 0 && math.IsDisplay(word.math)) {
                flush();
                MathAppend(MathAppend(lines)).math = word.math;
                continue;
            }
            const bool joined = joins[index] != 0;
            bool needsSpace = !line.empty() && !joined && !IsClosingPunctuation(word.text);
            if (!line.empty() && !joined) {
                double groupWidth = widths[index];
                for (size_t next = index + 1; next < words.size() && joins[next] != 0 &&
                    !(words[next].math >= 0 && math.IsDisplay(words[next].math)); next++) {
                    groupWidth += widths[next];
                }
                if (lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width) {
                    flush();
                    needsSpace = false;
                }
            }
            if (word.math >= 0) {
                if (needsSpace) {
                    appendText(L" ", word, false);
                    lineWidth += spaceWidth;
                }
                MathAppend(line).math = word.math;
                lineWidth += widths[index];
                continue;
            }
            if (widths[index] > width) {
                // A word wider than the line is split by character, as PushWrappedWord does.
                flush();
                std::wstring part;
                double partWidth = 0.0;
                const double scale = size * 0.001;
                for (wchar_t ch : word.text) {
                    double chWidth = CodepointWidth(font, (uint16_t)ch, scale);
                    if (!part.empty() && partWidth + chWidth > width) {
                        appendText(std::move(part), word, true);
                        flush();
                        part.clear();
                        partWidth = 0.0;
                    }
                    part.push_back(ch);
                    partWidth += chWidth;
                }
                appendText(std::move(part), word, true);
                lineWidth = partWidth;
                continue;
            }
            std::wstring textRun = word.text;
            double wordWidth = widths[index];
            if (needsSpace) {
                if (word.strike) {
                    appendText(L" ", word, false);
                    lineWidth += spaceWidth;
                } else {
                    textRun.insert(textRun.begin(), L' ');
                    wordWidth += spaceWidth;
                }
            }
            appendText(std::move(textRun), word, true);
            lineWidth += wordWidth;
        }

        flush();
        if (lines.empty()) MathAppend(lines);
        return lines;
    }

    // Wraps heading or table-cell text that carries formulas (Block::hasMath).
    RAYOMD_MATH_COLD std::vector<std::vector<StyledSpan>> WrapMathText(const std::string& text, double width, double size, bool bold) {
        std::vector<StyledSpan> spans = MathTextSpans(text, size, width, bold);
        std::vector<StyledWord> words = SplitMathWords(spans);
        return WrapMathWords(words, MarkMathJoins(spans, words.size()), width, size);
    }

    // A table cell inside a table with formulas. A cell without a formula wraps
    // exactly as RenderTable wraps it.
    RAYOMD_MATH_COLD std::vector<std::vector<StyledSpan>> WrapMathCell(const std::string& cell, double width, double size, bool bold) {
        if (cell.find(Internal::kMathTextOpen) != std::string::npos) return WrapMathText(cell, width, size, bold);
        std::vector<std::vector<StyledSpan>> lines;
        for (const std::wstring& line : WrapText(font, Utf8ToWide(cell), width, size)) {
            MathAppend(MathAppend(lines)).text = line;
        }
        return lines;
    }

    double MathLineWidth(const std::vector<StyledSpan>& line, double size, bool) const {
        double width = 0.0;
        for (const StyledSpan& span : line) {
            width += span.math >= 0 ? math.At(span.math).Width() : TextWidth(font, span.text, size);
        }
        return width;
    }

    // Paints a line of plain text and formulas in one colour (headings, table cells).
    RAYOMD_MATH_COLD void PaintMathTextLine(const std::vector<StyledSpan>& line, double x, double baseline, double size,
        const char* color, bool bold) {
        double cursor = x;
        for (const StyledSpan& span : line) {
            if (span.math >= 0) {
                cursor += math.Emit(span.math, content, cursor, baseline, color);
                continue;
            }
            PaintText(cursor, baseline, size, span.text, color, bold);
            cursor += TextWidth(font, span.text, size);
        }
    }

    void TableFill(double x, double top, double w, double h) { DrawRect(x, top, w, h, "0.91 0.93 0.95"); }
    void TableStroke(double x, double top, double w, double h) { DrawStrokeRect(x, top, w, h); }
    const char* TableTextColor(bool header) const { return header ? "0.04 0.04 0.04" : "0.10 0.10 0.10"; }

    // One piece of a wrapped line: text[begin, end) of the paragraph's run buffer in one style.
    struct StyledRun {
        uint32_t begin = 0;
        uint32_t end = 0;
        std::string_view url;       // link target, in StyledRuns::parsed
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
    };

    // The wrapped lines of one paragraph, kept flat: the text of every run lies in one
    // buffer and a line is a range of runs. One object serves all paragraphs, so wrapping
    // builds no string and no vector per line.
    struct StyledRuns {
        Internal::InlineRuns parsed;        // the paragraph as parsed; holds the link targets
        std::wstring wide;                  // one parsed run, or the whole plain paragraph, as wide text
        std::wstring text;
        std::vector<StyledRun> runs;
        std::vector<uint32_t> lineEnds;     // one past the last run of each line

        std::wstring_view Text(const StyledRun& run) const {
            return std::wstring_view(text.data() + run.begin, run.end - run.begin);
        }
    };
    StyledRuns paragraphRuns;

    // Wraps `text` into `out` with the line breaks and span boundaries of WrapStyled.
    // Returns false for text with explicit line breaks or possible formulas, which take
    // the general path.
    bool WrapStyledRuns(const std::string& text, double width, double size, StyledRuns& out) {
        if (text.size() >= 0x40000000u || text.find('\n') != std::string::npos ||
            text.find('$') != std::string::npos || text.find("\\(") != std::string::npos) {
            return false;
        }
        out.text.clear();
        out.runs.clear();
        out.lineEnds.clear();
        if (!RayoMd::Text::ContainsByteClass(text, RayoMd::Text::kByteInlineSyntax)) {
            // No inline syntax: every line is one unstyled run, also when it is empty.
            out.wide.clear();
            if (text.find('\xE2') == std::string::npos) AppendUtf8ToWide(out.wide, text);
            else AppendUtf8ToWide(out.wide, NormalizeSymbols(text));
            WrapWideWords(font, std::wstring_view(out.wide), width, size, [&](std::wstring_view line) {
                out.runs.emplace_back();
                StyledRun& run = out.runs.back();
                run.begin = static_cast<uint32_t>(out.text.size());
                out.text.append(line.data(), line.size());
                run.end = static_cast<uint32_t>(out.text.size());
                out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
            });
            return true;
        }

        Internal::ParseInlineRuns(text, out.parsed);
        size_t lineStart = 0;       // index of the first run of the line being filled
        double lineWidth = 0.0;
        const double spaceWidth = TextWidth(font, L" ", size);
        // The word joins the last run of the line when the style is the same.
        auto append = [&](std::wstring_view word, bool spaced, bool bold, bool italic, bool strike, std::string_view url,
            bool code) {
            StyledRun* run = out.runs.size() != lineStart ? &out.runs.back() : nullptr;
            if (!run || run->bold != bold || run->italic != italic || run->strike != strike || run->code != code ||
                run->url != url) {
                out.runs.emplace_back();
                run = &out.runs.back();
                run->begin = static_cast<uint32_t>(out.text.size());
                run->url = url;
                run->bold = bold;
                run->italic = italic;
                run->strike = strike;
                run->code = code;
            }
            if (spaced) out.text.push_back(L' ');
            out.text.append(word.data(), word.size());
            run->end = static_cast<uint32_t>(out.text.size());
        };

        for (const Internal::InlineRun& span : out.parsed.runs) {
            // Parsed runs differ in style or link target, so each one is a span of its own.
            out.wide.clear();
            AppendUtf8ToWide(out.wide, out.parsed.Text(span));
            const std::wstring& source = out.wide;
            const std::string_view url = out.parsed.Url(span);
            for (size_t at = 0; at < source.size();) {
                while (at < source.size() && IsWideSpace(source[at])) at++;
                size_t end = at;
                while (end < source.size() && !IsWideSpace(source[end])) end++;
                if (end == at) break;
                std::wstring_view word(source.data() + at, end - at);
                at = end;

                double wordWidth = TextWidth(font, word, size);
                const bool lineEmpty = out.runs.size() == lineStart;
                bool needsSpace = !lineEmpty && !IsClosingPunctuation(word);
                double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

                if (!lineEmpty && lineWidth + addWidth > width) {
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineWidth = 0.0;
                    needsSpace = false;
                }

                bool spaced = false;
                if (needsSpace) {
                    if (span.strike) {
                        append(std::wstring_view(L" ", 1), false, false, false, false, std::string_view(), false);
                        lineWidth += spaceWidth;
                    } else {
                        spaced = true;
                        wordWidth += spaceWidth;
                    }
                }
                append(word, spaced, span.bold, span.italic, span.strike, url, span.code);
                lineWidth += wordWidth;
            }
        }

        if (out.runs.size() != lineStart) out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
        if (out.lineEnds.empty()) out.lineEnds.push_back(0);
        return true;
    }

    std::vector<std::vector<StyledSpan>> WrapStyled(const std::string& text, double width, double size) {
        if (text.find('\n') != std::string::npos) {
            std::vector<std::vector<StyledSpan>> explicitLines;
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                std::string segment = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                std::vector<std::vector<StyledSpan>> wrapped = WrapStyled(segment, width, size);
                explicitLines.insert(explicitLines.end(), std::make_move_iterator(wrapped.begin()),
                    std::make_move_iterator(wrapped.end()));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return explicitLines;
        }
        if (!RayoMd::Text::ContainsByteClass(text, RayoMd::Text::kByteInlineSyntax)) {
            std::vector<std::vector<StyledSpan>> lines;
            std::wstring wide = Utf8ToWide(NormalizeSymbols(text));
            std::vector<std::wstring> wrapped = WrapText(font, std::wstring_view(wide), width, size);
            lines.reserve(wrapped.size());
            for (std::wstring& line : wrapped) {
                lines.emplace_back();
                lines.back().emplace_back();
                lines.back().back().text = std::move(line);
            }
            if (lines.empty()) lines.push_back({});
            return lines;
        }

        std::vector<StyledSpan> spans = ParseInlineStyled(text, size, width);
        if (math.Active()) {
            std::vector<StyledWord> mathWords = SplitMathWords(spans);
            return WrapMathWords(mathWords, MarkMathJoins(spans, mathWords.size()), width, size);
        }
        std::vector<std::vector<StyledSpan>> lines;
        lines.reserve(std::max<size_t>(1, text.size() / 72));
        std::vector<StyledSpan> line;
        line.reserve(16);
        double lineWidth = 0.0;
        double spaceWidth = TextWidth(font, L" ", size);
        const std::string noUrl;

        // The words are views into the spans: nothing is copied until a word lands in a line.
        for (const StyledSpan& span : spans) {
            const std::wstring& source = span.text;
            for (size_t at = 0; at < source.size();) {
                while (at < source.size() && IsWideSpace(source[at])) at++;
                size_t end = at;
                while (end < source.size() && !IsWideSpace(source[end])) end++;
                if (end == at) break;
                std::wstring_view word(source.data() + at, end - at);
                at = end;

                double wordWidth = TextWidth(font, word, size);
                bool needsSpace = !line.empty() && !IsClosingPunctuation(word);
                double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

                if (!line.empty() && lineWidth + addWidth > width) {
                    lines.push_back(std::move(line));
                    line.clear();
                    line.reserve(16);
                    lineWidth = 0.0;
                    needsSpace = false;
                }

                bool spaced = false;
                if (needsSpace) {
                    if (span.strike) {
                        AppendStyledWord(line, std::wstring_view(L" ", 1), false, false, false, false, noUrl, false);
                        lineWidth += spaceWidth;
                    } else {
                        spaced = true;
                        wordWidth += spaceWidth;
                    }
                }
                AppendStyledWord(line, word, spaced, span.bold, span.italic, span.strike, span.url, span.code);
                lineWidth += wordWidth;
            }
        }

        if (!line.empty()) lines.push_back(std::move(line));
        if (lines.empty()) lines.push_back({});
        return lines;
    }

    void NewPage() {
        pageStarts.push_back(content.size());
        pageLinks.push_back({});
        y = PAGE_H - margin;
    }

    void RenderPageBreak() {
        if (!pageStarts.empty() && content.size() > pageStarts.back()) NewPage();
    }

    void Ensure(double needed) {
        if (y - needed < margin) NewPage();
    }

    void PaintText(double x, double baseline, double size, std::wstring_view line, const char* color,
        bool fauxBold = false, bool italic = false, bool strike = false) {
        const size_t colorSize = strlen(color);
        const size_t hexBytes = HexTextBytes(line);
        TailWriter w(content, 96 + colorSize * 2 + kOperandBytes * 9 + hexBytes * 2);
        w.Lit("q ");
        w.Bytes(color, colorSize);
        w.Lit(" rg BT /F1 ");
        w.Fixed(size);
        w.Lit(" Tf 1 0 ");
        if (italic) w.Lit("0.18 ");
        else w.Lit("0 ");
        w.Lit("1 ");
        w.Fixed(x);
        w.Lit(" ");
        w.Fixed(baseline);
        w.Lit(" Tm ");
        const char* hex = w.cursor;
        w.cursor = WriteHexText(w.cursor, font, line, usedCids);
        const size_t hexSize = (size_t)(w.cursor - hex);
        w.Lit(" Tj");
        if (fauxBold) {
            w.Lit(" 1 0 ");
            if (italic) w.Lit("0.18 ");
            else w.Lit("0 ");
            w.Lit("1 ");
            w.Fixed(x + 0.28);
            w.Lit(" ");
            w.Fixed(baseline);
            w.Lit(" Tm ");
            w.Bytes(hex, hexSize);
            w.Lit(" Tj");
        }
        w.Lit(" ET");
        if (strike && !line.empty()) {
            double width = TextWidth(font, line, size);
            w.Lit(" ");
            w.Bytes(color, colorSize);
            w.Lit(" RG 0.55 w ");
            w.Fixed(x);
            w.Lit(" ");
            w.Fixed(baseline + size * 0.34);
            w.Lit(" m ");
            w.Fixed(x + width);
            w.Lit(" ");
            w.Fixed(baseline + size * 0.34);
            w.Lit(" l S");
        }
        w.Lit(" Q\n");
    }

    void DrawTextLine(double x, double size, const std::wstring& line, const char* color = "0.08 0.08 0.08") {
        double lh = size * 1.35;
        Ensure(lh);
        PaintText(x, y - size, size, line, color);
        y -= lh;
    }

    void DrawRect(double x, double top, double w, double h, const char* color) {
        const size_t colorSize = strlen(color);
        TailWriter out(content, 24 + colorSize + kOperandBytes * 4);
        out.Lit("q ");
        out.Bytes(color, colorSize);
        out.Lit(" rg ");
        out.Fixed(x);
        out.Lit(" ");
        out.Fixed(top - h);
        out.Lit(" ");
        out.Fixed(w);
        out.Lit(" ");
        out.Fixed(h);
        out.Lit(" re f Q\n");
    }

    void DrawStrokeRect(double x, double top, double w, double h, const char* color = "0.72 0.72 0.72", double lineWidth = 0.45) {
        const size_t colorSize = strlen(color);
        TailWriter out(content, 24 + colorSize + kOperandBytes * 5);
        out.Lit("q ");
        out.Bytes(color, colorSize);
        out.Lit(" RG ");
        out.Fixed(lineWidth);
        out.Lit(" w ");
        out.Fixed(x);
        out.Lit(" ");
        out.Fixed(top - h);
        out.Lit(" ");
        out.Fixed(w);
        out.Lit(" ");
        out.Fixed(h);
        out.Lit(" re S Q\n");
    }

    void AddLink(double x, double baseline, double width, double size, std::string_view url) {
        if (url.empty() || width <= 0.0 || pageLinks.empty()) return;
        pageLinks.back().push_back({ x, baseline - 1.0, x + width, baseline + size * 1.05, std::string(url) });
    }

    void DrawImage(int imageIndex, double x, double top, double w, double h) {
        std::string& c = content;
        c += "q ";
        AppendF(c, w);
        c += " 0 0 ";
        AppendF(c, h);
        c += " ";
        AppendF(c, x);
        c += " ";
        AppendF(c, top - h);
        c += " cm /";
        c += "Im";
        AppendSize(c, (size_t)imageIndex + 1);
        c += " Do Q\n";
    }

    void RenderImageFallback(const Block& block) {
        std::string fallback = block.text.empty() ? block.imageSrc : block.text;
        if (fallback.empty()) fallback = "image";
        RenderParagraph(fallback);
    }

    void RenderImage(const Block& block) {
        if (!images) {
            RenderImageFallback(block);
            return;
        }

        int index = -1;
        if (!images->Resolve(block.imageSrc, block.text, index)) {
            RenderImageFallback(block);
            return;
        }

        const PdfImage& image = images->Get(index);
        double maxW = PAGE_W - margin * 2.0;
        double maxH = PAGE_H - margin * 2.0;
        double w = (double)image.width * 72.0 / 96.0;
        double h = (double)image.height * 72.0 / 96.0;
        if (w <= 0.0 || h <= 0.0) {
            RenderImageFallback(block);
            return;
        }

        double scale = std::min(1.0, std::min(maxW / w, maxH / h));
        w *= scale;
        h *= scale;

        Ensure(h + 10.0);
        double x = margin + (maxW - w) * 0.5;
        DrawImage(index, x, y, w, h);
        y -= h + 10.0;
    }

    void RenderHeading(const Block& block) {
        static const double sizes[] = { 0, 26, 22, 18, 15, 13, 12 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        if (y < PAGE_H - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;

        if (block.hasMath) {
            RenderMathTextLines(block.text, margin, PAGE_W - margin * 2.0, size, "0.02 0.02 0.02", false, false);
            y -= level <= 2 ? 8.0 : 5.0;
            return;
        }
        std::wstring text = Utf8ToWide(block.text);
        for (const auto& line : WrapText(font, text, PAGE_W - margin * 2.0, size)) {
            DrawTextLine(margin, size, line, "0.02 0.02 0.02");
        }
        y -= level <= 2 ? 8.0 : 5.0;
    }

    // Heading lines that contain formulas: plain text and formulas in one colour,
    // each line as tall as its tallest formula needs. `quote` adds the quote strip.
    RAYOMD_MATH_COLD void RenderMathTextLines(const std::string& text, double x, double width, double size,
        const char* color, bool bold, bool quote) {
        math.Clear();
        double lh = size * 1.35;
        for (const auto& line : WrapMathText(text, width, size, bold)) {
            MathLineExtent extent = MeasureMathLine(math, line, size, lh);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lh + 2.0 + extra);
                DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lh + 3.0 + extra, "0.94 0.95 0.96");
                DrawRect(margin, y + 2.0, 3.0, lh + 3.0 + extra, "0.45 0.62 0.72");
            } else {
                Ensure(lh + extra);
            }
            y -= extent.above;
            PaintMathTextLine(line, x, y - size, size, color, bold);
            y -= lh + extent.below;
        }
    }

    // Paragraph or quote lines of a text that contains formulas. A line grows by
    // exactly what its tallest formula needs beyond the normal line box; the text
    // baseline, code backgrounds and link rectangles keep their usual offsets.
    RAYOMD_MATH_COLD void RenderMathLines(const std::vector<std::vector<StyledSpan>>& lines, double x, double width, bool quote) {
        const char* textColor = quote ? "0.18 0.22 0.25" : "0.08 0.08 0.08";
        const char* codeColor = quote ? "0.16 0.16 0.15" : "0.18 0.18 0.17";
        const char* codeFill = quote ? "0.88 0.89 0.88" : "0.94 0.94 0.92";
        for (const auto& line : lines) {
            MathLineExtent extent = MeasureMathLine(math, line, bodySize, lineHeight);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lineHeight + 2.0 + extra);
                DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0 + extra, "0.94 0.95 0.96");
                DrawRect(margin, y + 2.0, 3.0, lineHeight + 3.0 + extra, "0.45 0.62 0.72");
            } else {
                Ensure(lineHeight + extra);
            }
            y -= extent.above;
            double cursor = x;
            if (IsDisplayMathLine(math, line)) {
                cursor = x + std::max(0.0, (width - math.At(line[0].math).Width()) * 0.5);
            }
            double baseline = y - bodySize;
            for (const StyledSpan& span : line) {
                if (span.math >= 0) {
                    cursor += math.Emit(span.math, content, cursor, baseline, textColor);
                    continue;
                }
                double spanWidth = TextWidth(font, span.text, bodySize);
                if (span.code) DrawRect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, codeFill);
                const char* color = span.url.empty() ? (span.code ? codeColor : textColor) : "0.05 0.30 0.68";
                PaintText(cursor, baseline, bodySize, span.text, color, span.bold, span.italic, span.strike);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight + extent.below;
        }
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, PAGE_W - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        double lh = bodySize * 1.35;
        if (math.Active()) math.Clear();
        if (WrapStyledRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lh);
                double cursor = x;
                double baseline = y - bodySize;
                for (; index < lineEnd; index++) {
                    const StyledRun& run = paragraphRuns.runs[index];
                    const std::wstring_view runText = paragraphRuns.Text(run);
                    double spanWidth = TextWidth(font, runText, bodySize);
                    if (run.code) DrawRect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.94 0.94 0.92");
                    const char* color = run.url.empty() ? (run.code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : "0.05 0.30 0.68";
                    PaintText(cursor, baseline, bodySize, runText, color, run.bold, run.italic, run.strike);
                    AddLink(cursor, baseline, spanWidth, bodySize, run.url);
                    cursor += spanWidth;
                }
                y -= lh;
            }
            y -= 5.0;
            return;
        }
        const std::vector<std::vector<StyledSpan>> lines = WrapStyled(text, width, bodySize);
        if (math.Active()) {
            RenderMathLines(lines, x, width, false);
            y -= 5.0;
            return;
        }
        for (const auto& line : lines) {
            Ensure(lh);
            double cursor = x;
            double baseline = y - bodySize;
            for (const auto& span : line) {
                double spanWidth = TextWidth(font, span.text, bodySize);
                if (span.code) DrawRect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.94 0.94 0.92");
                const char* color = span.url.empty() ? (span.code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : "0.05 0.30 0.68";
                PaintText(cursor, baseline, bodySize, span.text, color, span.bold, span.italic, span.strike);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lh;
        }
        y -= 5.0;
    }

    void RenderListItem(const std::string& marker, const Block& block) {
        double x = margin + 16.0 + block.level * 18.0;
        double width = PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0;
        RenderParagraph(marker + block.text, x, width);
    }

    void RenderQuote(const Block& block) {
        if (block.children.empty()) {
            RenderQuote(block.text);
            return;
        }
        RenderQuoteChildren(block.children);
    }

    void RenderQuoteChildren(const std::vector<Block>& children) {
        for (const Block& child : children) {
            switch (child.type) {
            case BlockType::Paragraph: RenderQuote(child.text); break;
            case BlockType::Heading: RenderQuoteHeading(child); break;
            case BlockType::Bullet:
                RenderQuote("- " + child.text);
                if (!child.children.empty()) {
                    double savedMargin = margin;
                    margin += 14.0;
                    RenderQuoteChildren(child.children);
                    margin = savedMargin;
                }
                break;
            case BlockType::Numbered: {
                std::string item;
                AppendInt(item, child.number);
                item += ". " + child.text;
                RenderQuote(item);
                if (!child.children.empty()) {
                    double savedMargin = margin;
                    margin += 14.0;
                    RenderQuoteChildren(child.children);
                    margin = savedMargin;
                }
                break;
            }
            case BlockType::Quote: {
                double savedMargin = margin;
                margin += 10.0;
                RenderQuote(child);
                margin = savedMargin;
                break;
            }
            case BlockType::MathBlock: RenderDisplayMath(child.text, true); break;
            case BlockType::Code:
            case BlockType::Table:
            case BlockType::Rule:
            case BlockType::Image: {
                double savedMargin = margin;
                margin += 14.0;
                if (child.type == BlockType::Code) RenderCode(child.text);
                else if (child.type == BlockType::Table && child.hasMath) RenderMathTable(*this, child);
                else if (child.type == BlockType::Table) RenderTable(child.rows, child.aligns);
                else if (child.type == BlockType::Rule) RenderRule();
                else RenderImage(child);
                margin = savedMargin;
                break;
            }
            case BlockType::PageBreak: RenderPageBreak(); break;
            }
        }
    }

    void RenderQuoteHeading(const Block& block) {
        static const double sizes[] = { 0, 18, 16, 14, 13, 12, 11.5 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        double height = size * 1.35;
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        if (block.hasMath) {
            RenderMathTextLines(block.text, x, width, size, "0.10 0.15 0.18", true, true);
            y -= 7.0;
            return;
        }
        std::wstring text = Utf8ToWide(block.text);
        for (const std::wstring& line : WrapText(font, text, width, size)) {
            Ensure(height + 2.0);
            DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, height + 3.0, "0.94 0.95 0.96");
            DrawRect(margin, y + 2.0, 3.0, height + 3.0, "0.45 0.62 0.72");
            PaintText(x, y - size, size, line, "0.10 0.15 0.18", true);
            y -= height;
        }
        y -= 7.0;
    }
    void RenderQuote(const std::string& text) {
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        if (math.Active()) math.Clear();
        if (WrapStyledRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lineHeight + 2.0);
                DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
                DrawRect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
                double cursor = x;
                double baseline = y - bodySize;
                for (; index < lineEnd; index++) {
                    const StyledRun& run = paragraphRuns.runs[index];
                    const std::wstring_view runText = paragraphRuns.Text(run);
                    double spanWidth = TextWidth(font, runText, bodySize);
                    if (run.code) DrawRect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.88 0.89 0.88");
                    const char* color = run.url.empty() ? (run.code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : "0.05 0.30 0.68";
                    PaintText(cursor, baseline, bodySize, runText, color, run.bold, run.italic, run.strike);
                    AddLink(cursor, baseline, spanWidth, bodySize, run.url);
                    cursor += spanWidth;
                }
                y -= lineHeight;
            }
            y -= 7.0;
            return;
        }
        const std::vector<std::vector<StyledSpan>> lines = WrapStyled(text, width, bodySize);
        if (math.Active()) {
            RenderMathLines(lines, x, width, true);
            y -= 7.0;
            return;
        }
        for (const auto& line : lines) {
            Ensure(lineHeight + 2.0);
            DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
            DrawRect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
            double cursor = x;
            double baseline = y - bodySize;
            for (const StyledSpan& span : line) {
                double spanWidth = TextWidth(font, span.text, bodySize);
                if (span.code) DrawRect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.88 0.89 0.88");
                const char* color = span.url.empty() ? (span.code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : "0.05 0.30 0.68";
                PaintText(cursor, baseline, bodySize, span.text, color, span.bold, span.italic, span.strike);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight;
        }
        y -= 7.0;
    }

    void RenderCode(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = PAGE_W - margin * 2.0 - 16.0;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                Ensure(lh + 4.0);
                DrawRect(margin, y + 3.0, PAGE_W - margin * 2.0, lh + 5.0, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "0.12 0.12 0.12");
            }
        }
        y -= 8.0;
    }

    void RenderMath(const std::string& text) {
        RenderDisplayMath(text, false);
    }

    // Display math: one unbreakable formula, centred in the available width.
    // `quoted` draws it inside the block-quote strip. A formula that is empty or
    // does not fit the page even at half size is shown as its source.
    RAYOMD_MATH_COLD void RenderDisplayMath(const std::string& tex, bool quoted) {
        double left = quoted ? margin + 14.0 : margin;
        double available = quoted ? PAGE_W - margin * 2.0 - 22.0 : PAGE_W - margin * 2.0;
        double padTop = quoted ? kQuoteMathPad : kDisplayMathAbove;
        double padBottom = quoted ? kQuoteMathPad : 0.0;
        double maxHeight = PAGE_H - margin * 2.0 - padTop - padBottom - 3.0;
        MathFormula formula;
        if (!LayoutMathToFit(tex, bodySize, true, false, available, maxHeight, &mathFallback, formula)) {
            double savedMargin = margin;
            if (quoted) margin += 14.0;
            RenderMathSource(tex);
            margin = savedMargin;
            return;
        }
        double total = padTop + formula.Ascent() + formula.Descent() + padBottom;
        Ensure(total + 2.0);
        if (quoted) {
            DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, total + 3.0, "0.94 0.95 0.96");
            DrawRect(margin, y + 2.0, 3.0, total + 3.0, "0.45 0.62 0.72");
        }
        formula.Emit(content, left + (available - formula.Width()) * 0.5, y - padTop - formula.Ascent(),
            quoted ? "0.18 0.22 0.25" : "0.08 0.08 0.08");
        math.MarkUsed();
        y -= total;
        y -= quoted ? 7.0 : kDisplayMathBelow;
    }

    // The pre-math rendering of a formula block: its source in a tinted box.
    RAYOMD_MATH_COLD void RenderMathSource(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 10.5;
        double pitch = size * 1.35;   // what DrawTextLine advances by
        double x = margin + 12.0;
        double width = PAGE_W - margin * 2.0 - 24.0;
        bool first = true;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                size_t pageCount = pageStarts.size();
                Ensure(pitch + 8.0);
                if (pageStarts.size() != pageCount) first = true;
                // One tile per line, flush with its neighbours, so the tint never covers text.
                double pad = first ? 4.0 : 0.0;
                DrawRect(margin, y + pad, PAGE_W - margin * 2.0, pitch + pad, "0.97 0.97 0.95");
                DrawTextLine(x, size, line, "0.10 0.10 0.10");
                first = false;
            }
        }
        DrawRect(margin, y, PAGE_W - margin * 2.0, 4.0, "0.97 0.97 0.95");
        y -= 12.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;

        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = PAGE_W - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        double cellTextWidth = std::max(16.0, colWidth - pad * 2.0);

        y -= 3.0;
        for (size_t r = 0; r < rows.size(); r++) {
            std::vector<std::vector<std::wstring>> wrapped(columns);
            size_t maxLines = 1;

            for (size_t c = 0; c < columns; c++) {
                std::string cell = c < rows[r].size() ? rows[r][c] : "";
                wrapped[c] = WrapText(font, Utf8ToWide(cell), cellTextWidth, size);
                maxLines = std::max(maxLines, wrapped[c].size());
            }

            double rowHeight = maxLines * lh + pad * 2.0;
            Ensure(rowHeight + 5.0);
            double top = y;
            if (r == 0) DrawRect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");

            for (size_t c = 0; c < columns; c++) {
                double cellX = margin + c * colWidth;
                DrawStrokeRect(cellX, top, colWidth, rowHeight);

                int align = c < aligns.size() ? aligns[c] : -1;
                for (size_t lineIdx = 0; lineIdx < wrapped[c].size(); lineIdx++) {
                    const std::wstring& line = wrapped[c][lineIdx];
                    double tx = cellX + pad;
                    double lineWidth = TextWidth(font, line, size);
                    if (align == 0) tx = cellX + (colWidth - lineWidth) * 0.5;
                    else if (align == 1) tx = cellX + colWidth - pad - lineWidth;

                    double baseline = top - pad - size - lineIdx * lh;
                    PaintText(tx, baseline, size, line, r == 0 ? "0.04 0.04 0.04" : "0.10 0.10 0.10", r == 0);
                }
            }

            y -= rowHeight;
        }
        y -= 9.0;
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = content;
        c += "q 0.68 0.68 0.68 RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, PAGE_W - margin);
        c += " ";
        AppendF(c, y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

static bool AddGlyphClosure(const TtfFont& font, uint16_t glyph, std::vector<uint8_t>& include) {
    if (glyph >= include.size()) return false;
    if (include[glyph]) return true;
    include[glyph] = 1;

    if (glyph + 1 >= font.loca.size()) return true;
    uint32_t start = font.loca[glyph];
    uint32_t end = font.loca[glyph + 1];
    if (end <= start) return true;

    auto it = font.tables.find("glyf");
    if (it == font.tables.end()) return false;
    const TtfFont::Table& glyf = it->second;
    size_t glyphStart = (size_t)glyf.offset + start;
    size_t glyphEnd = (size_t)glyf.offset + end;
    if (glyphEnd > font.bytes.size() || glyphStart + 10 > glyphEnd) return false;

    int16_t contours = ReadS16(font.bytes, glyphStart);
    if (contours >= 0) return true;

    size_t off = glyphStart + 10;
    bool more = true;
    while (more) {
        if (off + 4 > glyphEnd) return false;
        uint16_t flags = ReadU16(font.bytes, off);
        uint16_t componentGlyph = ReadU16(font.bytes, off + 2);
        off += 4;
        if (!AddGlyphClosure(font, componentGlyph, include)) return false;
        off += (flags & 0x0001) ? 4 : 2;
        if (flags & 0x0008) off += 2;
        else if (flags & 0x0040) off += 4;
        else if (flags & 0x0080) off += 8;
        more = (flags & 0x0020) != 0;
    }
    return off <= glyphEnd;
}

static bool OriginalTableBytes(const TtfFont& font, const std::string& tag, std::string& out) {
    auto it = font.tables.find(tag);
    if (it == font.tables.end()) return false;
    const TtfFont::Table& t = it->second;
    if ((size_t)t.offset + t.length > font.bytes.size()) return false;
    out.assign((const char*)font.bytes.data() + t.offset, t.length);
    return true;
}

static bool BuildSubsetFontBytes(const TtfFont& font, const CidList& used, std::string& out) {
    if (font.glyphCount == 0 || font.loca.size() < (size_t)font.glyphCount + 1) return false;

    std::vector<uint8_t> include((size_t)font.glyphCount, 0);
    AddGlyphClosure(font, 0, include);
    AddGlyphClosure(font, font.GlyphFor('?'), include);
    AddGlyphClosure(font, font.GlyphFor(' '), include);
    for (uint16_t cid : used) {
        AddGlyphClosure(font, font.GlyphFor(cid), include);
    }

    auto glyfIt = font.tables.find("glyf");
    if (glyfIt == font.tables.end()) return false;
    const TtfFont::Table& oldGlyf = glyfIt->second;

    std::string glyfData;
    glyfData.reserve(std::min<size_t>(font.bytes.size(), std::max<size_t>(32 * 1024, used.size() * 256)));
    std::string locaData;
    locaData.reserve(((size_t)font.glyphCount + 1) * 4);

    for (uint32_t gid = 0; gid < font.glyphCount; gid++) {
        AppendU32(locaData, (uint32_t)glyfData.size());
        uint32_t start = font.loca[gid];
        uint32_t end = font.loca[gid + 1];
        if (include[gid] && end > start) {
            size_t oldStart = (size_t)oldGlyf.offset + start;
            size_t oldEnd = (size_t)oldGlyf.offset + end;
            if (oldEnd > font.bytes.size() || oldEnd < oldStart) return false;
            glyfData.append((const char*)font.bytes.data() + oldStart, oldEnd - oldStart);
            while (glyfData.size() & 3) glyfData.push_back('\0');
        }
    }
    AppendU32(locaData, (uint32_t)glyfData.size());

    struct SubsetTable {
        std::string tag;
        std::string data;
        uint32_t checksum = 0;
        uint32_t offset = 0;
    };

    static const char* wanted[] = {
        "OS/2", "cmap", "cvt ", "fpgm", "gasp", "glyf", "head", "hhea",
        "hmtx", "loca", "maxp", "name", "post", "prep"
    };

    std::vector<SubsetTable> tables;
    for (const char* tag : wanted) {
        std::string table;
        std::string tagString(tag, 4);
        if (tagString == "glyf") {
            table = glyfData;
        } else if (tagString == "loca") {
            table = locaData;
        } else {
            if (!OriginalTableBytes(font, tagString, table)) continue;
            if (tagString == "head") {
                if (table.size() < 54) return false;
                SetU32(table, 8, 0);
                SetU16(table, 50, 1);
            }
        }
        tables.push_back({ tagString, std::move(table), 0, 0 });
    }

    if (tables.empty()) return false;
    std::sort(tables.begin(), tables.end(),
        [](const SubsetTable& a, const SubsetTable& b) { return a.tag < b.tag; });

    uint16_t numTables = (uint16_t)tables.size();
    uint16_t maxPower = 1;
    uint16_t entrySelector = 0;
    while ((uint16_t)(maxPower * 2) <= numTables) {
        maxPower *= 2;
        entrySelector++;
    }
    uint16_t searchRange = maxPower * 16;
    uint16_t rangeShift = numTables * 16 - searchRange;

    out.clear();
    out.reserve(12 + (size_t)numTables * 16 + glyfData.size() + locaData.size() + 96 * numTables);
    AppendU32(out, ReadU32(font.bytes, 0));
    AppendU16(out, numTables);
    AppendU16(out, searchRange);
    AppendU16(out, entrySelector);
    AppendU16(out, rangeShift);
    out.resize(12 + (size_t)numTables * 16, '\0');

    size_t headAdjustmentOffset = std::string::npos;
    for (SubsetTable& table : tables) {
        while (out.size() & 3) out.push_back('\0');
        table.offset = (uint32_t)out.size();
        table.checksum = ChecksumString(table.data);
        out.append(table.data);
        while (out.size() & 3) out.push_back('\0');
        if (table.tag == "head") headAdjustmentOffset = table.offset + 8;
    }

    for (size_t i = 0; i < tables.size(); i++) {
        size_t rec = 12 + i * 16;
        memcpy(&out[rec], tables[i].tag.data(), 4);
        SetU32(out, rec + 4, tables[i].checksum);
        SetU32(out, rec + 8, tables[i].offset);
        SetU32(out, rec + 12, (uint32_t)tables[i].data.size());
    }

    if (headAdjustmentOffset == std::string::npos) return false;
    uint32_t fileSum = ChecksumString(out);
    SetU32(out, headAdjustmentOffset, 0xB1B0AFBAu - fileSum);
    return true;
}

static std::string BuildFontFileObject(const std::string& fontBytes) {
    std::string body;
    body.reserve(fontBytes.size() + 128);
    body += "<< /Length ";
    AppendSize(body, fontBytes.size());
    body += " /Length1 ";
    AppendSize(body, fontBytes.size());
    body += " >>\nstream\n";
    body.append(fontBytes.data(), fontBytes.size());
    body += "\nendstream";
    return body;
}

class BoundedStringCache {
public:
    BoundedStringCache(size_t maxEntriesValue, size_t maxBytesValue)
        : maxEntries(maxEntriesValue), maxBytes(maxBytesValue) {}

    std::shared_ptr<const std::string> Get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = values.find(key);
        return it == values.end() ? nullptr : it->second;
    }

    std::shared_ptr<const std::string> Insert(const std::string& key, std::string value) {
        auto candidate = std::make_shared<const std::string>(std::move(value));
        if (candidate->size() > maxBytes) return candidate;
        std::lock_guard<std::mutex> lock(mutex);
        auto existing = values.find(key);
        if (existing != values.end()) return existing->second;
        if (values.size() >= maxEntries || bytes + candidate->size() > maxBytes) {
            values.clear();
            bytes = 0;
        }
        bytes += candidate->size();
        values.emplace(key, candidate);
        return candidate;
    }

private:
    const size_t maxEntries;
    const size_t maxBytes;
    size_t bytes = 0;
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const std::string>> values;
};

static std::shared_ptr<const std::string> CachedFontFileObject(
    const TtfFont& font, const CidList& used, const std::string& key) {
    static BoundedStringCache cache(8, 32u * 1024u * 1024u);
    if (auto cached = cache.Get(key)) return cached;

    std::string subset;
    const std::string* fontBytes = nullptr;
    if (BuildSubsetFontBytes(font, used, subset) && subset.size() < font.bytes.size()) fontBytes = &subset;

    std::string full;
    if (!fontBytes) {
        full.assign((const char*)font.bytes.data(), font.bytes.size());
        fontBytes = &full;
    }
    return cache.Insert(key, BuildFontFileObject(*fontBytes));
}

static std::shared_ptr<const std::string> MakeCidToGidMap(
    const TtfFont& font, const CidList& used, const std::string& key) {
    static BoundedStringCache cache(64, 8u * 1024u * 1024u);
    if (auto cached = cache.Get(key)) return cached;
    uint16_t maxCid = 255;
    for (uint16_t cid : used) maxCid = std::max(maxCid, cid);
    std::string map((size_t)(maxCid + 1) * 2, char(0));
    for (uint32_t cid = 0; cid <= maxCid; cid++) {
        uint16_t glyph = font.GlyphFor(cid);
        map[(size_t)cid * 2] = (char)((glyph >> 8) & 0xff);
        map[(size_t)cid * 2 + 1] = (char)(glyph & 0xff);
    }
    return cache.Insert(key, std::move(map));
}

static std::shared_ptr<const std::string> MakeToUnicodeCMap(
    const CidList& used, const std::string& key) {
    static BoundedStringCache cache(64, 8u * 1024u * 1024u);
    if (auto cached = cache.Get(key)) return cached;
    std::string out;
    out.reserve(512 + used.size() * 20);
    out += R"PDF(/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /RayoMDUnicode def
/CMapType 2 def
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
)PDF";
    int count = 0;
    for (auto it = used.begin(); it != used.end();) {
        int chunk = std::min<int>(100, (int)used.size() - count);
        AppendInt(out, chunk);
        out += " beginbfchar";
        out.push_back(char(10));
        for (int i = 0; i < chunk; i++, ++it) {
            out.push_back('<');
            AppendHex4(out, *it);
            out += "> <";
            AppendHex4(out, *it);
            out += ">";
            out.push_back(char(10));
            count++;
        }
        out += "endbfchar";
        out.push_back(char(10));
    }
    if (count == 0) {
        out += R"PDF(1 beginbfchar
<0020> <0020>
endbfchar
)PDF";
    }
    out += R"PDF(endcmap
CMapName currentdict /CMap defineresource pop
end
end
)PDF";
    return cache.Insert(key, std::move(out));
}

static std::shared_ptr<const std::string> MakeWidths(
    const TtfFont& font, const CidList& used, const std::string& key) {
    static BoundedStringCache cache(64, 8u * 1024u * 1024u);
    if (auto cached = cache.Get(key)) return cached;
    std::string out;
    out.reserve(16 + used.size() * 14);
    out += "[ ";
    if (used.empty()) {
        out += "32 [ ";
        AppendInt(out, font.WidthForCid(32));
        out += " ] ";
    } else {
        for (uint16_t cid : used) {
            AppendInt(out, cid);
            out += " [ ";
            AppendInt(out, font.WidthForCid(cid));
            out += " ] ";
        }
    }
    out += "]";
    return cache.Insert(key, std::move(out));
}

static std::string EscapeLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) out.push_back(c);
    }
    return out;
}

// EscapeLiteral written straight into a buffer with room for two bytes per input byte.
// Returns the end of what was written.
static char* WriteEscapedLiteral(char* out, std::string_view text) {
    // Most lines hold no parenthesis, backslash or control byte and are copied in one piece.
    if (!RayoMd::Text::ContainsByteClass(text, RayoMd::Text::kByteLiteralSpecial)) {
        if (!text.empty()) memcpy(out, text.data(), text.size());
        return out + text.size();
    }
    for (char c : text) {
        if ((unsigned char)c < 32 || (unsigned char)c >= 127) continue;
        if (c == '(' || c == ')' || c == '\\') *out++ = '\\';
        *out++ = c;
    }
    return out;
}

static bool IsAsciiDocument(const std::string& s) {
    auto isAsciiAfterSymbolNormalization = [](std::string_view text) {
        for (size_t i = 0; i < text.size();) {
            unsigned char c = (unsigned char)text[i];
            if (c < 128) {
                i++;
                continue;
            }
            if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9C\x85") == 0) {
                i += 3;
                continue;
            }
            if (i + 6 <= text.size() && text.compare(i, 6, "\xE2\x9A\xA0\xEF\xB8\x8F") == 0) {
                i += 6;
                continue;
            }
            if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9A\xA0") == 0) {
                i += 3;
                continue;
            }
            if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9D\x8C") == 0) {
                i += 3;
                continue;
            }
            return false;
        }
        return true;
    };

#ifdef FAST_MD_SSE2
    const char* ptr = s.data();
    const char* end = ptr + s.size();
    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128((const __m128i*)ptr);
        if (_mm_movemask_epi8(chunk) != 0) return isAsciiAfterSymbolNormalization(s);
        ptr += 16;
    }
    while (ptr < end) {
        if ((unsigned char)*ptr >= 128) return isAsciiAfterSymbolNormalization(s);
        ptr++;
    }
    return true;
#else
    return isAsciiAfterSymbolNormalization(s);
#endif
}

static constexpr std::array<unsigned char, 128> MakeAsciiWidthTable() {
    std::array<unsigned char, 128> widths{};
    for (auto& width : widths) width = 52;
    widths[' '] = 28;
    for (char c : { 'i', 'l', 'I', '!', '.', ',', ':', ';' }) widths[static_cast<unsigned char>(c)] = 25;
    for (char c : { 'm', 'w', 'M', 'W' }) widths[static_cast<unsigned char>(c)] = 78;
    for (char c = '0'; c <= '9'; c++) widths[static_cast<unsigned char>(c)] = 56;
    for (char c = 'A'; c <= 'Z'; c++) widths[static_cast<unsigned char>(c)] = 62;
    return widths;
}

static constexpr auto kAsciiWidthHundredths = MakeAsciiWidthTable();

// kAsciiWidthHundredths for the bytes of a word and 0 for the four space characters: one
// lookup per byte both ends a word and measures it.
static constexpr std::array<unsigned char, 256> MakeAsciiWordWidthTable() {
    std::array<unsigned char, 256> widths{};
    for (size_t c = 0; c < widths.size(); c++) widths[c] = c < 128 ? kAsciiWidthHundredths[c] : 52;
    for (char c : { ' ', '\t', '\r', '\n' }) widths[static_cast<unsigned char>(c)] = 0;
    return widths;
}

static constexpr auto kAsciiWordWidths = MakeAsciiWordWidthTable();

static double AsciiCharWidth(char c, double size, bool mono = false) {
    if (mono) return size * 0.60;
    return size * kAsciiWidthHundredths[static_cast<unsigned char>(c)] * 0.01;
}

static double AsciiTextWidth(std::string_view text, double size, bool mono = false) {
    if (mono) return static_cast<double>(text.size()) * size * 0.60;
    unsigned widthHundredths = 0;
    for (unsigned char c : text) widthHundredths += kAsciiWidthHundredths[c];
    return static_cast<double>(widthHundredths) * size * 0.01;
}

struct WrappedAsciiLine {
    std::string text;
    double width = 0.0;
};

// Appends the words of `text`, which starts and ends with a word, one space apart.
static void AppendSingleSpaced(std::string& out, std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        size_t end = i;
        while (end < text.size() && !IsSpace(text[end])) end++;
        if (i != 0) out.push_back(' ');
        out.append(text.data() + i, end - i);
        i = end;
        while (i < text.size() && IsSpace(text[i])) i++;
    }
}

// Greedy word wrap of text without inline syntax: emit(line, width) receives every line.
// A line is the piece of `text` from its first to its last word; only when two of its
// words are not exactly one space apart is it rebuilt in a scratch string. The view
// passed to emit is valid until emit returns.
template <typename Emit>
static void WrapAsciiWords(std::string_view text, double maxWidth, double size, bool mono, Emit emit) {
    const double spaceWidth = mono ? size * 0.60 : size * 0.28;
    std::string respaced;
    size_t lineStart = 0;
    size_t lineEnd = 0;         // the line is text[lineStart, lineEnd); empty when they are equal
    bool singleSpaced = true;
    double lineWidth = 0.0;
    bool emitted = false;
    auto flush = [&]() {
        std::string_view line = text.substr(lineStart, lineEnd - lineStart);
        if (!singleSpaced) {
            respaced.clear();
            AppendSingleSpaced(respaced, line);
            line = respaced;
        }
        emit(line, lineWidth);
        emitted = true;
    };
    auto startLine = [&](size_t start, size_t end, double width) {
        lineStart = start;
        lineEnd = end;
        lineWidth = width;
        singleSpaced = true;
    };

    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && kAsciiWordWidths[static_cast<unsigned char>(text[i])] == 0) i++;
        const size_t start = i;
        unsigned wordHundredths = 0;
        for (; i < text.size(); i++) {
            const unsigned charHundredths = kAsciiWordWidths[static_cast<unsigned char>(text[i])];
            if (charHundredths == 0) break;
            wordHundredths += charHundredths;
        }
        if (i == start) continue;
        // AsciiTextWidth of the word.
        const double wordWidth = mono ? static_cast<double>(i - start) * size * 0.60 :
            static_cast<double>(wordHundredths) * size * 0.01;
        const bool lineEmpty = lineEnd == lineStart;

        if (!lineEmpty && lineWidth + spaceWidth + wordWidth <= maxWidth) {
            if (start - lineEnd != 1 || text[lineEnd] != ' ') singleSpaced = false;
            lineEnd = i;
            lineWidth += spaceWidth + wordWidth;
            continue;
        }
        if (lineEmpty && wordWidth <= maxWidth) {
            startLine(start, i, wordWidth);
            continue;
        }
        if (!lineEmpty) {
            flush();
            lineStart = lineEnd = 0;
            lineWidth = 0.0;
        }
        if (wordWidth <= maxWidth) {
            startLine(start, i, wordWidth);
            continue;
        }

        // A word wider than the line is cut into pieces; the last piece stays open.
        size_t partStart = start;
        double partWidth = 0.0;
        for (size_t at = start; at < i; at++) {
            const double charWidth = AsciiCharWidth(text[at], size, mono);
            if (at > partStart && partWidth + charWidth > maxWidth) {
                emit(text.substr(partStart, at - partStart), partWidth);
                emitted = true;
                partStart = at;
                partWidth = 0.0;
            }
            partWidth += charWidth;
        }
        startLine(partStart, i, partWidth);
    }
    if (lineEnd != lineStart) flush();
    if (!emitted) emit(std::string_view(""), 0.0);
}

// Wraps text the parser already stripped. It is read a second time, with math delimiters
// literal; text without inline syntax, the usual case, is wrapped in place.
template <typename Emit>
static void ForEachWrappedAsciiLine(const std::string& raw, double maxWidth, double size, bool mono, Emit emit) {
    if (!RayoMd::Text::ContainsByteClass(raw, RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead)) {
        WrapAsciiWords(raw, maxWidth, size, mono, emit);
        return;
    }
    const std::string text = StripInlineMarkdown(raw, false);
    WrapAsciiWords(text, maxWidth, size, mono, emit);
}

static void WrapAsciiText(const std::string& raw, double maxWidth, double size,
    bool mono, std::vector<WrappedAsciiLine>& lines) {
    lines.clear();
    ForEachWrappedAsciiLine(raw, maxWidth, size, mono, [&](std::string_view line, double width) {
        lines.emplace_back();
        lines.back().text.assign(line.data(), line.size());
        lines.back().width = width;
    });
}

static std::vector<WrappedAsciiLine> WrapAsciiText(
    const std::string& raw, double maxWidth, double size, bool mono = false) {
    std::vector<WrappedAsciiLine> lines;
    WrapAsciiText(raw, maxWidth, size, mono, lines);
    return lines;
}

static std::vector<std::string> WrapAsciiLiteral(std::string_view raw, double maxWidth, double size, bool mono = false) {
    std::vector<std::string> lines;
    std::string line;
    line.reserve(std::min<size_t>(raw.size(), 256));
    double lineWidth = 0.0;

    for (char ch : raw) {
        if (ch == '\t') ch = ' ';
        if ((unsigned char)ch < 32 || (unsigned char)ch >= 127) continue;
        double charWidth = AsciiCharWidth(ch, size, mono);
        if (!line.empty() && lineWidth + charWidth > maxWidth) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0.0;
        }
        line.push_back(ch);
        lineWidth += charWidth;
    }

    lines.push_back(line);
    return lines;
}

class StandardRenderer {
public:
    // The content streams of all pages are appended to `output`, one after another.
    StandardRenderer(std::string& output, PdfStyle styleValue, const PdfMargin& marginValue, ImageRegistry* imageRegistry)
        : images(imageRegistry), style(styleValue), content(output) {
        margin = ResolveMarginPoints(marginValue);
        bodySize = style == PdfStyle::Tech ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) { RenderBlocks(*this, blocks); }

    // Offset in the output buffer at which the content stream of each page starts.
    const std::vector<size_t>& PageStarts() const { return pageStarts; }
    const std::vector<std::vector<LinkRect>>& PageLinks() const { return pageLinks; }
    bool MathUsed() const { return math.Used(); }

private:
    template <typename RendererType>
    friend void RenderBlocks(RendererType&, const std::vector<Block>&);
    template <typename RendererType>
    friend void RenderMathTable(RendererType&, const Block&);

    ImageRegistry* images = nullptr;
    PdfStyle style = PdfStyle::Elegant;
    double margin = 54.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    // The page being rendered is the tail of `content`, so painting is a plain append.
    std::string& content;
    std::vector<size_t> pageStarts;
    std::vector<std::vector<LinkRect>> pageLinks;
    MathPool math;

    double MaxMathHeight() const {
        return (PAGE_H - margin * 2.0) * 0.5;
    }

    void RenderBullet(const Block& block) {
        RenderParagraph("- " + block.text, margin + 16.0 + block.level * 18.0,
            PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderNumbered(const Block& block) {
        std::string item;
        item.reserve(block.text.size() + 8);
        AppendInt(item, block.number);
        item += ". ";
        item += block.text;
        RenderParagraph(item, margin + 16.0 + block.level * 18.0,
            PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderIndentedBlocks(const std::vector<Block>& blocks, double inset) {
        double savedMargin = margin;
        margin += inset;
        RenderBlocks(*this, blocks);
        margin = savedMargin;
    }

    void NewPage() {
        pageStarts.push_back(content.size());
        pageLinks.push_back({});
        y = PAGE_H - margin;
    }

    void RenderPageBreak() {
        if (!pageStarts.empty() && content.size() > pageStarts.back()) NewPage();
    }

    void Ensure(double needed) {
        if (y - needed < margin) NewPage();
    }

    void Rect(double x, double top, double w, double h, const char* color, bool stroke = false) {
        const size_t colorSize = strlen(color);
        TailWriter out(content, 32 + colorSize + kOperandBytes * 4);
        out.Lit("q ");
        out.Bytes(color, colorSize);
        if (stroke) out.Lit(" RG 0.45 w ");
        else out.Lit(" rg ");
        out.Fixed(x);
        out.Lit(" ");
        out.Fixed(top - h);
        out.Lit(" ");
        out.Fixed(w);
        out.Lit(" ");
        out.Fixed(h);
        out.Lit(" re ");
        if (stroke) out.Lit("S Q\n");
        else out.Lit("f Q\n");
    }

    struct AsciiSpan {
        std::string text;
        std::string url;
        bool code = false;
        int math = -1;   // index into `math` when the span is a formula; text is empty then
    };

    struct AsciiWord {
        std::string text;
        std::string url;
        bool code = false;
        int math = -1;
    };

    void AddLink(double x, double baseline, double width, double size, std::string_view url) {
        if (url.empty() || width <= 0.0 || pageLinks.empty()) return;
        pageLinks.back().push_back({ x, baseline - 1.0, x + width, baseline + size * 1.05, std::string(url) });
    }

    void PushAsciiSpan(std::vector<AsciiSpan>& spans, std::string&& text, std::string&& url, bool code) {
        if (text.empty()) return;
        if (!spans.empty() && spans.back().url == url && spans.back().code == code && spans.back().math < 0) {
            spans.back().text += text;
        } else {
            spans.emplace_back();
            AsciiSpan& span = spans.back();
            span.text = std::move(text);
            span.url = std::move(url);
            span.code = code;
        }
    }

    // A formula becomes one span that refers to the pool. A formula that is empty
    // or cannot fit the line is shown as its TeX source in code style instead.
    RAYOMD_MATH_COLD void PushAsciiMathSpan(std::vector<AsciiSpan>& spans, const Internal::InlineSpan& span, double size,
        double width, bool bold) {
        int index = math.Add(span.text, size, span.math == Internal::InlineMath::Display, bold, width,
            MaxMathHeight(), nullptr);
        AsciiSpan& added = MathAppend(spans);
        if (index < 0) {
            added.text = MathSourceText(span);
            added.code = true;
            math.MarkSourceShown();
            return;
        }
        added.math = index;
    }

    std::vector<AsciiSpan> ParseAsciiLinkSpans(const std::string& text, double size, double width) {
        std::vector<AsciiSpan> spans;
        std::vector<Internal::InlineSpan> inlineSpans = Internal::ParseInlineSpans(text);
        spans.reserve(inlineSpans.size());
        for (Internal::InlineSpan& span : inlineSpans) {
            if (span.math != Internal::InlineMath::None) {
                PushAsciiMathSpan(spans, span, size, width, false);
                continue;
            }
            PushAsciiSpan(spans, std::move(span.text), std::move(span.url), span.code);
        }
        return spans;
    }

    // Heading and table-cell text (Block::hasMath): plain text with formulas between
    // kMathTextOpen and kMathTextClose. No Markdown is interpreted here.
    RAYOMD_MATH_COLD std::vector<AsciiSpan> MathTextSpans(const std::string& text, double size, double width, bool bold) {
        std::vector<AsciiSpan> spans;
        Internal::ForEachMathTextSegment(text, [&](std::string_view segment, bool isMath) {
            if (!isMath) {
                if (!segment.empty()) MathAppend(spans).text.assign(segment.data(), segment.size());
                return;
            }
            Internal::InlineSpan span;
            span.text.assign(segment.data(), segment.size());
            span.math = Internal::InlineMath::Inline;
            PushAsciiMathSpan(spans, span, size, width, bold);
        });
        return spans;
    }

    // Word list of formula-bearing text: the words WrapAsciiLinks walks, plus one word
    // per formula.
    RAYOMD_MATH_COLD std::vector<AsciiWord> SplitAsciiMathWords(const std::vector<AsciiSpan>& spans) {
        std::vector<AsciiWord> words;
        for (const AsciiSpan& span : spans) {
            if (span.math >= 0) {
                MathAppend(words).math = span.math;
                continue;
            }
            const size_t length = span.text.size();
            for (size_t start = 0; start < length;) {
                while (start < length && IsSpace(span.text[start])) start++;
                size_t end = start;
                while (end < length && !IsSpace(span.text[end])) end++;
                if (end > start) {
                    AsciiWord& word = MathAppend(words);
                    word.text.assign(span.text, start, end - start);
                    word.url = span.url;
                    word.code = span.code;
                }
                start = end;
            }
        }
        return words;
    }

    // Appends one word to the line being filled, with a leading space when `spaced`. It is
    // merged into the previous span when link and code state are the same.
    void AppendAsciiWord(std::vector<AsciiSpan>& line, std::string_view word, bool spaced, const std::string& url,
        bool code) {
        if (!line.empty()) {
            AsciiSpan& last = line.back();
            if (last.url == url && last.code == code) {
                if (spaced) last.text.push_back(' ');
                last.text.append(word.data(), word.size());
                return;
            }
        }
        line.emplace_back();
        AsciiSpan& span = line.back();
        span.text.reserve(word.size() + 1);
        if (spaced) span.text.push_back(' ');
        span.text.append(word.data(), word.size());
        span.url = url;
        span.code = code;
    }

    // Text next to a formula is positioned with the real advance widths of the
    // standard fonts, not with kAsciiWidthHundredths: the approximation is off by
    // several points over a long run, which would show as a gap before the formula.
    // Line breaks are still decided with the approximate table, as everywhere else.
    static double MathTextWidth(std::string_view text, double size, bool mono, bool bold) {
        return Internal::StandardTextWidth(text, size, mono ? Internal::StandardTextFont::Mono :
            (bold ? Internal::StandardTextFont::Bold : Internal::StandardTextFont::Regular));
    }

    // Line filling for text that contains at least one formula. Same greedy rule as
    // WrapAsciiLinks, plus: a formula is one unbreakable word, words glued to a
    // formula (joinPrev) stay on its line, and a $$...$$ formula gets its own line.
    RAYOMD_MATH_COLD std::vector<std::vector<AsciiSpan>> WrapAsciiMathWords(const std::vector<AsciiWord>& words,
        const std::vector<unsigned char>& joins, double width, double size) {
        std::vector<std::vector<AsciiSpan>> lines;
        std::vector<AsciiSpan> line;
        double lineWidth = 0.0;
        double spaceWidth = AsciiTextWidth(" ", size);
        std::vector<double> widths(words.size());
        for (size_t index = 0; index < words.size(); index++) {
            const AsciiWord& word = words[index];
            widths[index] = word.math >= 0 ? math.At(word.math).Width() :
                AsciiTextWidth(word.text, size, word.code);
        }
        // Appends text to the line: merged into the previous span when the style is the
        // same (never into a formula), as AppendAsciiWord does for plain text.
        auto appendText = [&](std::string text, const std::string& url, bool code) {
            if (!line.empty()) {
                AsciiSpan& last = line.back();
                if (last.math < 0 && last.code == code && last.url == url) {
                    last.text += text;
                    return;
                }
            }
            AsciiSpan& added = MathAppend(line);
            added.text = std::move(text);
            added.url = url;
            added.code = code;
        };
        auto flush = [&]() {
            if (!line.empty()) MathAppend(lines).swap(line);
            line.clear();
            lineWidth = 0.0;
        };

        for (size_t index = 0; index < words.size(); index++) {
            const AsciiWord& word = words[index];
            if (word.math >= 0 && math.IsDisplay(word.math)) {
                flush();
                MathAppend(MathAppend(lines)).math = word.math;
                continue;
            }
            const bool joined = joins[index] != 0;
            bool needsSpace = !line.empty() && !joined;
            if (!line.empty() && !joined) {
                double groupWidth = widths[index];
                for (size_t next = index + 1; next < words.size() && joins[next] != 0 &&
                    !(words[next].math >= 0 && math.IsDisplay(words[next].math)); next++) {
                    groupWidth += widths[next];
                }
                if (lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width) {
                    flush();
                    needsSpace = false;
                }
            }
            if (word.math >= 0) {
                if (needsSpace) {
                    appendText(" ", std::string(), false);
                    lineWidth += spaceWidth;
                }
                MathAppend(line).math = word.math;
                lineWidth += widths[index];
                continue;
            }
            if (widths[index] > width) {
                // A word wider than the line is split by character, as WrapAsciiText does.
                flush();
                std::string part;
                double partWidth = 0.0;
                for (char ch : word.text) {
                    double chWidth = AsciiCharWidth(ch, size, word.code);
                    if (!part.empty() && partWidth + chWidth > width) {
                        appendText(std::move(part), word.url, word.code);
                        flush();
                        part.clear();
                        partWidth = 0.0;
                    }
                    part.push_back(ch);
                    partWidth += chWidth;
                }
                appendText(std::move(part), word.url, word.code);
                lineWidth = partWidth;
                continue;
            }
            std::string textRun = word.text;
            double wordWidth = widths[index];
            if (needsSpace) {
                textRun.insert(textRun.begin(), ' ');
                wordWidth += spaceWidth;
            }
            appendText(std::move(textRun), word.url, word.code);
            lineWidth += wordWidth;
        }

        flush();
        if (lines.empty()) MathAppend(lines);
        return lines;
    }

    // Wraps heading or table-cell text that carries formulas (Block::hasMath).
    RAYOMD_MATH_COLD std::vector<std::vector<AsciiSpan>> WrapMathText(const std::string& text, double width, double size, bool bold) {
        std::vector<AsciiSpan> spans = MathTextSpans(text, size, width, bold);
        std::vector<AsciiWord> words = SplitAsciiMathWords(spans);
        return WrapAsciiMathWords(words, MarkMathJoins(spans, words.size()), width, size);
    }

    // A table cell inside a table with formulas. A cell without a formula wraps
    // exactly as RenderTable wraps it.
    RAYOMD_MATH_COLD std::vector<std::vector<AsciiSpan>> WrapMathCell(const std::string& cell, double width, double size, bool bold) {
        if (cell.find(Internal::kMathTextOpen) != std::string::npos) return WrapMathText(cell, width, size, bold);
        std::vector<std::vector<AsciiSpan>> lines;
        for (WrappedAsciiLine& line : WrapAsciiText(cell, width, size)) {
            MathAppend(MathAppend(lines)).text = std::move(line.text);
        }
        return lines;
    }

    double MathLineWidth(const std::vector<AsciiSpan>& line, double size, bool bold) const {
        double width = 0.0;
        for (const AsciiSpan& span : line) {
            width += span.math >= 0 ? math.At(span.math).Width() : MathTextWidth(span.text, size, false, bold);
        }
        return width;
    }

    // Paints a line of plain text and formulas in one colour (headings, table cells).
    RAYOMD_MATH_COLD void PaintMathTextLine(const std::vector<AsciiSpan>& line, double x, double baseline, double size,
        const char* color, bool bold) {
        double cursor = x;
        for (const AsciiSpan& span : line) {
            if (span.math >= 0) {
                cursor += math.Emit(span.math, content, cursor, baseline, color);
                continue;
            }
            Text(cursor, baseline, size, span.text, bold ? "F2" : "F1", color);
            cursor += MathTextWidth(span.text, size, false, bold);
        }
    }

    void TableFill(double x, double top, double w, double h) { Rect(x, top, w, h, "0.91 0.93 0.95"); }
    void TableStroke(double x, double top, double w, double h) { Rect(x, top, w, h, "0.72 0.72 0.72", true); }
    const char* TableTextColor(bool) const { return "0.08 0.08 0.08"; }

    // One piece of a wrapped line: text[begin, end) of the paragraph's run buffer, in the
    // link and code state of the span it came from.
    struct AsciiRun {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t widthHundredths = 0;   // sum of kAsciiWidthHundredths over the text
        bool code = false;
        std::string_view url;           // link target, in AsciiRuns::parsed
    };

    // The wrapped lines of one paragraph, kept flat: the text of every run lies in one
    // buffer and a line is a range of runs. One object serves all paragraphs, so wrapping
    // builds no string and no vector per line.
    struct AsciiRuns {
        Internal::InlineRuns parsed;        // the paragraph as parsed; holds the link targets
        std::string text;
        std::vector<AsciiRun> runs;
        std::vector<uint32_t> lineEnds;     // one past the last run of each line

        std::string_view Text(const AsciiRun& run) const {
            return std::string_view(text.data() + run.begin, run.end - run.begin);
        }
        // The value of AsciiTextWidth(Text(run), size, run.code), without reading the text again.
        double Width(const AsciiRun& run, double size) const {
            if (run.code) return static_cast<double>(run.end - run.begin) * size * 0.60;
            return static_cast<double>(run.widthHundredths) * size * 0.01;
        }
    };
    AsciiRuns paragraphRuns;

    // Wraps `text` into `out` with the line breaks and span boundaries of WrapAsciiLinks.
    // Returns false for text with explicit line breaks or possible formulas, which take
    // the general path.
    bool WrapAsciiRuns(const std::string& text, double width, double size, AsciiRuns& out) {
        if (text.size() >= 0x40000000u || text.find('\n') != std::string::npos ||
            text.find('$') != std::string::npos || text.find("\\(") != std::string::npos) {
            return false;
        }
        Internal::ParseInlineRuns(text, out.parsed);
        out.runs.clear();
        out.lineEnds.clear();
        // Every word is written once, with at most one space in front of it.
        out.text.resize(out.parsed.text.size() * 2 + 1);
        char* const textBegin = &out.text[0];
        char* cursor = textBegin;
        size_t lineStart = 0;       // index of the first run of the line being filled
        double lineWidth = 0.0;
        const double spaceWidth = AsciiTextWidth(" ", size);

        const std::vector<Internal::InlineRun>& parsed = out.parsed.runs;
        for (size_t first = 0; first < parsed.size();) {
            // The standard fonts show no emphasis: parsed runs that differ only in that are
            // one span of text, and a word may run across them.
            const bool code = parsed[first].code;
            const std::string_view url = out.parsed.Url(parsed[first]);
            size_t last = first;
            while (last + 1 < parsed.size() && parsed[last + 1].code == code && out.parsed.Url(parsed[last + 1]) == url) last++;
            const std::string_view source(out.parsed.text.data() + parsed[first].begin, parsed[last].end - parsed[first].begin);
            first = last + 1;
            for (size_t at = 0; at < source.size();) {
                while (at < source.size() && kAsciiWordWidths[static_cast<unsigned char>(source[at])] == 0) at++;
                const size_t start = at;
                unsigned wordHundredths = 0;
                for (; at < source.size(); at++) {
                    const unsigned charHundredths = kAsciiWordWidths[static_cast<unsigned char>(source[at])];
                    if (charHundredths == 0) break;
                    wordHundredths += charHundredths;
                }
                if (at == start) break;
                const size_t wordBytes = at - start;
                double wordWidth = code ? static_cast<double>(wordBytes) * size * 0.60 :
                    static_cast<double>(wordHundredths) * size * 0.01;
                bool needsSpace = out.runs.size() != lineStart;
                const double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

                if (needsSpace && lineWidth + addWidth > width) {
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineWidth = 0.0;
                    needsSpace = false;
                }

                // The word joins the last run of the line when link and code state are the same.
                AsciiRun* run = out.runs.size() != lineStart ? &out.runs.back() : nullptr;
                if (!run || run->code != code || run->url != url) {
                    out.runs.emplace_back();
                    run = &out.runs.back();
                    run->begin = static_cast<uint32_t>(cursor - textBegin);
                    run->code = code;
                    run->url = url;
                }
                if (needsSpace) {
                    wordWidth += spaceWidth;
                    *cursor++ = ' ';
                    run->widthHundredths += kAsciiWidthHundredths[static_cast<unsigned char>(' ')];
                }
                memcpy(cursor, source.data() + start, wordBytes);
                cursor += wordBytes;
                run->widthHundredths += wordHundredths;
                run->end = static_cast<uint32_t>(cursor - textBegin);
                lineWidth += wordWidth;
            }
        }

        out.text.resize(static_cast<size_t>(cursor - textBegin));
        if (out.runs.size() != lineStart) out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
        if (out.lineEnds.empty()) out.lineEnds.push_back(0);
        return true;
    }

    std::vector<std::vector<AsciiSpan>> WrapAsciiLinks(const std::string& text, double width, double size) {
        if (text.find('\n') != std::string::npos) {
            std::vector<std::vector<AsciiSpan>> explicitLines;
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                std::string segment = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                std::vector<std::vector<AsciiSpan>> wrapped = WrapAsciiLinks(segment, width, size);
                explicitLines.insert(explicitLines.end(), std::make_move_iterator(wrapped.begin()),
                    std::make_move_iterator(wrapped.end()));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return explicitLines;
        }
        std::vector<AsciiSpan> spans = ParseAsciiLinkSpans(text, size, width);
        if (math.Active()) {
            std::vector<AsciiWord> mathWords = SplitAsciiMathWords(spans);
            return WrapAsciiMathWords(mathWords, MarkMathJoins(spans, mathWords.size()), width, size);
        }
        std::vector<std::vector<AsciiSpan>> lines;
        lines.reserve(std::max<size_t>(1, text.size() / 72));
        std::vector<AsciiSpan> line;
        double lineWidth = 0.0;
        double spaceWidth = AsciiTextWidth(" ", size);

        // The words are views into the spans: nothing is copied until a word lands in a line.
        for (const AsciiSpan& span : spans) {
            const std::string& source = span.text;
            for (size_t at = 0; at < source.size();) {
                while (at < source.size() && IsSpace(source[at])) at++;
                size_t end = at;
                while (end < source.size() && !IsSpace(source[end])) end++;
                if (end == at) break;
                std::string_view word(source.data() + at, end - at);
                at = end;

                double wordWidth = AsciiTextWidth(word, size, span.code);
                bool needsSpace = !line.empty();
                double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

                if (!line.empty() && lineWidth + addWidth > width) {
                    lines.push_back(std::move(line));
                    line.clear();
                    lineWidth = 0.0;
                    needsSpace = false;
                }

                if (needsSpace) wordWidth += spaceWidth;
                AppendAsciiWord(line, word, needsSpace, span.url, span.code);
                lineWidth += wordWidth;
            }
        }

        if (!line.empty()) lines.push_back(std::move(line));
        if (lines.empty()) lines.push_back({});
        return lines;
    }

    void DrawImage(int imageIndex, double x, double top, double w, double h) {
        std::string& c = content;
        c += "q ";
        AppendF(c, w);
        c += " 0 0 ";
        AppendF(c, h);
        c += " ";
        AppendF(c, x);
        c += " ";
        AppendF(c, top - h);
        c += " cm /";
        c += "Im";
        AppendSize(c, (size_t)imageIndex + 1);
        c += " Do Q\n";
    }

    void RenderImageFallback(const Block& block) {
        std::string fallback = block.text.empty() ? block.imageSrc : block.text;
        if (fallback.empty()) fallback = "image";
        RenderParagraph(fallback);
    }

    void RenderImage(const Block& block) {
        if (!images) {
            RenderImageFallback(block);
            return;
        }

        int index = -1;
        if (!images->Resolve(block.imageSrc, block.text, index)) {
            RenderImageFallback(block);
            return;
        }

        const PdfImage& image = images->Get(index);
        double maxW = PAGE_W - margin * 2.0;
        double maxH = PAGE_H - margin * 2.0;
        double w = (double)image.width * 72.0 / 96.0;
        double h = (double)image.height * 72.0 / 96.0;
        if (w <= 0.0 || h <= 0.0) {
            RenderImageFallback(block);
            return;
        }

        double scale = std::min(1.0, std::min(maxW / w, maxH / h));
        w *= scale;
        h *= scale;

        Ensure(h + 10.0);
        double x = margin + (maxW - w) * 0.5;
        DrawImage(index, x, y, w, h);
        y -= h + 10.0;
    }

    void Text(double x, double baseline, double size, std::string_view text, const char* fontName, const char* color = "0.08 0.08 0.08") {
        if (text.empty()) return;
        const size_t colorSize = strlen(color);
        const size_t fontSize = strlen(fontName);
        TailWriter out(content, 48 + colorSize + fontSize + kOperandBytes * 3 + text.size() * 2);
        out.Lit("q ");
        out.Bytes(color, colorSize);
        out.Lit(" rg BT /");
        out.Bytes(fontName, fontSize);
        out.Lit(" ");
        out.Fixed(size);
        out.Lit(" Tf 1 0 0 1 ");
        out.Fixed(x);
        out.Lit(" ");
        out.Fixed(baseline);
        out.Lit(" Tm (");
        out.cursor = WriteEscapedLiteral(out.cursor, text);
        out.Lit(") Tj ET Q\n");
    }

    void DrawTextLine(double x, double size, std::string_view line, const char* fontName = "F1", const char* color = "0.08 0.08 0.08", bool mono = false) {
        double lh = size * 1.35;
        Ensure(lh);
        Text(x, y - size, size, line, fontName, color);
        y -= lh;
    }

    void RenderHeading(const Block& block) {
        static const double sizes[] = { 0, 26, 22, 18, 15, 13, 12 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        if (y < PAGE_H - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;
        if (block.hasMath) {
            RenderMathTextLines(block.text, margin, PAGE_W - margin * 2.0, size, "0.02 0.02 0.02", true, false);
            y -= level <= 2 ? 8.0 : 5.0;
            return;
        }
        ForEachWrappedAsciiLine(block.text, PAGE_W - margin * 2.0, size, false, [&](std::string_view line, double) {
            DrawTextLine(margin, size, line, "F2", "0.02 0.02 0.02");
        });
        y -= level <= 2 ? 8.0 : 5.0;
    }

    // Heading lines that contain formulas: plain text and formulas in one colour,
    // each line as tall as its tallest formula needs. `quote` adds the quote strip.
    RAYOMD_MATH_COLD void RenderMathTextLines(const std::string& text, double x, double width, double size,
        const char* color, bool bold, bool quote) {
        math.Clear();
        double lh = size * 1.35;
        for (const auto& line : WrapMathText(text, width, size, bold)) {
            MathLineExtent extent = MeasureMathLine(math, line, size, lh);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lh + 2.0 + extra);
                Rect(margin, y + 2.0, PAGE_W - margin * 2.0, lh + 3.0 + extra, "0.94 0.95 0.96");
                Rect(margin, y + 2.0, 3.0, lh + 3.0 + extra, "0.45 0.62 0.72");
            } else {
                Ensure(lh + extra);
            }
            y -= extent.above;
            PaintMathTextLine(line, x, y - size, size, color, bold);
            y -= lh + extent.below;
        }
    }

    // Paragraph or quote lines of a text that contains formulas. A line grows by
    // exactly what its tallest formula needs beyond the normal line box; the text
    // baseline, code backgrounds and link rectangles keep their usual offsets.
    RAYOMD_MATH_COLD void RenderMathLines(const std::vector<std::vector<AsciiSpan>>& lines, double x, double width, bool quote) {
        const char* textColor = quote ? "0.18 0.22 0.25" : "0.08 0.08 0.08";
        const char* codeColor = quote ? "0.16 0.16 0.15" : "0.18 0.18 0.17";
        const char* codeFill = quote ? "0.88 0.89 0.88" : "0.94 0.94 0.92";
        for (const auto& line : lines) {
            MathLineExtent extent = MeasureMathLine(math, line, bodySize, lineHeight);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lineHeight + 2.0 + extra);
                Rect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0 + extra, "0.94 0.95 0.96");
                Rect(margin, y + 2.0, 3.0, lineHeight + 3.0 + extra, "0.45 0.62 0.72");
            } else {
                Ensure(lineHeight + extra);
            }
            y -= extent.above;
            double cursor = x;
            if (IsDisplayMathLine(math, line)) {
                cursor = x + std::max(0.0, (width - math.At(line[0].math).Width()) * 0.5);
            }
            double baseline = y - bodySize;
            for (const AsciiSpan& span : line) {
                if (span.math >= 0) {
                    cursor += math.Emit(span.math, content, cursor, baseline, textColor);
                    continue;
                }
                double spanWidth = MathTextWidth(span.text, bodySize, span.code, false);
                if (span.code) Rect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, codeFill);
                const char* color = span.url.empty() ? (span.code ? codeColor : textColor) : "0.05 0.30 0.68";
                Text(cursor, baseline, bodySize, span.text, span.code ? "F3" : "F1", color);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight + extent.below;
        }
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, PAGE_W - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        if (RayoMd::Text::ContainsByteClass(text, RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteLineFeed)) {
            if (math.Active()) math.Clear();
            if (WrapAsciiRuns(text, width, bodySize, paragraphRuns)) {
                const double lh = bodySize * 1.35;
                size_t index = 0;
                for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                    Ensure(lh);
                    double cursor = x;
                    double baseline = y - bodySize;
                    for (; index < lineEnd; index++) {
                        const AsciiRun& run = paragraphRuns.runs[index];
                        double spanWidth = paragraphRuns.Width(run, bodySize);
                        if (run.code) Rect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lh, "0.94 0.94 0.92");
                        const char* color = run.url.empty() ? (run.code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : "0.05 0.30 0.68";
                        Text(cursor, baseline, bodySize, paragraphRuns.Text(run), run.code ? "F3" : "F1", color);
                        AddLink(cursor, baseline, spanWidth, bodySize, run.url);
                        cursor += spanWidth;
                    }
                    y -= lh;
                }
                y -= 5.0;
                return;
            }
            const std::vector<std::vector<AsciiSpan>> lines = WrapAsciiLinks(text, width, bodySize);
            if (math.Active()) {
                RenderMathLines(lines, x, width, false);
                y -= 5.0;
                return;
            }
            for (const auto& line : lines) {
                double lh = bodySize * 1.35;
                Ensure(lh);
                double cursor = x;
                double baseline = y - bodySize;
                for (const AsciiSpan& span : line) {
                    double spanWidth = AsciiTextWidth(span.text, bodySize, span.code);
                    if (span.code) Rect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lh, "0.94 0.94 0.92");
                    const char* color = span.url.empty() ? (span.code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : "0.05 0.30 0.68";
                    Text(cursor, baseline, bodySize, span.text, span.code ? "F3" : "F1", color);
                    AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                    cursor += spanWidth;
                }
                y -= lh;
            }
            y -= 5.0;
            return;
        }

        ForEachWrappedAsciiLine(text, width, bodySize, false, [&](std::string_view line, double) {
            DrawTextLine(x, bodySize, line);
        });
        y -= 5.0;
    }

    void RenderQuote(const Block& block) {
        if (block.children.empty()) {
            RenderQuote(block.text);
            return;
        }
        RenderQuoteChildren(block.children);
    }

    void RenderQuoteChildren(const std::vector<Block>& children) {
        for (const Block& child : children) {
            switch (child.type) {
            case BlockType::Paragraph: RenderQuote(child.text); break;
            case BlockType::Heading: RenderQuoteHeading(child); break;
            case BlockType::Bullet:
                RenderQuote("- " + child.text);
                if (!child.children.empty()) {
                    double savedMargin = margin;
                    margin += 14.0;
                    RenderQuoteChildren(child.children);
                    margin = savedMargin;
                }
                break;
            case BlockType::Numbered: {
                std::string item;
                AppendInt(item, child.number);
                item += ". " + child.text;
                RenderQuote(item);
                if (!child.children.empty()) {
                    double savedMargin = margin;
                    margin += 14.0;
                    RenderQuoteChildren(child.children);
                    margin = savedMargin;
                }
                break;
            }
            case BlockType::Quote: {
                double savedMargin = margin;
                margin += 10.0;
                RenderQuote(child);
                margin = savedMargin;
                break;
            }
            case BlockType::MathBlock: RenderDisplayMath(child.text, true); break;
            case BlockType::Code:
            case BlockType::Table:
            case BlockType::Rule:
            case BlockType::Image: {
                double savedMargin = margin;
                margin += 14.0;
                if (child.type == BlockType::Code) RenderCode(child.text);
                else if (child.type == BlockType::Table && child.hasMath) RenderMathTable(*this, child);
                else if (child.type == BlockType::Table) RenderTable(child.rows, child.aligns);
                else if (child.type == BlockType::Rule) RenderRule();
                else RenderImage(child);
                margin = savedMargin;
                break;
            }
            case BlockType::PageBreak: RenderPageBreak(); break;
            }
        }
    }

    void RenderQuoteHeading(const Block& block) {
        static const double sizes[] = { 0, 18, 16, 14, 13, 12, 11.5 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        double height = size * 1.35;
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        if (block.hasMath) {
            RenderMathTextLines(block.text, x, width, size, "0.10 0.15 0.18", true, true);
            y -= 7.0;
            return;
        }
        for (const WrappedAsciiLine& line : WrapAsciiText(block.text, width, size)) {
            Ensure(height + 2.0);
            Rect(margin, y + 2.0, PAGE_W - margin * 2.0, height + 3.0, "0.94 0.95 0.96");
            Rect(margin, y + 2.0, 3.0, height + 3.0, "0.45 0.62 0.72");
            Text(x, y - size, size, line.text, "F2", "0.10 0.15 0.18");
            y -= height;
        }
        y -= 7.0;
    }
    void RenderQuote(const std::string& text) {
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        if (math.Active()) math.Clear();
        if (WrapAsciiRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lineHeight + 2.0);
                Rect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
                Rect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
                double cursor = x;
                double baseline = y - bodySize;
                for (; index < lineEnd; index++) {
                    const AsciiRun& run = paragraphRuns.runs[index];
                    double spanWidth = paragraphRuns.Width(run, bodySize);
                    if (run.code) Rect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.88 0.89 0.88");
                    const char* color = run.url.empty() ? (run.code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : "0.05 0.30 0.68";
                    Text(cursor, baseline, bodySize, paragraphRuns.Text(run), run.code ? "F3" : "F1", color);
                    AddLink(cursor, baseline, spanWidth, bodySize, run.url);
                    cursor += spanWidth;
                }
                y -= lineHeight;
            }
            y -= 7.0;
            return;
        }
        const std::vector<std::vector<AsciiSpan>> lines = WrapAsciiLinks(text, width, bodySize);
        if (math.Active()) {
            RenderMathLines(lines, x, width, true);
            y -= 7.0;
            return;
        }
        for (const auto& line : lines) {
            Ensure(lineHeight + 2.0);
            Rect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
            Rect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
            double cursor = x;
            double baseline = y - bodySize;
            for (const AsciiSpan& span : line) {
                double spanWidth = AsciiTextWidth(span.text, bodySize, span.code);
                if (span.code) Rect(cursor - 1.5, y + 1.0, spanWidth + 3.0, lineHeight, "0.88 0.89 0.88");
                const char* color = span.url.empty() ? (span.code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : "0.05 0.30 0.68";
                Text(cursor, baseline, bodySize, span.text, span.code ? "F3" : "F1", color);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight;
        }
        y -= 7.0;
    }

    void RenderCode(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = PAGE_W - margin * 2.0 - 16.0;
        for (const auto& rawLine : raw) {
            for (const auto& line : WrapAsciiLiteral(rawLine, width, size, true)) {
                Ensure(lh + 4.0);
                Rect(margin, y + 3.0, PAGE_W - margin * 2.0, lh + 5.0, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "F3", "0.12 0.12 0.12", true);
            }
        }
        y -= 8.0;
    }

    void RenderMath(const std::string& text) {
        RenderDisplayMath(text, false);
    }

    // Display math: one unbreakable formula, centred in the available width.
    // `quoted` draws it inside the block-quote strip. A formula that is empty or
    // does not fit the page even at half size is shown as its source.
    RAYOMD_MATH_COLD void RenderDisplayMath(const std::string& tex, bool quoted) {
        double left = quoted ? margin + 14.0 : margin;
        double available = quoted ? PAGE_W - margin * 2.0 - 22.0 : PAGE_W - margin * 2.0;
        double padTop = quoted ? kQuoteMathPad : kDisplayMathAbove;
        double padBottom = quoted ? kQuoteMathPad : 0.0;
        double maxHeight = PAGE_H - margin * 2.0 - padTop - padBottom - 3.0;
        MathFormula formula;
        if (!LayoutMathToFit(tex, bodySize, true, false, available, maxHeight, nullptr, formula)) {
            double savedMargin = margin;
            if (quoted) margin += 14.0;
            RenderMathSource(tex);
            margin = savedMargin;
            return;
        }
        double total = padTop + formula.Ascent() + formula.Descent() + padBottom;
        Ensure(total + 2.0);
        if (quoted) {
            Rect(margin, y + 2.0, PAGE_W - margin * 2.0, total + 3.0, "0.94 0.95 0.96");
            Rect(margin, y + 2.0, 3.0, total + 3.0, "0.45 0.62 0.72");
        }
        formula.Emit(content, left + (available - formula.Width()) * 0.5, y - padTop - formula.Ascent(),
            quoted ? "0.18 0.22 0.25" : "0.08 0.08 0.08");
        math.MarkUsed();
        y -= total;
        y -= quoted ? 7.0 : kDisplayMathBelow;
    }

    // The pre-math rendering of a formula block: its source in a tinted box.
    RAYOMD_MATH_COLD void RenderMathSource(const std::string& text) {
        double size = 10.5;
        double pitch = size * 1.35;   // what DrawTextLine advances by
        bool first = true;
        for (const auto& rawLine : SplitLines(text)) {
            for (const auto& line : WrapAsciiLiteral(rawLine, PAGE_W - margin * 2.0 - 24.0, size, true)) {
                size_t pageCount = pageStarts.size();
                Ensure(pitch + 8.0);
                if (pageStarts.size() != pageCount) first = true;
                // One tile per line, flush with its neighbours, so the tint never covers text.
                double pad = first ? 4.0 : 0.0;
                Rect(margin, y + pad, PAGE_W - margin * 2.0, pitch + pad, "0.97 0.97 0.95");
                DrawTextLine(margin + 12.0, size, line, "F3", "0.10 0.10 0.10", true);
                first = false;
            }
        }
        Rect(margin, y, PAGE_W - margin * 2.0, 4.0, "0.97 0.97 0.95");
        y -= 12.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;
        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = PAGE_W - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        y -= 3.0;

        std::vector<std::vector<WrappedAsciiLine>> wrapped(columns);
        static const std::string emptyCell;
        for (size_t r = 0; r < rows.size(); r++) {
            size_t maxLines = 1;
            for (size_t c = 0; c < columns; c++) {
                const std::string& cell = c < rows[r].size() ? rows[r][c] : emptyCell;
                WrapAsciiText(cell, std::max(16.0, colWidth - pad * 2.0), size, false, wrapped[c]);
                maxLines = std::max(maxLines, wrapped[c].size());
            }

            double rowHeight = maxLines * lh + pad * 2.0;
            Ensure(rowHeight + 5.0);
            double top = y;
            if (r == 0) Rect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");
            for (size_t c = 0; c < columns; c++) {
                double cellX = margin + c * colWidth;
                Rect(cellX, top, colWidth, rowHeight, "0.72 0.72 0.72", true);
                int align = c < aligns.size() ? aligns[c] : -1;
                for (size_t li = 0; li < wrapped[c].size(); li++) {
                    const WrappedAsciiLine& line = wrapped[c][li];
                    double tx = cellX + pad;
                    double lw = line.width;
                    if (align == 0) tx = cellX + (colWidth - lw) * 0.5;
                    else if (align == 1) tx = cellX + colWidth - pad - lw;
                    Text(tx, top - pad - size - li * lh, size, line.text, r == 0 ? "F2" : "F1", "0.08 0.08 0.08");
                }
            }
            y -= rowHeight;
        }
        y -= 9.0;
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = content;
        c += "q 0.68 0.68 0.68 RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, PAGE_W - margin);
        c += " ";
        AppendF(c, y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

// Page content is rendered straight into the output buffer. A buffer the caller reuses
// keeps its capacity; a new one starts at the size a document of this length usually
// needs, so it does not grow several times while the pages are rendered.
static void PrepareOutput(std::string& pdfBytes, size_t expectedBytes) {
    constexpr size_t kMaxInitialReserve = 64u * 1024u * 1024u;
    pdfBytes.clear();
    expectedBytes = std::min(expectedBytes, kMaxInitialReserve);
    if (pdfBytes.capacity() < expectedBytes) pdfBytes.reserve(expectedBytes);
}

static bool BuildStandardPdfBytes(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes) {
    std::vector<Block> blocks;
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Parse);
        blocks = ParseMarkdown(markdown);
    }
    PdfObjects pdf;
    int pagesId = pdf.Reserve();
    int fontRegularId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    int fontBoldId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>");
    int fontMonoId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>");

    ImageRegistry imageRegistry(options);
    PrepareOutput(pdfBytes, markdown.size() * 4 + 32 * 1024);
    StandardRenderer renderer(pdfBytes, options.style, options.margin, &imageRegistry);
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Render);
        renderer.Render(blocks);
    }
    RayoMd::Profiling::ScopedPhase assemblyProfile(RayoMd::Profiling::Phase::Assembly);
    std::vector<int> imageObjectIds = AddImageObjects(pdf, imageRegistry.Images());
    std::vector<std::vector<int>> annotationIds = AddLinkAnnotationObjects(pdf, renderer.PageLinks());
    std::vector<int> mathFontIds;
    if (renderer.MathUsed()) mathFontIds = AddMathFontObjects(pdf);

    std::vector<int> pageIds;
    const std::vector<size_t>& pageStarts = renderer.PageStarts();
    for (size_t pageIndex = 0; pageIndex < pageStarts.size(); pageIndex++) {
        const size_t pageEnd = pageIndex + 1 < pageStarts.size() ? pageStarts[pageIndex + 1] : pdfBytes.size();
        int contentId = pdf.AddStreamInPlace("", pageStarts[pageIndex], pageEnd - pageStarts[pageIndex]);
        std::string page;
        page.reserve(192);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, PAGE_W);
        page += " ";
        AppendF(page, PAGE_H);
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, fontRegularId);
        page += " 0 R /F2 ";
        AppendInt(page, fontBoldId);
        page += " 0 R /F3 ";
        AppendInt(page, fontMonoId);
        page += " 0 R";
        AppendMathFontResources(page, mathFontIds);
        page += " >>";
        AppendXObjectResources(page, imageObjectIds);
        page += " >> /Contents ";
        AppendInt(page, contentId);
        page += " 0 R";
        if (pageIndex < annotationIds.size()) AppendPageAnnotations(page, annotationIds[pageIndex]);
        page += " >>";
        pageIds.push_back(pdf.Add(std::move(page)));
    }

    std::string pages;
    pages.reserve(48 + pageIds.size() * 8);
    pages += "<< /Type /Pages /Kids [";
    for (int id : pageIds) {
        AppendInt(pages, id);
        pages += " 0 R ";
    }
    pages += "] /Count ";
    AppendSize(pages, pageIds.size());
    pages += " >>";
    pdf.Set(pagesId, std::move(pages));

    std::string catalog;
    catalog.reserve(options.embedSource ? 256 : 48);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R";
    if (options.embedSource) AddReversibleSource(pdf, catalog, markdown);
    catalog += " >>";
    int catalogId = pdf.Add(std::move(catalog));
    int infoId = pdf.Add("<< /Producer (RayoMD Native Standard PDF) /Creator (RayoMD) /Title (" +
        EscapeLiteral("Markdown Export") + ") >>");

    pdf.BuildInto(catalogId, infoId, pdfBytes, options.embedSource);
    return true;
}

static const TtfFont* GetCachedFont() {
    struct CachedFont {
        TtfFont font;
        bool loaded = font.Load();
    };
    static const CachedFont cached;
    return cached.loaded ? &cached.font : nullptr;
}

static bool BuildUnicodePdfBytes(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes) {
    const TtfFont* fontPtr = nullptr;
    {
        RayoMd::Profiling::ScopedPhase fontProfile(RayoMd::Profiling::Phase::Font);
        fontPtr = GetCachedFont();
    }
    if (!fontPtr) {
        g_lastError = 1;
        return false;
    }
    const TtfFont& font = *fontPtr;

    std::vector<Block> blocks;
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Parse);
        blocks = ParseMarkdown(markdown);
    }

    PdfObjects pdf;

    int fontFileId = pdf.Reserve();
    int cidMapId = pdf.Reserve();
    int toUnicodeId = pdf.Reserve();
    int descriptorId = pdf.Reserve();
    int cidFontId = pdf.Reserve();
    int type0FontId = pdf.Reserve();
    int pagesId = pdf.Reserve();

    ImageRegistry imageRegistry(options);
    PrepareOutput(pdfBytes, markdown.size() * 8 + 256 * 1024);
    Renderer renderer(pdfBytes, font, type0FontId, options.style, options.margin, &imageRegistry);
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Render);
        renderer.Render(blocks);
    }
    RayoMd::Profiling::ScopedPhase assemblyProfile(RayoMd::Profiling::Phase::Assembly);
    std::vector<int> imageObjectIds = AddImageObjects(pdf, imageRegistry.Images());
    std::vector<std::vector<int>> annotationIds = AddLinkAnnotationObjects(pdf, renderer.PageLinks());
    std::vector<int> mathFontIds;
    if (renderer.MathUsed()) mathFontIds = AddMathFontObjects(pdf);

    const CidList& used = renderer.UsedCids();
    std::string cidKey = MakeCidKey(used);
    auto cidMapBytes = MakeCidToGidMap(font, used, cidKey);
    auto toUnicodeBytes = MakeToUnicodeCMap(used, cidKey);

    // The cached font objects are referenced, not copied: the shared pointers keep them
    // alive until BuildInto below.
    std::shared_ptr<const std::string> fontFileObject = CachedFontFileObject(font, used, cidKey);
    pdf.SetView(fontFileId, *fontFileObject);

    std::string cidMapHead = "<< /Length ";
    AppendSize(cidMapHead, cidMapBytes->size());
    cidMapHead += " >>\nstream\n";
    pdf.SetStreamView(cidMapId, std::move(cidMapHead), *cidMapBytes);

    std::string toUnicodeHead = "<< /Length ";
    AppendSize(toUnicodeHead, toUnicodeBytes->size());
    toUnicodeHead += " >>\nstream\n";
    pdf.SetStreamView(toUnicodeId, std::move(toUnicodeHead), *toUnicodeBytes);

    std::string desc;
    desc.reserve(224);
    desc += "<< /Type /FontDescriptor /FontName /RayoMDSegoe /Flags 32 /FontBBox [";
    AppendInt(desc, font.Metric(font.xMin));
    desc += " ";
    AppendInt(desc, font.Metric(font.yMin));
    desc += " ";
    AppendInt(desc, font.Metric(font.xMax));
    desc += " ";
    AppendInt(desc, font.Metric(font.yMax));
    desc += "] /ItalicAngle 0 /Ascent ";
    AppendInt(desc, font.Metric(font.ascent));
    desc += " /Descent ";
    AppendInt(desc, font.Metric(font.descent));
    desc += " /CapHeight ";
    AppendInt(desc, (int)(font.Metric(font.ascent) * 0.72));
    desc += " /StemV 80 /FontFile2 ";
    AppendInt(desc, fontFileId);
    desc += " 0 R >>";
    pdf.Set(descriptorId, std::move(desc));

    std::string cidFont;
    cidFont.reserve(256);
    cidFont += "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /RayoMDSegoe"
        " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
        " /FontDescriptor ";
    AppendInt(cidFont, descriptorId);
    cidFont += " 0 R /CIDToGIDMap ";
    AppendInt(cidFont, cidMapId);
    cidFont += " 0 R /DW 500 /W ";
    cidFont += *MakeWidths(font, used, cidKey);
    cidFont += " >>";
    pdf.Set(cidFontId, std::move(cidFont));

    std::string type0;
    type0.reserve(160);
    type0 += "<< /Type /Font /Subtype /Type0 /BaseFont /RayoMDSegoe"
        " /Encoding /Identity-H /DescendantFonts [";
    AppendInt(type0, cidFontId);
    type0 += " 0 R] /ToUnicode ";
    AppendInt(type0, toUnicodeId);
    type0 += " 0 R >>";
    pdf.Set(type0FontId, std::move(type0));

    std::vector<int> pageIds;
    const std::vector<size_t>& pageStarts = renderer.PageStarts();
    for (size_t pageIndex = 0; pageIndex < pageStarts.size(); pageIndex++) {
        const size_t pageEnd = pageIndex + 1 < pageStarts.size() ? pageStarts[pageIndex + 1] : pdfBytes.size();
        int contentId = pdf.AddStreamInPlace("", pageStarts[pageIndex], pageEnd - pageStarts[pageIndex]);
        std::string page;
        page.reserve(160);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, PAGE_W);
        page += " ";
        AppendF(page, PAGE_H);
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, type0FontId);
        page += " 0 R";
        AppendMathFontResources(page, mathFontIds);
        page += " >>";
        AppendXObjectResources(page, imageObjectIds);
        page += " >> /Contents ";
        AppendInt(page, contentId);
        page += " 0 R";
        if (pageIndex < annotationIds.size()) AppendPageAnnotations(page, annotationIds[pageIndex]);
        page += " >>";
        pageIds.push_back(pdf.Add(std::move(page)));
    }

    std::string pages;
    pages.reserve(48 + pageIds.size() * 8);
    pages += "<< /Type /Pages /Kids [";
    for (int id : pageIds) {
        AppendInt(pages, id);
        pages += " 0 R ";
    }
    pages += "] /Count ";
    AppendSize(pages, pageIds.size());
    pages += " >>";
    pdf.Set(pagesId, std::move(pages));

    std::string catalog;
    catalog.reserve(options.embedSource ? 256 : 48);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R";
    if (options.embedSource) AddReversibleSource(pdf, catalog, markdown);
    catalog += " >>";
    int catalogId = pdf.Add(std::move(catalog));
    int infoId = pdf.Add("<< /Producer (RayoMD Native Tiny PDF) /Creator (RayoMD) /Title (" +
        EscapeLiteral("Markdown Export") + ") >>");

    pdf.BuildInto(catalogId, infoId, pdfBytes, options.embedSource);
    return true;
}

BuildResult BuildPdf(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes) {
    if (&markdown == &pdfBytes) {
        // Pages are rendered straight into pdfBytes, so the source needs storage of its own.
        const std::string source = markdown;
        return BuildPdf(source, options, pdfBytes);
    }
    const auto profileBefore = RayoMd::Profiling::Capture();
    if (options.embedSource) {
        if (markdown.size() > RayoMd::PdfSource::kMaxSourceBytes) {
            g_lastError = static_cast<int>(BuildError::SourceTooLarge);
            return { BuildError::SourceTooLarge };
        }
        if (!RayoMd::PdfSource::IsValidUtf8(markdown)) {
            g_lastError = static_cast<int>(BuildError::InvalidSourceUtf8);
            return { BuildError::InvalidSourceUtf8 };
        }
    }
    g_lastError = 0;
    bool built = IsAsciiDocument(markdown)
        ? BuildStandardPdfBytes(markdown, options, pdfBytes)
        : BuildUnicodePdfBytes(markdown, options, pdfBytes);
    if (built && options.embedSource && pdfBytes.size() > RayoMd::PdfSource::kMaxPdfBytes) {
        pdfBytes.clear();
        g_lastError = static_cast<int>(BuildError::ReversiblePdfTooLarge);
        built = false;
    }
    RayoMd::Profiling::EmitDelta("build", profileBefore, RayoMd::Profiling::Capture());
    return built ? BuildResult{} : BuildResult{ static_cast<BuildError>(g_lastError) };
}

bool BuildPdfBytes(const std::string& markdown, const BuildOptions& options, std::string& pdfBytes) {
    return BuildPdf(markdown, Internal::PdfOptionsFromLegacy(options), pdfBytes).Ok();
}

bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes) {
    BuildOptions options;
    options.styleIdx = styleIdx;
    options.marginIdx = marginIdx;
    return BuildPdfBytes(markdown, options, pdfBytes);
}

} // namespace TinyPdf
