#include "rayomd/tiny_pdf.h"
#include "flate.h"
#include "export_options.h"
#include "rayomd_pdf_source.h"
#include "inline_markdown.h"
#include "../common/profiling.h"
#include "../common/text_utils.h"
#include "book.h"
#include "contents.h"
#include "markdown_parser.h"
#include "highlight.h"
#include "math_layout.h"
#include "pdfa.h"
#include "theme.h"

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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
// This file reaches GCC's inline-unit-growth limit at -O3, so which calls get inlined
// shifts with any change to it; hot helpers and lambdas say so explicitly.
// Rare paths (words wider than a line, table rows taller than a page, font discovery) are
// out of line and optimised for size, so they stay away from the hot text path.
// RAYOMD_SHARED keeps one copy of a sizeable helper that several block types call, instead
// of one inlined into each or a clone for the constant arguments of one caller.
#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_HOT_INLINE inline __attribute__((always_inline))
#define RAYOMD_HOT_LAMBDA __attribute__((always_inline))
#define RAYOMD_COLD __attribute__((cold, noinline))
#if defined(__clang__)
#define RAYOMD_SHARED __attribute__((noinline))
#else
#define RAYOMD_SHARED __attribute__((noinline, noclone))
#endif
#else
#define RAYOMD_HOT_INLINE inline
#define RAYOMD_HOT_LAMBDA
#define RAYOMD_COLD
#define RAYOMD_SHARED
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

// Writes the UTF-16 code units of `str` at `out`, which has room for one per byte, and returns
// the end. A sequence that is not UTF-8 becomes U+FFFD, as MultiByteToWideChar makes it on
// Windows; std::wstring_convert threw, which ended the process.
static wchar_t* WriteUtf8Units(wchar_t* out, std::string_view str) {
    for (size_t at = 0; at < str.size();) {
        const unsigned char lead = (unsigned char)str[at];
        if (lead < 0x80) {
            *out++ = (wchar_t)lead;
            at++;
            continue;
        }
        uint32_t codePoint = 0;
        size_t length = 0;
        if (!RayoMd::Text::DecodeUtf8(str, at, codePoint, length)) {
            *out++ = (wchar_t)0xFFFD;
            at++;
            continue;
        }
        at += length;
        if (codePoint >= 0x10000) {
            codePoint -= 0x10000;
            *out++ = (wchar_t)(0xD800 + (codePoint >> 10));
            *out++ = (wchar_t)(0xDC00 + (codePoint & 0x3FF));
        } else {
            *out++ = (wchar_t)codePoint;
        }
    }
    return out;
}

static std::wstring Utf8ToWideFallback(std::string_view str) {
    if (str.empty()) return L"";
    // UTF-16 never needs more code units than UTF-8 needs bytes.
    std::wstring wstr(str.size(), 0);
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstr[0], (int)wstr.size());
    if (len <= 0) return L"";
    wstr.resize((size_t)len);
#else
    wstr.resize((size_t)(WriteUtf8Units(&wstr[0], str) - wstr.data()));
#endif
    return wstr;
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

// Appends Utf8ToWide(str) to `out`. ASCII and, without simdutf, other text are written straight
// into `out`, so a caller that reuses the string converts without allocating.
static void AppendUtf8ToWide(std::wstring& out, std::string_view str) {
    if (str.empty()) return;
    if (IsAllAscii(str)) {
        out.append(str.begin(), str.end());
        return;
    }
#if defined(RAYOMD_USE_SIMDUTF)
    out += Utf8ToWide(str);
#else
    const size_t base = out.size();
    out.resize(base + str.size());
#if defined(_WIN32)
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &out[base], (int)str.size());
    out.resize(base + (len > 0 ? (size_t)len : 0));
#else
    out.resize((size_t)(WriteUtf8Units(&out[base], str) - out.data()));
#endif
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

static thread_local int g_lastError = 0;

int GetLastError() {
    return g_lastError;
}

using Internal::ResolveMarginPoints;
using Internal::ResolvePageSize;
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
    uint32_t sfntVersion = 0x00010000;  // of the font's table directory; a collection starts "ttcf"
    bool loaded = false;

    // RAYOMD_FONT first, then the sans fonts where the common systems install them, then,
    // on systems other than Windows, any regular sans TrueType font under the usual font
    // directories.
    bool Load() {
#ifdef _WIN32
        if (const wchar_t* explicitFont = _wgetenv(L"RAYOMD_FONT")) {
            if (*explicitFont && TryLoad(std::wstring(explicitFont))) return true;
        }
        wchar_t winDir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;
        const std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";
        for (const wchar_t* name : { L"segoeui.ttf", L"arial.ttf", L"tahoma.ttf", L"verdana.ttf" }) {
            if (TryLoad(fontsDir + name)) return true;
        }
        return false;
#else
        if (const char* explicitFont = std::getenv("RAYOMD_FONT")) {
            if (*explicitFont && TryLoad(std::string(explicitFont))) return true;
        }
        static const char* const kKnownFonts[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",              // Debian, Ubuntu
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",            // Fedora, RHEL
            "/usr/share/fonts/TTF/DejaVuSans.ttf",                          // Arch
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",                       // Alpine, older Fedora
            "/usr/share/fonts/truetype/DejaVuSans.ttf",                     // openSUSE
            "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",                 // FreeBSD
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            "/usr/share/fonts/gnu-free/FreeSans.ttf",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",         // macOS
            "/Library/Fonts/Arial Unicode.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
            "/system/fonts/Roboto-Regular.ttf",                             // Android, Termux
        };
        for (const char* path : kKnownFonts) {
            if (TryLoad(std::string(path))) return true;
        }
        return LoadFromFontDirectories();
#endif
    }

    template <typename Path>
    bool TryLoad(const Path& path) {
        bytes.clear();
        tables.clear();
        glyphForBmp.clear();
        advances.clear();
        loca.clear();
        for (auto& width : widthCache) width.store(0, std::memory_order_relaxed);
        loaded = ReadWholeFile(path, bytes) && Parse();
        return loaded;
    }

#ifndef _WIN32
    // The last resort: a walk of the usual font directories (at most 20,000 entries) for
    // TrueType files, trying the most promising names first: sans before serif, regular
    // before bold, italic, light, condensed, monospaced, emoji or symbol faces.
    RAYOMD_COLD bool LoadFromFontDirectories() {
        std::vector<std::string> roots = { "/usr/share/fonts", "/usr/local/share/fonts", "/System/Library/Fonts",
            "/Library/Fonts" };
        if (const char* home = std::getenv("HOME")) {
            for (const char* sub : { "/.local/share/fonts", "/.fonts", "/Library/Fonts" }) roots.push_back(std::string(home) + sub);
        }
        std::vector<std::string> paths;
        std::vector<int> scores;
        size_t visited = 0;
        for (const std::string& root : roots) {
            std::error_code error;
            std::filesystem::recursive_directory_iterator entries(root,
                std::filesystem::directory_options::skip_permission_denied, error);
            for (const std::filesystem::recursive_directory_iterator end; !error && entries != end && visited < 20000;
                entries.increment(error), visited++) {
                std::string name = entries->path().filename().string();
                for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ttf") != 0) continue;
                int score = 0;
                if (name.find("sans") != std::string::npos) score += 4;
                if (name.find("regular") != std::string::npos) score += 2;
                for (const char* face : { "bold", "italic", "oblique", "light", "thin", "black", "heavy", "medium",
                         "condensed", "narrow", "mono", "emoji", "symbol", "math" }) {
                    if (name.find(face) != std::string::npos) score -= 10;
                }
                paths.push_back(entries->path().string());
                scores.push_back(score);
            }
        }
        // The best remaining candidate, the first found among equals, up to 32 attempts. Scores
        // lie between -140 and 6, so kTried marks a file that failed to load.
        const int kTried = -1000000;
        for (int attempt = 0; attempt < 32; attempt++) {
            size_t best = paths.size();
            for (size_t index = 0; index < paths.size(); index++) {
                if (scores[index] != kTried && (best == paths.size() || scores[index] > scores[best])) best = index;
            }
            if (best == paths.size()) break;
            if (TryLoad(paths[best])) return true;
            scores[best] = kTried;
        }
        return false;
    }
#endif

    bool Parse() {
        if (bytes.size() < 12) return false;
        // A TrueType collection (.ttc) holds several fonts: the first one is used. Its table
        // offsets count from the start of the file, as those of a single font do.
        size_t directory = 0;
        if (memcmp(bytes.data(), "ttcf", 4) == 0) {
            if (ReadU32(bytes, 8) == 0) return false;
            directory = ReadU32(bytes, 12);
        }
        sfntVersion = ReadU32(bytes, directory);
        uint16_t numTables = ReadU16(bytes, directory + 4);
        if (directory + 12 + (size_t)numTables * 16 > bytes.size()) return false;

        for (uint16_t i = 0; i < numTables; i++) {
            size_t off = directory + 12 + (size_t)i * 16;
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

    // Whether the font has a glyph of its own for `cp` (GlyphFor falls back to '?').
    bool HasGlyph(uint32_t cp) const {
        return cp < glyphForBmp.size() && glyphForBmp[cp] != 0;
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

// CIDs are code points of the BMP; a character beyond it is shown as '?'.
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

// Equality of two views of text; views of the same bytes are equal without reading them.
static RAYOMD_HOT_INLINE bool SameText(std::string_view a, std::string_view b) {
    return (a.data() == b.data() && a.size() == b.size()) || a == b;
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

    // Always inlined: a copy of a known size and a pointer step. Left to GCC, the paint
    // functions called Lit<2> out of line, and growth elsewhere in this file moves more.
    template <size_t N>
    RAYOMD_HOT_INLINE void Lit(const char (&text)[N]) {
        memcpy(cursor, text, N - 1);
        cursor += N - 1;
    }
    RAYOMD_HOT_INLINE void Bytes(const char* text, size_t size) {
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

// The checkbox of a task list item, a square `side` points wide standing on (x, bottom), with
// a check mark when the task is done. Paths, not a glyph, so every font shows it.
static void AppendCheckbox(std::string& content, double x, double bottom, double side, bool done) {
    TailWriter out(content, 96 + kOperandBytes * 10);
    out.Lit("q 0.42 0.47 0.53 RG 0.7 w ");
    out.Fixed(x);
    out.Lit(" ");
    out.Fixed(bottom);
    out.Lit(" ");
    out.Fixed(side);
    out.Lit(" ");
    out.Fixed(side);
    out.Lit(" re S");
    if (done) {
        out.Lit(" 0.10 0.10 0.10 RG 1.2 w 1 J 1 j ");
        out.Fixed(x + side * 0.2);
        out.Lit(" ");
        out.Fixed(bottom + side * 0.52);
        out.Lit(" m ");
        out.Fixed(x + side * 0.42);
        out.Lit(" ");
        out.Fixed(bottom + side * 0.25);
        out.Lit(" l ");
        out.Fixed(x + side * 0.82);
        out.Lit(" ");
        out.Fixed(bottom + side * 0.8);
        out.Lit(" l S");
    }
    out.Lit(" Q\n");
}

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

    // Characters shown without a glyph of their own, as '?': those the font lacks and those
    // beyond the BMP.
    uint32_t missing = 0;

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
        uint16_t cid = (uint16_t)cp;
        if (cp > 0xffff || !font.HasGlyph(cp)) {
            cid = CidForCodepoint(font, cp);
            usedCids.missing += cp >= 0x20;
        }
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

// The key of the font objects of a document: the font, then the CIDs it shows.
static std::string MakeCidKey(const TtfFont& font, const CidList& used) {
    std::string key;
    key.reserve(sizeof(&font) + used.size() * 2);
    const TtfFont* const identity = &font;
    key.append(reinterpret_cast<const char*>(&identity), sizeof(identity));
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

    // A new object whose whole body is `body`.
    int AddView(std::string_view body) {
        objects.emplace_back();
        objects.back().view = body;
        return (int)objects.size();
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
    // With `fileId` the trailer gets an /ID of the file's body, which PDF/A requires.
    void BuildInto(int rootId, int infoId, std::string& pdf, bool pdf20 = false, bool fileId = false) const {
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
        char id[32];
        memset(id, '0', sizeof(id));
        const size_t total = xref + WriteTail(nullptr, offsets, xref, rootId, infoId, fileId ? id : nullptr);

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

        WriteTail(base + xref, offsets, xref, rootId, infoId, fileId ? id : nullptr);
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
        if (fileId) {
            // The identifier is that of everything before the cross-reference table.
            Internal::FileIdentifier(std::string_view(base, xref), id);
            WriteTail(base + xref, offsets, xref, rootId, infoId, id);
        }
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
    size_t WriteTail(char* out, const std::vector<size_t>& offsets, size_t xref, int rootId, int infoId,
        const char* fileId = nullptr) const {
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
        text(" 0 R");
        if (fileId) {
            text(" /ID [<");
            text(std::string_view(fileId, 32));
            text("> <");
            text(std::string_view(fileId, 32));
            text(">]");
        }
        text(" >>\nstartxref\n");
        number(xref);
        text("\n%%EOF\n");
        return size;
    }

    std::vector<Object> objects;
};

// --compress: a thread's encoder tables and output buffer, kept from export to export so a
// batch worker allocates them once.
struct FlateWork {
    RayoMd::Flate::Scratch scratch;
    std::string out;
};

RAYOMD_COLD static FlateWork& ThreadFlateWork() {
    static thread_local FlateWork work;
    return work;
}

// `data` FlateDecode-compressed into work.out. Returns its size, or 0 when the stream would not
// be smaller with its dictionary's "/Filter /FlateDecode " added; `data` then stays as it is.
RAYOMD_COLD static size_t Deflate(std::string_view data, FlateWork& work) {
    constexpr size_t kFilterBytes = 21;
    if (data.size() > RayoMd::Flate::kMaxInput) return 0;
    const size_t bound = RayoMd::Flate::ZlibBound(data.size());
    if (work.out.size() < bound) work.out.resize(bound);
    const size_t size = RayoMd::Flate::ZlibCompress(data, reinterpret_cast<uint8_t*>(&work.out[0]), work.scratch);
    return size + kFilterBytes < data.size() ? size : 0;
}

// A thread does not keep the output buffer of a large stream.
RAYOMD_COLD static void TrimFlateWork(FlateWork& work) {
    constexpr size_t kRetainedBytes = 4u * 1024u * 1024u;
    if (work.out.capacity() > kRetainedBytes) std::string().swap(work.out);
}

// --compress: every page's content stream FlateDecode-compressed where that makes it smaller.
// `pdfBytes` holds the streams and nothing else, page k's from starts[k]. They are moved together
// at the front, none longer than before, so none overwrites one not yet read; `starts` and
// `flate` then say where each lies and whether it is compressed.
RAYOMD_COLD static void CompressPageContents(std::string& pdfBytes, std::vector<size_t>& starts, std::vector<bool>& flate) {
    if (starts.empty()) return;
    FlateWork& work = ThreadFlateWork();
    flate.assign(starts.size(), false);
    size_t write = starts[0];
    for (size_t page = 0; page < starts.size(); page++) {
        const size_t start = starts[page];
        const size_t end = page + 1 < starts.size() ? starts[page + 1] : pdfBytes.size();
        const size_t packed = Deflate(std::string_view(pdfBytes.data() + start, end - start), work);
        starts[page] = write;
        if (packed != 0) {
            memcpy(&pdfBytes[write], work.out.data(), packed);
            write += packed;
            flate[page] = true;
        } else {
            memmove(&pdfBytes[write], pdfBytes.data() + start, end - start);
            write += end - start;
        }
    }
    pdfBytes.resize(write);
    TrimFlateWork(work);
}

// --compress: `data` FlateDecode-compressed into `storage` when that makes it smaller.
RAYOMD_COLD static bool CompressPayload(std::string_view data, std::string& storage) {
    FlateWork& work = ThreadFlateWork();
    const size_t packed = Deflate(data, work);
    if (packed != 0) storage.assign(work.out.data(), packed);
    TrimFlateWork(work);
    return packed != 0;
}

// The stream object `id` with the payload `data`, referenced, not copied. With `compress` the
// payload is compressed into `storage`, which then has to outlive BuildInto, where that makes
// it smaller.
static void SetPayloadStream(PdfObjects& pdf, int id, std::string_view data, bool compress, std::string& storage) {
    std::string head;
    if (compress && CompressPayload(data, storage)) {
        data = storage;
        head = "<< /Filter /FlateDecode /Length ";
    } else {
        head = "<< /Length ";
    }
    AppendSize(head, data.size());
    head += " >>\nstream\n";
    pdf.SetStreamView(id, std::move(head), data);
}

constexpr size_t kMaxImageBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxDecodedImageBytes = 96u * 1024u * 1024u;
// With `metadata` false the caller writes the XMP metadata, with the reversible profile's
// properties: a PDF/A file has one packet for both.
static void AddReversibleSource(PdfObjects& pdf, std::string& catalog, const std::string& markdown, bool metadata = true) {
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

    if (metadata) {
        const std::string xmp = RayoMd::PdfSource::BuildXmpMetadata(markdown, RAYOMD_VERSION);
        catalog += " /Metadata ";
        AppendInt(catalog, pdf.AddStream("/Type /Metadata /Subtype /XML", xmp));
        catalog += " 0 R";
    }
    catalog += " /Names << /EmbeddedFiles << /Names [(source.md) ";
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

// Adds the rectangle of link text that starts at x on `baseline` and is `width` wide. A piece
// of the same link that goes on where the last one ended (link text in several styles)
// extends it, so a link is one annotation per line.
static void AddLinkRect(std::vector<LinkRect>& links, double x, double baseline, double width, double size,
    std::string_view url) {
    const double y1 = baseline - 1.0;
    const double y2 = baseline + size * 1.05;
    if (!links.empty()) {
        LinkRect& last = links.back();
        if (last.x2 == x && last.y1 == y1 && last.y2 == y2 && last.url == url) {
            last.x2 = x + width;
            return;
        }
    }
    links.push_back({ x, y1, x + width, y2, std::string(url) });
}

static bool IsHttpUrl(std::string_view src) {
    return StartsWith(src, "http://") || StartsWith(src, "https://");
}

static char* WriteEscapedLiteral(char* out, std::string_view text);

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

    // A book's file (BuildBookPdf): its relative images resolve from its own directory, and must
    // still lie in the book's.
    void SetChapter(const std::string& pathUtf8) {
        chapterBase = PathFromUtf8(pathUtf8).parent_path();
        if (chapterBase.empty()) chapterBase = ".";
        chapterRootReady = false;
    }

#ifdef _WIN32
    DirectLocalImageResult TryOpenDirect(
        const std::string& src, std::string& key, WinLocalImageFile& file) {
        if (src.empty() || IsHttpUrl(src)) return DirectLocalImageResult::Rejected;
        if (allowUnsafe) return DirectLocalImageResult::NotApplicable;
        if (!hasSourcePath || IsUnsafeLocalImageSource(src)) return DirectLocalImageResult::Rejected;
        if (!chapterBase.empty()) return DirectLocalImageResult::NotApplicable;

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
        const std::filesystem::path* base = &root;
        if (!chapterBase.empty()) {
            if (!chapterRootReady) {
                chapterRootValid = TryCanonicalForPolicy(chapterBase, chapterRoot);
                chapterRootReady = true;
            }
            if (!chapterRootValid) return false;
            base = &chapterRoot;
        }
        std::filesystem::path normalized;
        if (!TryCanonicalForPolicy(*base / PathFromUtf8(src), normalized) ||
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
        if (path.is_relative() && hasSourcePath) path = (chapterBase.empty() ? sourceBase : chapterBase) / path;

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
    bool chapterRootReady = false;
    bool chapterRootValid = false;
    std::filesystem::path sourceBase;
    std::filesystem::path safeBase;
    std::filesystem::path safeRoot;
    std::filesystem::path chapterBase;   // a book's file's directory, or empty
    std::filesystem::path chapterRoot;   // and canonical
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

    // The book's file whose images follow (BuildBookPdf).
    void SetChapter(const std::string& pathUtf8) { localPolicy.SetChapter(pathUtf8); }

    // An image the document shows, by its source: false, and one more failed image, when the
    // policy rejects it or it cannot be loaded or decoded; RenderImage then shows its alt text.
    bool Resolve(const std::string& src, const std::string& alt, int& index) {
        const bool resolved = ResolveSource(src, alt, index);
        failed += !resolved;
        return resolved;
    }
    uint32_t FailedImages() const { return failed; }

    // An image a theme names, such as its logo: the caller's own configuration, as a font is, so
    // outside the policy for images in documents. False, and one more failed image, when it cannot
    // be read or decoded.
    RAYOMD_COLD bool ResolveThemeFile(const std::string& pathUtf8, int& index) {
        std::filesystem::path normalized;
        if (!TryCanonicalForPolicy(PathFromUtf8(pathUtf8), normalized)) {
            failed++;
            return false;
        }
        const std::string key = "file:" + PathToUtf8(normalized);
        auto known = indexByKey.find(key);
        if (known != indexByKey.end()) {
            index = known->second;
            return true;
        }
        SharedPdfImage sharedImage = LoadDecodedImageFromCache(key);
        if (!sharedImage) {
            PdfImage image;
            std::vector<uint8_t> bytes;
            if (IsKnownFailure(key) || !ReadLocalImageFile(PathToUtf8(normalized), bytes) ||
                bytes.size() > kMaxImageBytes || !DecodeImageBytes(bytes, image)) {
                StoreFailure(key);
                failed++;
                return false;
            }
            sharedImage = StoreDecodedImageInCache(key, std::move(image));
        }
        index = (int)images.size();
        indexByKey[key] = index;
        images.push_back(std::move(sharedImage));
        return true;
    }

private:
    bool ResolveSource(const std::string& src, const std::string& alt, int& index) {
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

public:
    const PdfImage& Get(int index) const {
        return *images[(size_t)index];
    }

    const std::vector<SharedPdfImage>& Images() const { return images; }

private:
    const PdfOptions& options;
    LocalImagePolicy localPolicy;
    std::vector<SharedPdfImage> images;
    std::unordered_map<std::string, int> indexByKey;
    uint32_t failed = 0;
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

// A link target as the body of a PDF literal string. /URI takes 7-bit ASCII, so the target's
// UTF-8 bytes from 0x80 are percent-encoded (an IRI as a URI, RFC 3987); `winAnsi` targets
// come from transcoded text and turn back into UTF-8 first. Written through a pointer, at most
// three bytes a byte: one push_back a byte cost a library call where GCC did not inline it.
static void AppendUriLiteral(std::string& out, std::string_view url, bool winAnsi) {
    std::string utf8;
    if (winAnsi && !IsAllAscii(url)) {
        utf8 = RayoMd::Text::WinAnsiToUtf8(url);
        url = utf8;
    }
    static const char kHex[] = "0123456789ABCDEF";
    const size_t at = out.size();
    out.resize(at + url.size() * 3);
    char* cursor = &out[at];
    for (const char ch : url) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 0x80) {
            cursor[0] = '%';
            cursor[1] = kHex[byte >> 4];
            cursor[2] = kHex[byte & 0x0F];
            cursor += 3;
            continue;
        }
        if (byte < 32 || byte == 127) continue;
        if (ch == '(' || ch == ')' || ch == '\\') *cursor++ = '\\';
        *cursor++ = ch;
    }
    out.resize((size_t)(cursor - out.data()));
}

// ---- Headings: the outline (bookmarks) and the targets of internal links -----------------

// A heading as drawn: its level, its text as the parser left it (WinAnsi bytes in a Latin
// document of the standard renderer, else UTF-8), its page and the top of its first line.
// The text lies in the document's blocks, which outlive the assembly of the file.
struct HeadingMark {
    int level = 1;
    std::string_view text;
    size_t page = 0;
    double top = 0.0;
};

// Heading text without its footnote references (kMathTextOpen, kNoteMark ... kMathTextClose).
RAYOMD_COLD static std::string WithoutFootnotes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size();) {
        if (text[at] == Internal::kMathTextOpen && at + 1 < text.size() && text[at + 1] == Internal::kNoteMark) {
            const size_t close = text.find(Internal::kMathTextClose, at);
            if (close != std::string_view::npos) {
                at = close + 1;
                continue;
            }
        }
        out += text[at++];
    }
    return out;
}

// The headings of a document with footnotes, without their references, for anchors, bookmarks
// and titles; `titles` holds the text of those that had one.
RAYOMD_COLD static const std::vector<HeadingMark>& TitledHeadings(const std::vector<HeadingMark>& headings,
    std::vector<HeadingMark>& out, std::vector<std::string>& titles) {
    titles.reserve(headings.size());     // no growth: the marks see into it
    out = headings;
    for (HeadingMark& heading : out) {
        if (heading.text.find(Internal::kNoteMark) == std::string_view::npos) continue;
        titles.push_back(WithoutFootnotes(heading.text));
        heading.text = titles.back();
    }
    return out;
}

// Writes `text`, a heading as the parser left it, as a PDF text string: ASCII as a literal,
// other text as UTF-16BE with its byte order mark. Control bytes, the formula markers among
// them, are dropped. Needs room for text.size() * 4 + 6 bytes.
static char* WriteTextString(char* out, std::string_view text, bool winAnsi) {
    if (IsAllAscii(text)) {
        *out++ = '(';
        out = WriteEscapedLiteral(out, text);
        *out++ = ')';
        return out;
    }
    std::string utf8;
    if (winAnsi) utf8 = RayoMd::Text::WinAnsiToUtf8(text);
    memcpy(out, "<FEFF", 5);
    out += 5;
    for (const wchar_t unit : Utf8ToWide(winAnsi ? std::string_view(utf8) : text)) {
        if ((uint32_t)unit >= 0x20) out = WriteHex4(out, (uint16_t)unit);
    }
    *out++ = '>';
    return out;
}

// Adds the outline of `headings`, nested by level with every entry open, and its entry to
// `catalog`. The entries are written one after another into `storage`, which must outlive
// BuildInto, with one growth instead of a string each.
RAYOMD_COLD static void AddOutline(PdfObjects& pdf, std::string& catalog, const std::vector<HeadingMark>& headings,
    const std::vector<int>& pageIds, bool winAnsi, std::string& storage) {
    if (headings.empty() || pageIds.empty()) return;
    constexpr size_t kNone = std::string::npos;
    constexpr size_t kRootBytes = 128;      // the root's text
    // An entry's text but its title: five object numbers of up to ten digits, a count and the top.
    constexpr size_t kEntryBytes = 216;
    struct Entry {
        size_t parent = kNone, first = kNone, last = kNone, previous = kNone, next = kNone;
        size_t descendants = 0;
        size_t end = 0;                     // of its text in `storage`
    };
    const size_t count = headings.size();
    std::vector<Entry> entries(count);
    Entry root;
    // The entries that enclose the next one, by rising level: levels 1 to 6 nest six deep.
    size_t open[6];
    size_t depth = 0;
    size_t bytes = kRootBytes;              // about what the outline takes with ASCII titles
    for (size_t index = 0; index < count; index++) {
        while (depth > 0 && headings[open[depth - 1]].level >= headings[index].level) depth--;
        Entry& entry = entries[index];
        Entry& parent = depth > 0 ? entries[open[depth - 1]] : root;
        entry.parent = depth > 0 ? open[depth - 1] : kNone;
        if (parent.last != kNone) {
            entries[parent.last].next = index;
            entry.previous = parent.last;
        } else {
            parent.first = index;
        }
        parent.last = index;
        for (size_t at = 0; at < depth; at++) entries[open[at]].descendants++;
        open[depth++] = index;
        bytes += headings[index].text.size() + 128;
    }

    // The root's object, then the entries' in order: entry `index` is object rootId + 1 + index.
    // Each is written in a scratch buffer, which stays in the cache and grows only for a longer
    // title, and then appended, so that `storage` gets only the bytes it keeps.
    const int rootId = pdf.Reserve();
    const auto id = [rootId](size_t index) { return (size_t)rootId + 1 + index; };
    const auto lit = [](char* out, std::string_view text) {
        memcpy(out, text.data(), text.size());
        return out + text.size();
    };
    const auto number = [](char* out, size_t value) { return std::to_chars(out, out + 20, value).ptr; };
    storage.reserve(storage.size() + bytes);
    const size_t rootStart = storage.size();
    std::string scratch(kRootBytes, '\0');
    char* out = lit(&scratch[0], "<< /Type /Outlines /First ");
    out = number(out, id(root.first));
    out = lit(out, " 0 R /Last ");
    out = number(out, id(root.last));
    out = lit(out, " 0 R /Count ");
    out = number(out, count);
    out = lit(out, " >>");
    storage.append(scratch.data(), (size_t)(out - scratch.data()));
    root.end = storage.size();
    for (size_t index = 0; index < count; index++) {
        const HeadingMark& heading = headings[index];
        Entry& entry = entries[index];
        const size_t room = heading.text.size() * 4 + 6 + kEntryBytes;
        if (scratch.size() < room) scratch.resize(room);
        out = lit(&scratch[0], "<< /Title ");
        out = WriteTextString(out, heading.text, winAnsi);
        out = lit(out, " /Parent ");
        out = number(out, entry.parent == kNone ? (size_t)rootId : id(entry.parent));
        out = lit(out, " 0 R");
        if (entry.previous != kNone) {
            out = lit(out, " /Prev ");
            out = number(out, id(entry.previous));
            out = lit(out, " 0 R");
        }
        if (entry.next != kNone) {
            out = lit(out, " /Next ");
            out = number(out, id(entry.next));
            out = lit(out, " 0 R");
        }
        if (entry.first != kNone) {
            out = lit(out, " /First ");
            out = number(out, id(entry.first));
            out = lit(out, " 0 R /Last ");
            out = number(out, id(entry.last));
            out = lit(out, " 0 R /Count ");
            out = number(out, entry.descendants);
        }
        out = lit(out, " /Dest [");
        out = number(out, (size_t)pageIds[std::min(heading.page, pageIds.size() - 1)]);
        out = lit(out, " 0 R /XYZ null ");
        out = RayoMd::Text::WriteFixed2(out, heading.top + 4.0);
        out = lit(out, " null] >>");
        storage.append(scratch.data(), (size_t)(out - scratch.data()));
        entry.end = storage.size();
    }

    // The text is complete and no longer moves.
    const std::string_view text(storage);
    pdf.SetView(rootId, text.substr(rootStart, root.end - rootStart));
    size_t start = root.end;
    for (const Entry& entry : entries) {
        pdf.AddView(text.substr(start, entry.end - start));
        start = entry.end;
    }
    catalog += " /Outlines ";
    AppendInt(catalog, rootId);
    catalog += " 0 R";
}

// ---- Document metadata from the YAML front matter ---------------------------------------------

// What the YAML front matter, which the parser skips, says about the document. Read are scalars
// on the key's line, plain (without a comment) or quoted, and lists, in brackets or as "- " lines
// under the key; block scalars ("|", ">") and maps are not. Several authors are joined with "; "
// and keywords with ", ", as Pandoc's PDF metadata has them, and the subject falls back to the
// description. All empty without a front matter.
struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string subject;
    std::string keywords;
    std::string lang;   // a well-formed language tag, such as en-US
    std::string date;   // as written, for a theme's {date}
};

static std::string_view TrimYamlSpace(std::string_view text) {
    const auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

// A quoted scalar from its opening quote: "\x" in double quotes is x, '' in single quotes is ',
// and it ends at its closing quote. `end` is set past that quote.
static std::string YamlQuoted(std::string_view value, size_t& end) {
    const char quote = value.front();
    std::string text;
    size_t index = 1;
    for (; index < value.size(); index++) {
        char ch = value[index];
        if (quote == '"' && ch == '\\' && index + 1 < value.size()) {
            ch = value[++index];
        } else if (ch == quote) {
            if (quote == '"' || index + 1 >= value.size() || value[index + 1] != '\'') break;
            index++;
        }
        text += ch;
    }
    end = std::min(index + 1, value.size());
    return text;
}

static std::string YamlScalar(std::string_view value) {
    value = TrimYamlSpace(value);
    if (value.empty() || value.front() == '|' || value.front() == '>') return std::string();
    if (value.front() == '"' || value.front() == '\'') {
        size_t end = 0;
        return YamlQuoted(value, end);
    }
    return std::string(TrimYamlSpace(value.substr(0, value.find(" #"))));
}

// The items of a flow sequence, "[a, 'b, c']".
static void YamlFlowItems(std::string_view value, std::vector<std::string>& items) {
    size_t at = 1;
    while (at < value.size()) {
        while (at < value.size() && (value[at] == ' ' || value[at] == '\t')) at++;
        if (at >= value.size() || value[at] == ']') return;
        std::string item;
        if (value[at] == '"' || value[at] == '\'') {
            size_t end = 0;
            item = YamlQuoted(value.substr(at), end);
            at += end;
        } else {
            const size_t end = std::min(value.find_first_of(",]", at), value.size());
            item = std::string(TrimYamlSpace(value.substr(at, end - at)));
            at = end;
        }
        if (!item.empty()) items.push_back(std::move(item));
        const size_t next = value.find_first_of(",]", at);
        if (next == std::string_view::npos || value[next] == ']') return;
        at = next + 1;
    }
}

// `entry` as a map entry, "key: value", when it is not a quoted scalar or a flow sequence.
static bool YamlMapEntry(std::string_view entry, std::string_view& key, std::string_view& value) {
    if (entry.empty() || entry.front() == '"' || entry.front() == '\'' || entry.front() == '[') return false;
    for (size_t colon = entry.find(':'); colon != std::string_view::npos; colon = entry.find(':', colon + 1)) {
        if (colon + 1 == entry.size() || entry[colon + 1] == ' ' || entry[colon + 1] == '\t') {
            key = entry.substr(0, colon);
            value = entry.substr(colon + 1);
            return colon != 0;
        }
    }
    return false;
}

// A language tag in the form PDF's /Lang takes (BCP 47): letters, then subtags of one to eight
// letters or digits after hyphens.
static bool IsLanguageTag(std::string_view tag) {
    if (tag.empty() || tag.size() > 35) return false;
    size_t run = 0;
    bool primary = true;
    for (const char ch : tag) {
        if (ch == '-') {
            if (run == 0) return false;
            run = 0;
            primary = false;
            continue;
        }
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if ((!letter && (primary || ch < '0' || ch > '9')) || ++run > 8) return false;
    }
    return run != 0;
}

RAYOMD_COLD static DocumentMetadata FrontMatterMetadata(std::string_view markdown) {
    DocumentMetadata metadata;
    size_t at = markdown.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const size_t dashes = markdown.find_first_not_of(" \t", at);
    if (dashes == std::string_view::npos || markdown.compare(dashes, 3, "---") != 0) return metadata;
    constexpr std::string_view kKeys[] = { "title", "author", "subject", "keywords", "lang", "description", "date" };
    constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
    std::vector<std::string> items[kKeyCount];
    bool found[kKeyCount] = {};
    size_t listKey = kKeyCount;   // the key whose "- " lines follow, if any
    bool nameWanted = false;      // the list's last item is a map that has shown no name yet
    bool closed = false;
    for (bool first = true; at < markdown.size() && !closed; first = false) {
        size_t end = markdown.find('\n', at);
        if (end == std::string_view::npos) end = markdown.size();
        const std::string_view line = markdown.substr(at, end - at);
        at = end + 1;
        const std::string_view trimmed = TrimYamlSpace(line);
        std::string_view entryKey;
        std::string_view entryValue;
        if (first) {
            if (trimmed != "---") return metadata;
        } else if (trimmed == "---" || trimmed == "...") {
            closed = true;
        } else if (trimmed.empty() || trimmed.front() == '#') {
            // A blank or comment line changes nothing, also within a list.
        } else if (listKey < kKeyCount && (trimmed == "-" || trimmed.compare(0, 2, "- ") == 0)) {
            // An item: a scalar, or a map, such as Pandoc's authors with a name and an
            // affiliation, of which the name counts.
            const std::string_view entry = TrimYamlSpace(trimmed.substr(1));
            std::string item;
            nameWanted = entry.empty();
            if (!YamlMapEntry(entry, entryKey, entryValue)) {
                item = YamlScalar(entry);
            } else if (entryKey == "name") {
                item = YamlScalar(entryValue);
            } else {
                nameWanted = true;
            }
            if (!item.empty()) items[listKey].push_back(std::move(item));
        } else if (listKey < kKeyCount && (line.front() == ' ' || line.front() == '\t')) {
            // A further line of the item above: the name of a map that had none yet.
            if (nameWanted && YamlMapEntry(trimmed, entryKey, entryValue) && entryKey == "name") {
                if (std::string item = YamlScalar(entryValue); !item.empty()) items[listKey].push_back(std::move(item));
                nameWanted = false;
            }
        } else {
            listKey = kKeyCount;
            for (size_t key = 0; key < kKeyCount; key++) {
                const std::string_view name = kKeys[key];
                if (found[key] || line.size() <= name.size() || line.compare(0, name.size(), name) != 0 ||
                    line[name.size()] != ':') {
                    continue;
                }
                const std::string_view rest = line.substr(name.size() + 1);
                if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t' && rest.front() != '\r') continue;
                found[key] = true;
                const std::string_view value = TrimYamlSpace(rest);
                if (value.empty() || value.front() == '#') {
                    listKey = key;
                } else if (value.front() == '[') {
                    YamlFlowItems(value, items[key]);
                } else if (std::string item = YamlScalar(value); !item.empty()) {
                    items[key].push_back(std::move(item));
                }
                break;
            }
        }
    }
    // Without its closing line it is no front matter.
    if (!closed) return metadata;
    const auto join = [](const std::vector<std::string>& parts, std::string_view separator) {
        std::string joined;
        for (const std::string& part : parts) {
            if (!joined.empty()) joined += separator;
            joined += part;
        }
        return joined;
    };
    metadata.title = join(items[0], ", ");
    metadata.author = join(items[1], "; ");
    metadata.subject = join(items[2].empty() ? items[5] : items[2], ", ");
    metadata.keywords = join(items[3], ", ");
    if (items[4].size() == 1 && IsLanguageTag(items[4].front())) metadata.lang = items[4].front();
    metadata.date = join(items[6], ", ");
    return metadata;
}

// The document information dictionary: its producer and what the front matter gives. The title
// is the front matter's, else the text of the first heading; a document with neither has none,
// and viewers show the file name instead. Heading text is WinAnsi with `winAnsi`.
RAYOMD_COLD static std::string InfoDictionary(const char* producer, const DocumentMetadata& metadata,
    const std::vector<HeadingMark>& headings, bool winAnsi) {
    std::string info = "<< /Producer (";
    info += producer;
    info += ") /Creator (RayoMD)";
    const auto field = [&info](std::string_view key, std::string_view value, bool valueWinAnsi) {
        if (value.empty()) return;
        const size_t at = info.size();
        info.resize(at + key.size() + value.size() * 4 + 6);
        memcpy(&info[at], key.data(), key.size());
        const char* const end = WriteTextString(&info[at + key.size()], value, valueWinAnsi);
        info.resize((size_t)(end - info.data()));
    };
    if (!metadata.title.empty()) {
        field(" /Title ", metadata.title, false);
    } else if (!headings.empty()) {
        field(" /Title ", headings.front().text, winAnsi);
    }
    field(" /Author ", metadata.author, false);
    field(" /Subject ", metadata.subject, false);
    field(" /Keywords ", metadata.keywords, false);
    info += " >>";
    return info;
}

// The front matter's language as the catalog's /Lang, which screen readers and text extraction
// use. A language tag needs no escaping.
static void AppendLanguage(std::string& catalog, const DocumentMetadata& metadata) {
    if (metadata.lang.empty()) return;
    catalog += " /Lang (";
    catalog += metadata.lang;
    catalog += ")";
}

// PDF/A-3b: the sRGB output intent and the XMP metadata, which holds the reversible profile's
// properties when `source` is embedded. The Info dictionary has to tell what the packet does:
// `metadata` takes the title, else the first heading's text, and every value as both hold it,
// and the Info dictionary is then made from it without the headings.
RAYOMD_COLD static void AddPdfaCatalogEntries(PdfObjects& pdf, std::string& catalog, DocumentMetadata& metadata,
    const std::vector<HeadingMark>& headings, const char* producer, std::string_view source) {
    metadata.title = Internal::PdfaText(!metadata.title.empty() || headings.empty() ? std::string_view(metadata.title)
        : headings.front().text);
    metadata.author = Internal::PdfaText(metadata.author);
    metadata.subject = Internal::PdfaText(metadata.subject);
    metadata.keywords = Internal::PdfaText(metadata.keywords);
    catalog += " /OutputIntents [<< /Type /OutputIntent /S /GTS_PDFA1 /OutputConditionIdentifier (sRGB IEC61966-2.1)"
        " /RegistryName (http://www.color.org) /Info (sRGB IEC61966-2.1) /DestOutputProfile ";
    AppendInt(catalog, pdf.AddStreamView("/N 3", Internal::SrgbIccProfile()));
    catalog += " 0 R >>] /Metadata ";
    const Internal::PdfaMetadata xmp{ producer, metadata.title, metadata.author, metadata.subject, metadata.keywords,
        source, RAYOMD_VERSION };
    AppendInt(catalog, pdf.AddStream("/Type /Metadata /Subtype /XML", Internal::PdfaXmpMetadata(xmp)));
    catalog += " 0 R";
}

// A link to a heading of this document. AddLinkAnnotationObjects reserves its annotation in
// its page's place; AddInternalLinks writes it once the pages exist, to point straight at the
// heading.
struct InternalLink {
    int id = 0;
    const LinkRect* link = nullptr;
    size_t page = 0;
    double top = 0.0;           // of the view
};

// The lower case JavaScript gives the capitals of Latin-1, Latin Extended-A, Greek, Cyrillic
// and Vietnamese, as GitHub's anchors have them. It has as many UTF-8 bytes as the capital.
RAYOMD_COLD static uint32_t LowerCodePoint(uint32_t cp) {
    if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) ||
        (cp >= 0x410 && cp <= 0x42F)) {
        return cp + 32;
    }
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
    if (cp == 0x178) return 0xFF;
    if (cp == 0x386) return 0x3AC;
    if (cp >= 0x388 && cp <= 0x38A) return cp + 37;
    if (cp == 0x38C) return 0x3CC;
    if (cp == 0x38E || cp == 0x38F) return cp + 63;
    if (cp >= 0x100 && cp <= 0x17E && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149) {
        // Capitals and small letters alternate; from Ĺ to ň and from Ź to ž the capitals are odd.
        const bool oddCapitals = (cp >= 0x139 && cp <= 0x148) || cp >= 0x179;
        return (cp & 1u) == (oddCapitals ? 1u : 0u) ? cp + 1 : cp;
    }
    if ((cp >= 0x1E00 && cp <= 0x1E95) || (cp >= 0x1EA0 && cp <= 0x1EFF)) return cp | 1u;
    return cp;
}

// Whether GitHub drops the character from an anchor: the punctuation and signs of Latin-1 (but
// ª µ º), general punctuation, currency signs, arrows, mathematical and technical signs, box
// drawing, shapes, dingbats, CJK and fullwidth punctuation, and emoji.
RAYOMD_COLD static bool IsAnchorSign(uint32_t cp) {
    static constexpr uint32_t kRanges[][2] = {
        { 0x80, 0xA9 }, { 0xAB, 0xB4 }, { 0xB6, 0xB9 }, { 0xBB, 0xBF }, { 0xD7, 0xD7 }, { 0xF7, 0xF7 },
        { 0x2000, 0x206F }, { 0x20A0, 0x20CF }, { 0x2190, 0x245F }, { 0x2500, 0x2BFF }, { 0x3000, 0x3004 },
        { 0x3008, 0x3020 }, { 0xFF01, 0xFF0F }, { 0xFF1A, 0xFF20 }, { 0xFF3B, 0xFF40 }, { 0xFF5B, 0xFF65 },
        { 0x1F000, 0x1FAFF },
    };
    for (const auto& range : kRanges) {
        if (cp >= range[0] && cp <= range[1]) return true;
    }
    return false;
}

// The anchor GitHub gives a heading, so that "[see](#getting-started)" reaches "## Getting
// Started": lower case, a space as '-', and no punctuation, signs or emoji. `text` is UTF-8;
// bytes that are not are dropped.
RAYOMD_COLD static std::string HeadingSlug(std::string_view text) {
    std::string slug;
    slug.reserve(text.size() + 2);
    for (size_t at = 0; at < text.size();) {
        const unsigned char ch = (unsigned char)text[at];
        if (ch < 0x80) {
            at++;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') slug += (char)ch;
            else if (ch >= 'A' && ch <= 'Z') slug += (char)(ch + 32);
            else if (ch == ' ') slug += '-';
            continue;
        }
        uint32_t cp = 0;
        size_t length = 1;
        if (!RayoMd::Text::DecodeUtf8(text, at, cp, length)) {
            at++;
            continue;
        }
        at += length;
        if (IsAnchorSign(cp)) continue;
        if (cp == 0x130) {
            slug += "i\xCC\x87";        // İ: an i and a combining dot above
            continue;
        }
        cp = LowerCodePoint(cp);
        char bytes[4];
        for (size_t index = length; index-- > 1; cp >>= 6) bytes[index] = (char)(0x80 | (cp & 0x3F));
        bytes[0] = (char)((length == 2 ? 0xC0 : length == 3 ? 0xE0 : 0xF0) | cp);
        slug.append(bytes, length);
    }
    return slug;
}

// `text` with its %XX escapes decoded, as a browser reads the fragment of a link.
RAYOMD_COLD static std::string PercentDecoded(std::string_view text) {
    const auto digit = [](char ch) {
        return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size(); at++) {
        if (text[at] == '%' && at + 2 < text.size() && digit(text[at + 1]) >= 0 && digit(text[at + 2]) >= 0) {
            out += (char)(digit(text[at + 1]) * 16 + digit(text[at + 2]));
            at += 2;
        } else {
            out += text[at];
        }
    }
    return out;
}

// A footnote reference is its number at 0.7 times the size of the text around it, raised by
// 0.36 of that size; the notes after the text are 0.85 times the body size, a note's blocks
// after its first paragraph indented by 16 points.
constexpr double kNoteSizeFactor = 0.7;
constexpr double kNoteRiseFactor = 0.36;
constexpr double kNoteTextFactor = 0.85;
constexpr double kNoteIndent = 16.0;

// Where the footnotes are drawn: note N's text and its first reference, at [N - 1]; a negative
// top where it is not drawn.
struct NoteMarks {
    struct Point {
        size_t page = 0;
        double top = -1.0;
    };
    std::vector<Point> notes;
    std::vector<Point> references;

    void MarkReference(int note, size_t page, double top) {
        if (references.size() < (size_t)note) references.resize((size_t)note);
        Point& point = references[(size_t)note - 1];
        if (point.top < 0.0) point = { page, top };
    }
};

// The number of the footnote a span stands for: an InlineMath::Note span, whose text is the
// label, or in heading and cell text a formula that is kNoteMark and the number. 0 for any
// other span and for a note the document does not have.
RAYOMD_COLD static int FootnoteNumber(const Internal::Footnotes* footnotes, const Internal::InlineSpan& span) {
    if (footnotes == nullptr) return 0;
    if (span.math == Internal::InlineMath::Note) {
        const auto found = footnotes->numbers.find(Internal::NormalizeReferenceLabel(span.text));
        return found == footnotes->numbers.end() ? 0 : found->second;
    }
    if (span.text.size() < 2 || span.text.size() > 10 || span.text[0] != Internal::kNoteMark) return 0;
    size_t number = 0;
    for (size_t at = 1; at < span.text.size(); at++) {
        if (span.text[at] < '0' || span.text[at] > '9') return 0;
        number = number * 10 + (size_t)(span.text[at] - '0');
    }
    return number >= 1 && number <= footnotes->notes.size() ? (int)number : 0;
}

// The short rule above the footnotes, from (x, y) `length` to the right.
RAYOMD_COLD static void AppendFootnoteRule(std::string& c, const char* color, double x, double y, double length) {
    c += "q ";
    c += color;
    c += " RG 0.5 w ";
    AppendF(c, x);
    c += " ";
    AppendF(c, y);
    c += " m ";
    AppendF(c, x + length);
    c += " ";
    AppendF(c, y);
    c += " l S Q\n";
}

// Where a link goes (AddLinkAnnotationObjects): outside the document, as a URI, or to a place in
// it, whose annotation is then reserved, or nowhere, when the place it names does not exist.
enum class LinkTarget : uint8_t { External, Reserved, Dropped };

// The headings that links to "#anchor" name. The anchors are made at the first such link, so a
// document without one (links to footnotes and table entries do not count) does no work for them.
class HeadingTargets {
public:
    HeadingTargets(const std::vector<HeadingMark>& headingMarks, bool winAnsiText, double pageHeight,
        const NoteMarks& noteMarks)
        : headings(headingMarks), winAnsi(winAnsiText), pageTop(pageHeight), notes(noteMarks) {}

    // The heading of each entry of the table of contents, an index into the headings, or npos.
    void SetContents(const std::vector<size_t>* entryHeadings) { contents = entryHeadings; }

    // Reserves the annotation of `link`, to "#fragment", in `out`; false when no heading has that
    // anchor. The fragment may be percent-encoded and differ from the anchor in case and
    // punctuation. An empty fragment and "top" go to the top of the first page, as in a browser,
    // in a document with footnotes "^N" to note N and "^rN" to its first reference, and in one
    // with a table of contents "\x01N" to the heading of its entry N.
    RAYOMD_COLD bool Reserve(PdfObjects& pdf, const LinkRect& link, std::vector<InternalLink>& out) {
        return ReserveFragment(pdf, link, std::string_view(link.url).substr(1), 0, out);
    }

    // A link of a book's file (Target): links to the place `written` names, a fragment as in
    // Reserve, an empty one and "top" to the top of page `topPage`.
    RAYOMD_COLD bool ReserveFragment(PdfObjects& pdf, const LinkRect& link, std::string_view written, size_t topPage,
        std::vector<InternalLink>& out) {
        std::string fragment(written);
        if (winAnsi && !IsAllAscii(fragment)) fragment = RayoMd::Text::WinAnsiToUtf8(fragment);
        fragment = PercentDecoded(fragment);
        if (!notes.notes.empty() && !fragment.empty() && fragment[0] == '^') return ReserveNote(pdf, link, fragment, out);
        if (contents != nullptr && !fragment.empty() && fragment[0] == '\x01') {
            const size_t entry = (size_t)std::strtoull(fragment.c_str() + 1, nullptr, 10);
            if (entry >= contents->size() || (*contents)[entry] >= headings.size()) return false;
            const HeadingMark& heading = headings[(*contents)[entry]];
            out.push_back({ pdf.Reserve(), &link, heading.page, heading.top + 4.0 });
            return true;
        }
        if (!indexed) Index();
        auto found = anchors.find(fragment);
        if (found == anchors.end()) found = anchors.find(HeadingSlug(fragment));
        if (found != anchors.end()) {
            const HeadingMark& heading = headings[found->second.heading];
            out.push_back({ pdf.Reserve(), &link, heading.page, heading.top + 4.0 });
            return true;
        }
        const bool top = fragment.empty() || (fragment.size() == 3 && (fragment[0] | 0x20) == 't' &&
            (fragment[1] | 0x20) == 'o' && (fragment[2] | 0x20) == 'p');
        if (top) out.push_back({ pdf.Reserve(), &link, topPage, pageTop });
        return top;
    }

    // Whether `link` goes outside the document or into it ("#fragment", Reserve).
    LinkTarget Target(PdfObjects& pdf, const LinkRect& link, size_t, std::vector<InternalLink>& out) {
        if (link.url[0] != '#') return LinkTarget::External;
        return Reserve(pdf, link, out) ? LinkTarget::Reserved : LinkTarget::Dropped;
    }

private:
    struct Anchor {
        size_t heading = 0;
        int repeats = 0;        // of the anchor in later headings
    };

    // Every heading's anchor, a repeated one numbered "-1", "-2", ... as on GitHub, skipping a
    // number that is already another heading's anchor.
    RAYOMD_COLD void Index() {
        indexed = true;
        std::string utf8;
        for (size_t index = 0; index < headings.size(); index++) {
            std::string_view text = headings[index].text;
            if (winAnsi && !IsAllAscii(text)) text = utf8 = RayoMd::Text::WinAnsiToUtf8(text);
            std::string anchor = HeadingSlug(text);
            const auto found = anchors.find(anchor);
            if (found != anchors.end()) {
                int& repeats = found->second.repeats;
                const std::string base = std::move(anchor);
                do {
                    anchor = base + '-' + std::to_string(++repeats);
                } while (anchors.count(anchor) != 0);
            }
            anchors.emplace(std::move(anchor), Anchor{ index, 0 });
        }
    }

    // A link to a footnote or back to its first reference; false when it names neither.
    RAYOMD_COLD bool ReserveNote(PdfObjects& pdf, const LinkRect& link, std::string_view fragment,
        std::vector<InternalLink>& out) {
        const bool back = fragment.size() > 1 && fragment[1] == 'r';
        const std::string_view digits = fragment.substr(back ? 2 : 1);
        size_t number = 0;
        for (const char ch : digits) {
            if (ch < '0' || ch > '9' || number > 1000000) return false;
            number = number * 10 + (size_t)(ch - '0');
        }
        const std::vector<NoteMarks::Point>& points = back ? notes.references : notes.notes;
        if (digits.empty() || number == 0 || number > points.size() || points[number - 1].top < 0.0) return false;
        out.push_back({ pdf.Reserve(), &link, points[number - 1].page, points[number - 1].top + 4.0 });
        return true;
    }

    const std::vector<HeadingMark>& headings;
    bool winAnsi = false;
    double pageTop = 0.0;   // the top of a page, where "#top" goes
    bool indexed = false;
    std::unordered_map<std::string, Anchor> anchors;
    const NoteMarks& notes;
    const std::vector<size_t>* contents = nullptr;
};

// "<< /Type /Annot /Subtype /Link /Rect [...] /Border [0 0 0]": the start of a link annotation;
// `print` adds /F 4, the Print flag PDF/A requires of every annotation.
static void AppendLinkAnnotationStart(std::string& annot, const LinkRect& link, bool print) {
    // One growth and pointer writes, not a call per piece where GCC leaves the appends out of line.
    TailWriter w(annot, 72 + kOperandBytes * 4);
    w.Lit("<< /Type /Annot /Subtype /Link");
    if (print) w.Lit(" /F 4");
    w.Lit(" /Rect [");
    w.Fixed(link.x1);
    w.Lit(" ");
    w.Fixed(link.y1);
    w.Lit(" ");
    w.Fixed(link.x2);
    w.Lit(" ");
    w.Fixed(link.y2);
    w.Lit("] /Border [0 0 0]");
}

// The link annotations of every page. Those of links into the document are left to `targets`
// (HeadingTargets, a book's BookTargets), which drops a link to a place the document does not
// have instead of leading nowhere.
template <typename Targets>
static std::vector<std::vector<int>> AddLinkAnnotationObjects(PdfObjects& pdf,
    const std::vector<std::vector<LinkRect>>& linksByPage, bool winAnsiTargets, Targets& targets,
    std::vector<InternalLink>& internalLinks, bool print) {
    std::vector<std::vector<int>> idsByPage;
    idsByPage.reserve(linksByPage.size());
    for (size_t page = 0; page < linksByPage.size(); page++) {
        const std::vector<LinkRect>& pageLinks = linksByPage[page];
        std::vector<int> ids;
        ids.reserve(pageLinks.size());
        for (const LinkRect& link : pageLinks) {
            if (link.url.empty() || link.x2 <= link.x1 || link.y2 <= link.y1) continue;
            const LinkTarget target = targets.Target(pdf, link, page, internalLinks);
            if (target != LinkTarget::External) {
                if (target == LinkTarget::Reserved) ids.push_back(internalLinks.back().id);
                continue;
            }
            std::string annot;
            annot.reserve(link.url.size() + 192);
            AppendLinkAnnotationStart(annot, link, print);
            annot += " /A << /S /URI /URI (";
            AppendUriLiteral(annot, link.url, winAnsiTargets);
            annot += ") >> >>";
            ids.push_back(pdf.Add(std::move(annot)));
        }
        idsByPage.push_back(std::move(ids));
    }
    return idsByPage;
}

// Writes the annotations of the links to headings, now that the pages they go to exist.
RAYOMD_COLD static void AddInternalLinks(PdfObjects& pdf, const std::vector<InternalLink>& links,
    const std::vector<int>& pageIds, bool print) {
    for (const InternalLink& internal : links) {
        std::string annot;
        annot.reserve(176);
        AppendLinkAnnotationStart(annot, *internal.link, print);
        annot += " /Dest [";
        AppendInt(annot, pageIds[std::min(internal.page, pageIds.size() - 1)]);
        annot += " 0 R /XYZ null ";
        AppendF(annot, internal.top);
        annot += " null] >>";
        pdf.Set(internal.id, std::move(annot));
    }
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
#define RAYOMD_MATH_COLD RAYOMD_COLD


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
    // PDF/A: every formula is shown as its source, as the math fonts are not embedded.
    void Disable() { disabled = true; }
    bool Disabled() const { return disabled; }

    int Add(std::string_view tex, double size, bool display, bool bold, double maxWidth, double maxHeight,
        const MathFallbackFont* fallback) {
        Item item;
        item.display = display;
        if (disabled || !LayoutMathToFit(tex, size, display, bold, maxWidth, maxHeight, fallback, item.formula)) return -1;
        items.push_back(std::move(item));
        return (int)items.size() - 1;
    }

    // A footnote reference, note `note` in text of `size`: a box `width` wide that the renderer
    // paints (PaintNote) and no formula.
    int AddNote(int note, double width, double size) {
        Item item;
        item.note = note;
        item.noteWidth = width;
        item.noteSize = size;
        items.push_back(std::move(item));
        return (int)items.size() - 1;
    }
    int Note(int index) const { return items[(size_t)index].note; }
    double NoteSize(int index) const { return items[(size_t)index].noteSize; }

    double Width(int index) const {
        const Item& item = items[(size_t)index];
        return item.note != 0 ? item.noteWidth : item.formula.Width();
    }
    double Ascent(int index) const {
        const Item& item = items[(size_t)index];
        return item.note != 0 ? item.noteSize * (kNoteRiseFactor + kNoteSizeFactor * 0.75) : item.formula.Ascent();
    }
    double Descent(int index) const {
        const Item& item = items[(size_t)index];
        return item.note != 0 ? 0.0 : item.formula.Descent();
    }
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
        int note = 0;               // a footnote reference's number; 0 for a formula
        double noteWidth = 0.0;
        double noteSize = 0.0;      // the size of the text around the reference
    };
    std::vector<Item> items;
    bool used = false;
    bool sourceShown = false;
    bool disabled = false;
};

template <typename SpanType>
static MathLineExtent MeasureMathLine(const MathPool& math, const std::vector<SpanType>& line,
    double size, double lineHeight) {
    MathLineExtent extent;
    for (const SpanType& span : line) {
        if (span.math < 0) continue;
        const double over = math.Ascent(span.math) - size;
        const double under = math.Descent(span.math) - (lineHeight - size);
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

// Source whitespace decides whether two words touch: "($x$)", "$n$-th", "[link](u)." and
// "**a**b" stay tight, "a $x$ b" keeps its spaces. joins[i] != 0 means: no space and no
// line break between word i and word i - 1, because word i starts a span (or is a formula)
// right where the text before it ends inside a word.
template <typename SpanType>
RAYOMD_MATH_COLD static std::vector<unsigned char> MarkWordJoins(const std::vector<SpanType>& spans, size_t wordCount) {
    std::vector<unsigned char> joins(wordCount, 0);
    size_t wordIndex = 0;
    bool gap = true;
    for (const SpanType& span : spans) {
        if (span.math >= 0) {
            if (wordIndex < wordCount) joins[wordIndex] = !gap && wordIndex > 0;
            wordIndex++;
            gap = false;
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
            if (!gap && wordIndex > 0 && wordIndex < wordCount) joins[wordIndex] = 1;
            inWord = true;
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

    double tableWidth = renderer.pageW - renderer.margin * 2.0;
    double colWidth = tableWidth / columns;
    double size = 9.6;
    double lh = size * 1.32;
    double pad = 5.0;
    double cellTextWidth = std::max(16.0, colWidth - pad * 2.0);
    static const std::string emptyCell;

    // Paints lines [from, to) of one cell into the cell box whose top is `top`.
    auto paintLines = [&](const auto& lines, size_t from, size_t to, double cellX, double top, int align, bool header) {
        double lineTop = top - pad;
        for (size_t li = from; li < to; li++) {
            const auto& line = lines[li];
            MathLineExtent extent = MeasureMathLine(renderer.math, line, size, lh);
            double lineWidth = renderer.MathLineWidth(line, size, header);
            double tx = cellX + pad;
            if (align == 0) tx = cellX + (colWidth - lineWidth) * 0.5;
            else if (align == 1) tx = cellX + colWidth - pad - lineWidth;
            renderer.PaintMathTextLine(line, tx, lineTop - extent.above - size, size,
                renderer.TableTextColor(header), header);
            lineTop -= lh + extent.above + extent.below;
        }
    };

    renderer.y -= 3.0;
    // A table that goes on to a new page repeats its header row there, unless it and the row
    // that moved do not fit a page together: the loop draws row 0 again, then that row.
    double headerHeight = 0.0;  // the header row's height; 0 when it was cut into slices
    size_t resumeRow = 0;       // the row to draw after a repeated header row, or 0
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
        if (rowHeight + 5.0 > renderer.pageH - renderer.margin * 2.0) {
            // Taller than a page: every page gets as many lines of each cell as fit, inside
            // cell borders of its own. A fresh page takes at least one line of every cell.
            std::vector<size_t> next(columns, 0);
            std::vector<size_t> end(columns, 0);
            bool freshPage = false;
            for (;;) {
                const double room = renderer.y - renderer.margin - 5.0 - pad * 2.0;
                double sliceHeight = 0.0;
                bool any = false;
                for (size_t c = 0; c < columns; c++) {
                    double height = 0.0;
                    size_t last = next[c];
                    while (last < wrapped[c].size()) {
                        MathLineExtent extent = MeasureMathLine(renderer.math, wrapped[c][last], size, lh);
                        const double lineHeight = lh + extent.above + extent.below;
                        if (height + lineHeight > room && !(freshPage && last == next[c])) break;
                        height += lineHeight;
                        last++;
                    }
                    end[c] = last;
                    sliceHeight = std::max(sliceHeight, height);
                    any = any || last > next[c];
                }
                if (!any) {
                    renderer.NewPage();
                    freshPage = true;
                    continue;
                }
                const double top = renderer.y;
                const double boxHeight = sliceHeight + pad * 2.0;
                if (r == 0) renderer.TableFill(renderer.margin, top, tableWidth, boxHeight);
                bool remaining = false;
                for (size_t c = 0; c < columns; c++) {
                    double cellX = renderer.margin + c * colWidth;
                    renderer.TableStroke(cellX, top, colWidth, boxHeight);
                    int align = c < block.aligns.size() ? block.aligns[c] : -1;
                    paintLines(wrapped[c], next[c], end[c], cellX, top, align, r == 0);
                    next[c] = end[c];
                    remaining = remaining || next[c] < wrapped[c].size();
                }
                renderer.y -= boxHeight;
                if (!remaining) break;
                renderer.NewPage();
                freshPage = true;
            }
            continue;
        }
        if (r == 0) {
            // The header row keeps a line of the next row with it.
            headerHeight = rowHeight;
            renderer.Ensure(rowHeight + 5.0 + (rows.size() > 1 ? lh + pad * 2.0 : 0.0));
        } else if (renderer.y - rowHeight - 5.0 < renderer.margin) {
            renderer.NewPage();
            if (headerHeight > 0.0 && headerHeight + rowHeight + 5.0 <= renderer.pageH - renderer.margin * 2.0) {
                resumeRow = r;
                r = static_cast<size_t>(-1);
                continue;
            }
        }
        double top = renderer.y;
        if (r == 0) renderer.TableFill(renderer.margin, top, tableWidth, rowHeight);
        for (size_t c = 0; c < columns; c++) {
            double cellX = renderer.margin + c * colWidth;
            renderer.TableStroke(cellX, top, colWidth, rowHeight);
            int align = c < block.aligns.size() ? block.aligns[c] : -1;
            paintLines(wrapped[c], 0, wrapped[c].size(), cellX, top, align, r == 0);
        }
        renderer.y -= rowHeight;
        if (resumeRow != 0) {
            r = resumeRow - 1;
            resumeRow = 0;
        }
    }
    renderer.y -= 9.0;
}

// The point sizes of the heading levels 1 to 6.
constexpr double kHeadingSizes[] = { 0, 26, 22, 18, 15, 13, 12 };

// The room a heading keeps under its first line, so that it never ends a page: the space after
// it, the headings right after it (each with its space before and after and its first line)
// and two lines of what follows them, unless nothing or a page break does.
static double HeadingKeep(const Block& heading, const Block* next, const Block* end, double lineHeight) {
    double keep = std::max(1, std::min(6, heading.level)) <= 2 ? 8.0 : 5.0;
    for (; next != end && next->type == BlockType::Heading; next++) {
        const int level = std::max(1, std::min(6, next->level));
        keep += (level <= 2 ? 20.0 : 13.0) + kHeadingSizes[level] * 1.35;
    }
    if (next != end && next->type != BlockType::PageBreak) keep += lineHeight * 2.0;
    return keep;
}

// Where an entry of the table of contents ends: the page, baseline and right end of its last line,
// for the page number and the dots the builder adds to it (ContentsNumbers).
struct ContentsLine {
    size_t entry = 0;
    size_t page = 0;
    double baseline = 0.0;
    double end = 0.0;
};

// A run of a line of highlighted code: where it ends, and the colour index of its characters
// (Internal::kCodeColorOfClass).
struct CodeRun {
    uint32_t end;
    uint8_t color;
};

// Splits a line of highlighted code into runs of one colour, into `runs`, which keeps its room
// from line to line. Spaces take the colour of the run they are in, leading ones that of the first
// run, so they never start one.
template <typename Char>
static void SplitCodeRuns(const Char* text, size_t length, const uint8_t* classes, std::vector<CodeRun>& runs) {
    runs.clear();
    uint8_t color = 0xFF;   // none yet
    for (size_t at = 0; at < length; at++) {
        if (text[at] == ' ') continue;
        const uint8_t next = Internal::kCodeColorOfClass[classes[at]];
        if (next == color) continue;
        if (color != 0xFF) runs.push_back({ (uint32_t)at, color });
        color = next;
    }
    runs.push_back({ (uint32_t)length, color == 0xFF ? (uint8_t)0 : color });
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
        case BlockType::Heading:
            renderer.RenderHeading(block, HeadingKeep(block, &block + 1, blocks.data() + blocks.size(), renderer.lineHeight));
            break;
        case BlockType::Paragraph: renderer.RenderParagraph(block.text); break;
        case BlockType::Bullet: renderer.RenderBullet(block); break;
        case BlockType::Numbered: renderer.RenderNumbered(block); break;
        case BlockType::Quote: renderer.RenderQuote(block); break;
        case BlockType::Code: renderer.RenderCode(block); break;
        case BlockType::MathBlock: renderer.RenderMath(block.text); break;
        case BlockType::Table:
            if (block.hasMath) RenderMathTable(renderer, block);
            else renderer.RenderTable(block.rows, block.aligns);
            break;
        case BlockType::Rule: renderer.RenderRule(); break;
        case BlockType::PageBreak: break;
        case BlockType::Image: renderer.RenderImage(block); break;
        case BlockType::Contents: renderer.RenderContents(); break;
        }
    }
}

// The footnotes after the text, as GitHub shows them: a short rule, then each note at
// kNoteTextFactor of the body size, its number in front and its other blocks indented
// (Footnotes). Notes where each starts.
template <typename RendererType>
RAYOMD_COLD static void RenderFootnoteSection(RendererType& renderer, const Internal::Footnotes& footnotes) {
    renderer.Ensure(renderer.lineHeight * 2.0 + 18.0);
    renderer.y -= 8.0;
    AppendFootnoteRule(renderer.content, renderer.ruleColor, renderer.margin, renderer.y,
        (renderer.pageW - renderer.margin * 2.0) / 3.0);
    renderer.y -= 10.0;
    const double bodySize = renderer.bodySize;
    const double lineHeight = renderer.lineHeight;
    renderer.bodySize = bodySize * kNoteTextFactor;
    renderer.lineHeight = renderer.bodySize * 1.35;
    renderer.noteMarks.notes.assign(footnotes.notes.size(), NoteMarks::Point{});
    for (size_t index = 0; index < footnotes.notes.size(); index++) {
        renderer.Ensure(renderer.lineHeight);
        renderer.noteMarks.notes[index] = { renderer.pageStarts.size() - 1, renderer.y };
        const Block& note = footnotes.notes[index];
        renderer.RenderParagraph(note.text);
        if (!note.children.empty()) {
            const double margin = renderer.margin;
            renderer.margin = margin + kNoteIndent;
            RenderBlocks(renderer, note.children);
            renderer.margin = margin;
        }
    }
    renderer.bodySize = bodySize;
    renderer.lineHeight = lineHeight;
}

// The blocks, then their footnotes, with the inline parser reading references to them.
template <typename RendererType>
RAYOMD_COLD static void RenderWithFootnotes(RendererType& renderer, const std::vector<Block>& blocks,
    const Internal::Footnotes& footnotes) {
    const Internal::FootnoteLabels labels(footnotes.numbers);
    renderer.SetFootnotes(&footnotes);
    renderer.Render(blocks);
    renderer.RenderFootnotes();
}

// A book for the builders (BuildBookPdf): its files' Markdown and the titles as the renderer
// reads them, transcoded to WinAnsi when the standard fonts show the book.
struct BookSource {
    const Book* book = nullptr;
    std::vector<std::string> texts;    // each file's, empty for a part
    std::vector<std::string> titles;   // each file's or part's
    std::string title;                 // the book's
};

// A book's files parsed (ParseBook), and where the renderer put them (RenderBook).
struct BookParse {
    Internal::BookFiles files;
    bool contentsFirst = false;                   // --toc: the table of contents follows the title page
    std::vector<size_t> firstPages;               // the page each file or part begins on
    std::vector<NoteMarks> noteMarks;             // each file's notes and their first references
};

// A book: its title page and with --toc its table of contents, then each part's title page and
// each file from a new page, the bookmarks of its headings as many levels down as its depth.
template <typename RendererType>
RAYOMD_COLD static void RenderBook(RendererType& renderer, ImageRegistry& images, const BookSource& source,
    BookParse& parse, bool titlePage) {
    const std::vector<BookChapter>& chapters = source.book->chapters;
    if (titlePage) renderer.RenderTitlePage(source.title, false);
    if (parse.contentsFirst) {
        renderer.RenderPageBreak();
        renderer.RenderContents();
    }
    std::vector<HeadingMark>& marks = renderer.headings;
    for (size_t index = 0; index < chapters.size(); index++) {
        const BookChapter& chapter = chapters[index];
        renderer.RenderPageBreak();
        parse.firstPages.push_back(renderer.pageStarts.size() - 1);
        if (chapter.part) {
            renderer.RenderTitlePage(source.titles[index], true);
            continue;
        }
        const size_t before = marks.size();
        images.SetChapter(chapter.path);
        const Internal::Footnotes& footnotes = parse.files.footnotes[index];
        if (footnotes.notes.empty()) {
            renderer.Render(parse.files.blocks[index]);
        } else {
            RenderWithFootnotes(renderer, parse.files.blocks[index], footnotes);
            renderer.SetFootnotes(nullptr);
        }
        const int depth = std::max(0, chapter.depth);
        for (size_t at = before; at < marks.size(); at++) marks[at].level = std::min(6, marks[at].level + depth);
        parse.noteMarks[index] = std::exchange(renderer.noteMarks, NoteMarks{});
    }
}

// Where the links of a book go (BuildBookPdf): "#fragment" into the file whose page the link is
// on, as in a document of its own (HeadingTargets); "b.md" and "b.md#fragment" to another file of
// the book, relative to the linking file, a fragment the file does not have to its top; and those
// of the table of contents to their headings.
class BookTargets {
public:
    RAYOMD_COLD BookTargets(const std::vector<HeadingMark>& headings, const Book& book, const BookParse& bookParse,
        bool winAnsiText, double pageHeight, const std::vector<size_t>* contentsHeadings)
        : marks(headings), chapters(book.chapters), parse(bookParse), winAnsi(winAnsiText), pageTop(pageHeight),
          contents(contentsHeadings), files(book.chapters.size()) {
        size_t mark = 0;
        for (size_t index = 0; index < files.size(); index++) {
            File& file = files[index];
            // The marks of the pages from the file's first to the next one's; none of those before.
            const size_t end = index + 1 < files.size() ? parse.firstPages[index + 1] : std::string::npos;
            while (mark < marks.size() && marks[mark].page < parse.firstPages[index]) mark++;
            while (mark < marks.size() && marks[mark].page < end) file.marks.push_back(marks[mark++]);
            file.targets = std::make_unique<HeadingTargets>(file.marks, winAnsi, pageTop, parse.noteMarks[index]);
            if (!chapters[index].part) file.path = PathToUtf8(PathFromUtf8(chapters[index].path).lexically_normal());
        }
    }

    // AddLinkAnnotationObjects: `link` is on page `page`.
    RAYOMD_COLD LinkTarget Target(PdfObjects& pdf, const LinkRect& link, size_t page, std::vector<InternalLink>& out) {
        const std::string_view url = link.url;
        if (url.size() > 1 && url[0] == '#' && url[1] == '\x01') return ReserveContents(pdf, link, out);
        const size_t file = FileOfPage(page);
        if (url[0] == '#') {
            if (file == kNone) return LinkTarget::Dropped;
            return files[file].targets->ReserveFragment(pdf, link, url.substr(1), parse.firstPages[file], out)
                ? LinkTarget::Reserved : LinkTarget::Dropped;
        }
        if (file == kNone) return LinkTarget::External;
        const size_t hash = url.find('#');
        const size_t target = FileOfLink(file, url.substr(0, hash));
        if (target == kNone) return LinkTarget::External;
        const std::string_view fragment = hash == std::string_view::npos ? std::string_view() : url.substr(hash + 1);
        if (!files[target].targets->ReserveFragment(pdf, link, fragment, parse.firstPages[target], out)) {
            out.push_back({ pdf.Reserve(), &link, parse.firstPages[target], pageTop });
        }
        return LinkTarget::Reserved;
    }

private:
    static constexpr size_t kNone = std::string::npos;

    // A file's path, normalized (none for a part), its headings, and the targets of its links
    // (HeadingTargets keeps references to the headings).
    struct File {
        std::string path;
        std::vector<HeadingMark> marks;
        std::unique_ptr<HeadingTargets> targets;
    };

    // "#\x01N", entry N of the table of contents.
    LinkTarget ReserveContents(PdfObjects& pdf, const LinkRect& link, std::vector<InternalLink>& out) const {
        const size_t entry = (size_t)std::strtoull(link.url.c_str() + 2, nullptr, 10);
        if (contents == nullptr || entry >= contents->size() || (*contents)[entry] >= marks.size()) return LinkTarget::Dropped;
        const HeadingMark& heading = marks[(*contents)[entry]];
        out.push_back({ pdf.Reserve(), &link, heading.page, heading.top + 4.0 });
        return LinkTarget::Reserved;
    }

    // The file or part a page belongs to, kNone for the title page and a table of contents first.
    size_t FileOfPage(size_t page) const {
        const auto after = std::upper_bound(parse.firstPages.begin(), parse.firstPages.end(), page);
        return after == parse.firstPages.begin() ? kNone : (size_t)(after - parse.firstPages.begin()) - 1;
    }

    // The file of the book a link of file `from` names, kNone for one outside it: a relative path,
    // percent-encoded as a URL is, or a directory, whose README.md or index.md it names. A book
    // has tens or hundreds of files, so they are looked through in turn.
    RAYOMD_COLD size_t FileOfLink(size_t from, std::string_view written) const {
        if (written.empty() || written[0] == '/' || written[0] == '\\' || HasUriScheme(written)) return kNone;
        std::string path(written);
        if (winAnsi && !IsAllAscii(path)) path = RayoMd::Text::WinAnsiToUtf8(path);
        path = PercentDecoded(path);
        const std::filesystem::path named = (PathFromUtf8(chapters[from].path).parent_path() / PathFromUtf8(path)).lexically_normal();
        for (const char* index : { "", "README.md", "index.md" }) {
            const std::string key = PathToUtf8(*index == '\0' ? named : (named / index).lexically_normal());
            for (size_t file = 0; file < files.size(); file++) {
                if (!files[file].path.empty() && files[file].path == key) return file;
            }
        }
        return kNone;
    }

    const std::vector<HeadingMark>& marks;
    const std::vector<BookChapter>& chapters;
    const BookParse& parse;
    bool winAnsi = false;
    double pageTop = 0.0;
    const std::vector<size_t>* contents = nullptr;   // the heading of each entry of the table of contents
    std::vector<File> files;                         // each file's; sized once, as HeadingTargets refers to it
};

// The bar beside quoted text, by Block::alert: a plain quote's, then those of the GitHub alerts
// in GitHub's colours, whose titles follow. Arrays, not pointers: no relocations.
constexpr char kQuoteBars[][15] = { "0.45 0.62 0.72", "0.04 0.41 0.85", "0.10 0.50 0.22", "0.51 0.31 0.87",
    "0.60 0.40 0.00", "0.82 0.14 0.18" };
constexpr char kAlertTitles[][10] = { "", "Note", "Tip", "Important", "Warning", "Caution" };

// The colours a theme can change, as PDF colour operands: the style's own where the theme sets
// none. Built for one export, which the renderer holding its strings must not outlive.
class ThemePalette {
public:
    explicit ThemePalette(const PdfTheme& theme)
        : heading(Pick(theme.headingColor, "0.02 0.02 0.02", storage[0])),
          link(Pick(theme.linkColor, "0.05 0.30 0.68", storage[1])),
          rule(Pick(theme.accentColor, "0.68 0.68 0.68", storage[2])),
          quoteBar(Pick(theme.accentColor, kQuoteBars[0], storage[3])) {}
    ThemePalette(const ThemePalette&) = delete;
    ThemePalette& operator=(const ThemePalette&) = delete;

private:
    char storage[4][24] = {};   // first: the pointers below are made from it

    static const char* Pick(int32_t rgb, const char* styleColor, char* buffer) {
        if (rgb < 0) return styleColor;
        char* out = buffer;
        for (int shift = 16; shift >= 0; shift -= 8) {
            if (out != buffer) *out++ = ' ';
            out = RayoMd::Text::WriteFixed2(out, ((rgb >> shift) & 0xFF) / 255.0);
        }
        *out = '\0';
        return buffer;
    }

public:
    const char* const heading;
    const char* const link;
    const char* const rule;       // rules, in the accent colour
    const char* const quoteBar;   // the bar beside a plain quote, in the accent colour
};

class Renderer {
public:
    // The content streams of all pages are appended to `output`, one after another.
    Renderer(std::string& output, const TtfFont& f, int fontObject, PdfStyle styleValue, const PdfMargin& marginValue,
        const PdfPageSize& pageSize, ImageRegistry* imageRegistry, const ThemePalette& palette)
        : font(f), fontId(fontObject), images(imageRegistry), style(styleValue), content(output),
          headingColor(palette.heading), linkColor(palette.link), ruleColor(palette.rule), plainQuoteBar(palette.quoteBar),
          quoteBar(palette.quoteBar) {
        const PdfPageSize page = ResolvePageSize(pageSize);
        pageW = page.width;
        pageH = page.height;
        margin = ResolveMarginPoints(marginValue, page);
        bodySize = style == PdfStyle::Tech ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        mathFallback = { this, &Renderer::MeasureMathFallback, &Renderer::EmitMathFallback,
            kMathFallbackAscent / kMathSizeFactor, kMathFallbackDescent / kMathSizeFactor };
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) { RenderBlocks(*this, blocks); }
    // The footnotes the text refers to (ParseMarkdown), which RenderFootnotes draws after it.
    void SetFootnotes(const Internal::Footnotes* notes) { footnotes = notes; }
    void RenderFootnotes() {
        if (footnotes != nullptr && !footnotes->notes.empty()) RenderFootnoteSection(*this, *footnotes);
    }
    // The entries of the table of contents (contents.h), which RenderContents draws.
    void SetContents(const std::vector<Internal::ContentsEntry>* entries) { contents = entries; }

    // The page numbers of the table's entries, right-aligned on the last line of each and after a
    // row of dots below the top level: one stream for each page, for the builder to add to the
    // table's. `pages[i]` is the page of entry i's heading, npos for one not drawn. The digits and
    // the dot join the font subset.
    RAYOMD_COLD void ContentsNumbers(const std::vector<size_t>& pages, std::vector<std::string>& overlays) {
        overlays.resize(pageStarts.size());
        const double em = bodySize;
        const double dotWidth = TextWidth(font, L".", bodySize);
        const double pitch = em * 0.5 + dotWidth;
        for (const ContentsLine& line : contentsLines) {
            if (pages[line.entry] == std::string::npos) continue;
            char digits[24];
            char* const digitsEnd = std::to_chars(digits, digits + sizeof(digits), pages[line.entry] + 1).ptr;
            const std::wstring number(digits + 0, digitsEnd);
            const double numberLeft = pageW - margin - TextWidth(font, number, bodySize);
            const bool top = (*contents)[line.entry].level == 1;
            std::string& out = overlays[line.page];
            // The dots stand on a grid from the left margin, so that those of all entries line up.
            const double first = margin + std::ceil((line.end + em * 0.3 - margin) / pitch) * pitch;
            const double limit = numberLeft - em * 0.3 - dotWidth;
            if (!top && first <= limit) {
                const std::wstring dots((size_t)((limit - first) / pitch) + 1, L'.');
                AppendOverlayText(out, first, line.baseline, dots, "0.45 0.45 0.45", false, pitch - dotWidth);
            }
            AppendOverlayText(out, numberLeft, line.baseline, number, "0.08 0.08 0.08", top, 0.0);
        }
    }

    // Offset in the output buffer at which the content stream of each page starts; with
    // --compress, CompressPageContents moves the streams and these with them.
    std::vector<size_t>& PageStarts() { return pageStarts; }
    const std::vector<std::vector<LinkRect>>& PageLinks() const { return pageLinks; }
    const CidList& UsedCids() const { return usedCids.Values(); }
    // For text drawn outside the pages' streams, such as a theme's header, whose glyphs the font
    // subset needs too.
    UsedCidSet& Cids() { return usedCids; }
    uint32_t MissingCharacters() const { return usedCids.missing; }
    bool MathUsed() const { return math.Used(); }
    // PDF/A: formulas show their TeX source, as the math fonts are not embedded.
    void DisableMath() { math.Disable(); }
    // PdfOptions::highlightCode off: every code block in one colour.
    void DisableHighlighting() { highlightCode = false; }
    const std::vector<HeadingMark>& Headings() const { return headings; }
    const NoteMarks& Notes() const { return noteMarks; }
    double Margin() const { return margin; }
    double PageWidth() const { return pageW; }
    double PageHeight() const { return pageH; }

private:
    template <typename RendererType>
    friend void RenderBlocks(RendererType&, const std::vector<Block>&);
    template <typename RendererType>
    friend void RenderMathTable(RendererType&, const Block&);
    template <typename RendererType>
    friend void RenderFootnoteSection(RendererType&, const Internal::Footnotes&);
    template <typename RendererType>
    friend void RenderBook(RendererType&, ImageRegistry&, const BookSource&, BookParse&, bool);

    const TtfFont& font;
    int fontId = 0;
    ImageRegistry* images = nullptr;
    PdfStyle style = PdfStyle::Elegant;
    double margin = 62.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    double pageW = 595.0;
    double pageH = 842.0;
    // The page being rendered is the tail of `content`, so painting is a plain append.
    std::string& content;
    std::vector<size_t> pageStarts;
    std::vector<std::vector<LinkRect>> pageLinks;
    std::vector<HeadingMark> headings;
    // The theme's colours, and that of the bar beside quoted text: a plain quote's, or that of
    // the alert it is in.
    const char* const headingColor;
    const char* const linkColor;
    const char* const ruleColor;
    const char* const plainQuoteBar;
    const char* quoteBar;

    // Notes where a heading's first line is drawn, for its outline entry and the links to it.
    // A line `firstLine` high that does not fit moves to the next page first, as drawing it
    // would; 0 notes the current place.
    void MarkHeading(int level, const std::string& text, double firstLine) {
        if (text.empty()) return;
        if (firstLine > 0.0) Ensure(firstLine);
        headings.push_back({ level, text, pageStarts.size() - 1, y });
    }
    UsedCidSet usedCids;
    MathPool math;
    const Internal::Footnotes* footnotes = nullptr;
    NoteMarks noteMarks;
    MathFallbackFont mathFallback{};
    // Highlighted code: whether to highlight it, a block's text as the lexer reads it, the
    // classes of that text, and the runs of one colour of a line, kept from block to block.
    bool highlightCode = true;
    std::string codeText;
    std::vector<uint8_t> codeClasses;
    std::vector<CodeRun> codeRuns;
    // The table of contents' entries, and where each ended (RenderContents).
    const std::vector<Internal::ContentsEntry>* contents = nullptr;
    std::vector<ContentsLine> contentsLines;

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
        return (pageH - margin * 2.0) * 0.5;
    }

    void RenderBullet(const Block& block) {
        if (block.task != 0) {
            RenderTaskItem(block);
            return;
        }
        RenderListItem("- ", block);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    // A task list item: its checkbox where a bullet or number would go, its text after it.
    void RenderTaskItem(const Block& block) {
        const double x = margin + 16.0 + block.level * 18.0;
        const double side = bodySize * 0.7;
        Ensure(bodySize * 1.35);
        AppendCheckbox(content, x, y - bodySize - 0.4, side, block.task == 2);
        RenderParagraph(block.text, x + side + 5.0, pageW - margin * 2.0 - 16.0 - block.level * 18.0 - side - 5.0);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderNumbered(const Block& block) {
        if (block.task != 0) {
            RenderTaskItem(block);
            return;
        }
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
        const int note = FootnoteNumber(footnotes, span);
        if (note != 0 || span.math == Internal::InlineMath::Note) {
            PushNoteSpan(spans, note, span.text, size);
            return;
        }
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

    // A footnote reference as one box in the pool; one to a note the document lost, as written.
    RAYOMD_MATH_COLD void PushNoteSpan(std::vector<StyledSpan>& spans, int note, const std::string& label, double size) {
        if (note == 0) {
            MathAppend(spans).text = Utf8ToWide("[^" + label + "]");
            return;
        }
        const std::string number = std::to_string(note);
        const double width = TextWidth(font, std::wstring(number.begin(), number.end()), size * kNoteSizeFactor);
        MathAppend(spans).math = math.AddNote(note, width, size);
    }

    // A footnote reference: its number raised, in the link colour, linking to the note ("#^N").
    // The first reference of a note is where the note's number links back to ("#^rN").
    RAYOMD_MATH_COLD double PaintNote(int index, double x, double baseline) {
        const int note = math.Note(index);
        const double size = math.NoteSize(index);
        const double raised = baseline + size * kNoteRiseFactor;
        const double width = math.Width(index);
        const std::string number = std::to_string(note);
        PaintText(x, raised, size * kNoteSizeFactor, std::wstring(number.begin(), number.end()), linkColor);
        AddLink(x, raised, width, size * kNoteSizeFactor, "#^" + number);
        noteMarks.MarkReference(note, pageStarts.size() - 1, baseline + size);
        return width;
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

    // Line filling for text that contains at least one formula. Same greedy rule as
    // WrapStyledRuns: words that touch in the source (joins) stay together without a space,
    // a formula is one unbreakable word, and a $$...$$ formula gets a line of its own.
    RAYOMD_MATH_COLD std::vector<std::vector<StyledSpan>> WrapMathWords(const std::vector<StyledWord>& words,
        const std::vector<unsigned char>& joins, double width, double size) {
        std::vector<std::vector<StyledSpan>> lines;
        std::vector<StyledSpan> line;
        double lineWidth = 0.0;
        double spaceWidth = TextWidth(font, L" ", size);
        std::vector<double> widths(words.size());
        for (size_t index = 0; index < words.size(); index++) {
            const StyledWord& word = words[index];
            widths[index] = word.math >= 0 ? math.Width(word.math) : TextWidth(font, word.text, size);
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

        bool groupOverflows = false;
        for (size_t index = 0; index < words.size(); index++) {
            const StyledWord& word = words[index];
            if (word.math >= 0 && math.IsDisplay(word.math)) {
                flush();
                MathAppend(MathAppend(lines)).math = word.math;
                continue;
            }
            const bool joined = joins[index] != 0;
            bool needsSpace = !line.empty() && !joined;
            if (!joined) {
                double groupWidth = widths[index];
                for (size_t next = index + 1; next < words.size() && joins[next] != 0 &&
                    !(words[next].math >= 0 && math.IsDisplay(words[next].math)); next++) {
                    groupWidth += widths[next];
                }
                if (!line.empty() && lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width) {
                    flush();
                    needsSpace = false;
                }
                groupOverflows = lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width;
            } else if (groupOverflows && !line.empty() && lineWidth + widths[index] > width) {
                // Words that touch but are wider than a line together break where they touch.
                flush();
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
        return WrapMathWords(words, MarkWordJoins(spans, words.size()), width, size);
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
            width += span.math >= 0 ? math.Width(span.math) : TextWidth(font, span.text, size);
        }
        return width;
    }

    // Paints a line of plain text and formulas in one colour (headings, table cells).
    RAYOMD_MATH_COLD void PaintMathTextLine(const std::vector<StyledSpan>& line, double x, double baseline, double size,
        const char* color, bool bold) {
        double cursor = x;
        for (const StyledSpan& span : line) {
            if (span.math >= 0) {
                cursor += math.Note(span.math) != 0 ? PaintNote(span.math, cursor, baseline)
                    : math.Emit(span.math, content, cursor, baseline, color);
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
        std::wstring wide;                  // the parsed text, or the whole plain paragraph, as wide text
        std::vector<uint32_t> wideEnds;     // where each parsed run ends in `wide`
        std::wstring text;
        std::vector<StyledRun> runs;
        std::vector<uint32_t> lineEnds;     // one past the last run of each line

        std::wstring_view Text(const StyledRun& run) const {
            return std::wstring_view(text.data() + run.begin, run.end - run.begin);
        }
    };
    StyledRuns paragraphRuns;
    std::vector<StyledRuns> tableCells;     // the wrapped cells of a table row, by column

    // Colors of text runs: plain text, code and the background of code.
    struct RunColors { const char* text; const char* code; const char* codeFill; };
    static constexpr RunColors kBodyRunColors{ "0.08 0.08 0.08", "0.18 0.18 0.17", "0.94 0.94 0.92" };
    static constexpr RunColors kQuoteRunColors{ "0.18 0.22 0.25", "0.16 0.16 0.15", "0.88 0.89 0.88" };
    static constexpr RunColors kHeaderCellRunColors{ "0.04 0.04 0.04", "0.18 0.18 0.17", "0.94 0.94 0.92" };
    static constexpr RunColors kCellRunColors{ "0.10 0.10 0.10", "0.18 0.18 0.17", "0.94 0.94 0.92" };

    // The background of code `text`, which starts at x and is `width` wide, from its first
    // glyph on: the space shown before a code word stays clear, and so does the word before.
    void CodeBackground(double x, double top, double width, double lh, std::wstring_view text, double size,
        const char* fill) {
        size_t spaces = 0;
        while (spaces < text.size() && text[spaces] == L' ') spaces++;
        const double skip = spaces != 0 ? TextWidth(font, text.substr(0, spaces), size) : 0.0;
        DrawRect(x + skip - 1.5, top + 1.0, width - skip + 3.0, lh, fill);
    }

    // Paints runs [begin, end) of `runs` as one line from x: code backgrounds `lh` high from
    // `top`, the text on `baseline` (all of it bold when `bold`) and link rectangles. The last
    // run is measured only when its width is drawn.
    RAYOMD_SHARED void PaintStyledRuns(const StyledRuns& runs, size_t begin, size_t end, double x, double top,
        double baseline, double size, double lh, const RunColors& colors, bool bold = false) {
        for (size_t index = begin; index < end; index++) {
            const StyledRun& run = runs.runs[index];
            const std::wstring_view runText = runs.Text(run);
            const bool measured = index + 1 < end || run.code || !run.url.empty();
            const double spanWidth = measured ? TextWidth(font, runText, size) : 0.0;
            if (run.code) CodeBackground(x, top, spanWidth, lh, runText, size, colors.codeFill);
            const char* color = run.url.empty() ? (run.code ? colors.code : colors.text) : linkColor;
            PaintText(x, baseline, size, runText, color, run.bold || bold, run.italic, run.strike);
            AddLink(x, baseline, spanWidth, size, run.url);
            x += spanWidth;
        }
    }

    // Appends `text` to the line whose first run is `lineStart`, after a space when `spaced`.
    // The text joins the last run of the line when the style is the same.
    static RAYOMD_HOT_INLINE void AppendStyledRunText(StyledRuns& out, size_t lineStart, std::wstring_view text,
        bool spaced, bool bold, bool italic, bool strike, std::string_view url, bool code) {
        StyledRun* run = out.runs.size() != lineStart ? &out.runs.back() : nullptr;
        if (!run || run->bold != bold || run->italic != italic || run->strike != strike || run->code != code ||
            !SameText(run->url, url)) {
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
        out.text.append(text.data(), text.size());
        run->end = static_cast<uint32_t>(out.text.size());
    }

    // Appends wide text [begin, end), which lies inside parsed run `index`, in that run's style.
    static RAYOMD_HOT_INLINE void AppendStyledPiece(StyledRuns& out, size_t lineStart, size_t index, size_t begin,
        size_t end, bool spaced) {
        const Internal::InlineRun& span = out.parsed.runs[index];
        AppendStyledRunText(out, lineStart, std::wstring_view(out.wide.data() + begin, end - begin), spaced, span.bold,
            span.italic, span.strike, out.parsed.Url(span), span.code);
    }

    // A word wider than a whole line, cut by character where the line is full; the last part
    // stays open for the words after it. The line is empty when this is called.
    RAYOMD_COLD void PlaceWideStyledWord(StyledRuns& out, size_t& lineStart, double& lineWidth, size_t firstRun,
        size_t wordStart, size_t wordEnd, double width, double size) const {
        const std::wstring_view source = out.wide;
        const double scale = size * 0.001;
        size_t piece = firstRun;
        size_t partStart = wordStart;
        double partWidth = 0.0;
        for (size_t ch = wordStart; ch < wordEnd; ch++) {
            if (ch >= out.wideEnds[piece]) {
                if (ch > partStart) AppendStyledPiece(out, lineStart, piece, partStart, ch, false);
                while (ch >= out.wideEnds[piece]) piece++;
                lineWidth += partWidth;
                partStart = ch;
                partWidth = 0.0;
            }
            const double chWidth = CodepointWidth(font, (uint16_t)source[ch], scale);
            if (lineWidth + partWidth + chWidth > width && (ch > partStart || out.runs.size() != lineStart)) {
                if (ch > partStart) AppendStyledPiece(out, lineStart, piece, partStart, ch, false);
                out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                lineStart = out.runs.size();
                lineWidth = 0.0;
                partStart = ch;
                partWidth = 0.0;
            }
            partWidth += chWidth;
        }
        if (wordEnd > partStart) AppendStyledPiece(out, lineStart, piece, partStart, wordEnd, false);
        lineWidth += partWidth;
    }

    // Wraps `text` into `out` with the line breaks and span boundaries of WrapStyled. A word is
    // everything between two white spaces of the source, so it may cross style and link
    // boundaries ("**a**b", "[link](u)."): it gets no space and no line break inside, and is cut
    // by character only when it is wider than a whole line. Returns false for text with
    // explicit line breaks or possible formulas, which take the general path.
    bool WrapStyledRuns(const std::string& text, double width, double size, StyledRuns& out) {
        if (text.find('\n') != std::string::npos || text.find('$') != std::string::npos ||
            text.find("\\(") != std::string::npos || (footnotes != nullptr && text.find("[^") != std::string::npos)) {
            return false;
        }
        return WrapStyledInline(text, width, size, out);
    }

    // WrapStyledRuns for one line of inline Markdown without formulas, such as a table cell.
    // Text too long for the run offsets gives one empty line and false.
    RAYOMD_SHARED bool WrapStyledInline(std::string_view text, double width, double size, StyledRuns& out) {
        out.text.clear();
        out.runs.clear();
        out.lineEnds.clear();
        if (text.size() >= 0x40000000u) {
            out.lineEnds.push_back(0);
            return false;
        }
        if (!Internal::NeedsInlineParse(text)) {
            // No inline syntax: every line is one unstyled run, also when it is empty.
            out.wide.clear();
            if (text.find('\xE2') == std::string_view::npos) AppendUtf8ToWide(out.wide, text);
            else AppendUtf8ToWide(out.wide, NormalizeSymbols(std::string(text)));
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
        const std::vector<Internal::InlineRun>& parsed = out.parsed.runs;
        out.wide.clear();
        out.wideEnds.clear();
        for (const Internal::InlineRun& span : parsed) {
            AppendUtf8ToWide(out.wide, out.parsed.Text(span));
            out.wideEnds.push_back(static_cast<uint32_t>(out.wide.size()));
        }
        const std::wstring_view source = out.wide;
        size_t lineStart = 0;       // index of the first run of the line being filled
        double lineWidth = 0.0;
        const double spaceWidth = TextWidth(font, L" ", size);

        const size_t runCount = parsed.size();
        const size_t total = source.size();
        size_t at = 0;
        for (size_t index = 0; index < runCount; index++) {
            const Internal::InlineRun& span = parsed[index];
            const size_t runEnd = out.wideEnds[index];
            if (span.lineBreak) {
                // <br>: the line ends here, and one more after it leaves an empty line.
                if (out.runs.size() != lineStart || !out.lineEnds.empty()) {
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineWidth = 0.0;
                }
                at = runEnd;
                continue;
            }
            const std::string_view url = out.parsed.Url(span);
            // Set while the last run of the line holds the previous word of this parsed run, so
            // it has this style and the next word goes into it without comparing styles.
            bool joinLast = false;
            while (at < runEnd) {
                if (IsWideSpace(source[at])) {
                    at++;
                    continue;
                }
                const size_t wordStart = at;
                while (at < runEnd && !IsWideSpace(source[at])) at++;
                // A word that reaches the end of its run goes on in the runs after it unless
                // white space follows: "**a**b" and "[link](u)." are one word each.
                size_t lastRun = index;
                if (at == runEnd && at < total && !IsWideSpace(source[at])) {
                    do {
                        lastRun++;
                        while (at < out.wideEnds[lastRun] && !IsWideSpace(source[at])) at++;
                    } while (at == out.wideEnds[lastRun] && at < total && !IsWideSpace(source[at]));
                }
                const size_t wordEnd = at;
                const std::wstring_view word(source.data() + wordStart, wordEnd - wordStart);

                double wordWidth = TextWidth(font, word, size);
                const bool lineEmpty = out.runs.size() == lineStart;
                bool needsSpace = !lineEmpty;
                double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

                if (!lineEmpty && lineWidth + addWidth > width) {
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineWidth = 0.0;
                    needsSpace = false;
                    joinLast = false;
                }
                if (wordWidth > width) {
                    PlaceWideStyledWord(out, lineStart, lineWidth, index, wordStart, wordEnd, width, size);
                    joinLast = false;
                } else {
                    bool spaced = false;
                    if (needsSpace) {
                        if (span.strike) {
                            AppendStyledRunText(out, lineStart, std::wstring_view(L" ", 1), false, false, false, false,
                                std::string_view(), false);
                            lineWidth += spaceWidth;
                        } else {
                            spaced = true;
                            wordWidth += spaceWidth;
                        }
                    }
                    lineWidth += wordWidth;
                    if (lastRun != index) {
                        // In one piece per parsed run the word touches.
                        for (size_t piece = index, begin = wordStart; begin < wordEnd; piece++) {
                            const size_t end = std::min<size_t>(wordEnd, out.wideEnds[piece]);
                            if (end == begin) continue;
                            AppendStyledPiece(out, lineStart, piece, begin, end, spaced);
                            spaced = false;
                            begin = end;
                        }
                    } else if (joinLast) {
                        if (spaced) out.text.push_back(L' ');
                        out.text.append(word.data(), word.size());
                        out.runs.back().end = static_cast<uint32_t>(out.text.size());
                    } else {
                        AppendStyledRunText(out, lineStart, word, spaced, span.bold, span.italic, span.strike, url,
                            span.code);
                        // A struck word gets an unstyled space run before it, so it never joins.
                        joinLast = !span.strike;
                    }
                }
                if (lastRun != index) {
                    index = lastRun - 1;    // go on in the run that holds the end of the word
                    break;
                }
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
        if (!Internal::NeedsInlineParse(text)) {
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
            return WrapMathWords(mathWords, MarkWordJoins(spans, mathWords.size()), width, size);
        }
        std::vector<std::vector<StyledSpan>> lines;
        lines.reserve(std::max<size_t>(1, text.size() / 72));
        std::vector<StyledSpan> line;
        line.reserve(16);
        double lineWidth = 0.0;
        double spaceWidth = TextWidth(font, L" ", size);
        const std::string noUrl;
        auto newLine = [&]() {
            lines.push_back(std::move(line));
            line.clear();
            line.reserve(16);
            lineWidth = 0.0;
        };
        // Width of what continues the word that ends span `index` without a space: the first
        // word of each following span, for as long as the span before it ends inside a word.
        auto gluedWidth = [&](size_t index) {
            double glued = 0.0;
            for (size_t next = index + 1; next < spans.size(); next++) {
                const std::wstring& following = spans[next].text;
                size_t end = 0;
                while (end < following.size() && !IsWideSpace(following[end])) end++;
                glued += TextWidth(font, std::wstring_view(following.data(), end), size);
                if (end < following.size()) break;
            }
            return glued;
        };

        // The words are views into the spans: nothing is copied until a word lands in a line.
        // A word that starts a span where the span before ends inside a word continues that
        // word ("**a**b", "[link](u)."): no space and no line break between them.
        bool endsInWord = false;
        bool groupOverflows = false;
        for (size_t index = 0; index < spans.size(); index++) {
            const StyledSpan& span = spans[index];
            const std::wstring& source = span.text;
            for (size_t at = 0; at < source.size();) {
                const bool glued = at == 0 && endsInWord && !IsWideSpace(source[0]);
                while (at < source.size() && IsWideSpace(source[at])) at++;
                size_t end = at;
                while (end < source.size() && !IsWideSpace(source[end])) end++;
                if (end == at) break;
                std::wstring_view word(source.data() + at, end - at);
                at = end;

                double wordWidth = TextWidth(font, word, size);
                bool needsSpace = false;
                if (glued) {
                    // Only words glued into something wider than a line break where they touch.
                    if (groupOverflows && !line.empty() && lineWidth + wordWidth > width) newLine();
                } else {
                    const double groupWidth = end == source.size() ? wordWidth + gluedWidth(index) : wordWidth;
                    needsSpace = !line.empty();
                    double addWidth = groupWidth + (needsSpace ? spaceWidth : 0.0);
                    if (!line.empty() && lineWidth + addWidth > width) {
                        newLine();
                        needsSpace = false;
                    }
                    groupOverflows = lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width;
                }

                if (wordWidth > width) {
                    // Wider than a whole line: cut by character; the last part stays open.
                    const double scale = size * 0.001;
                    size_t partStart = 0;
                    double partWidth = 0.0;
                    for (size_t ch = 0; ch < word.size(); ch++) {
                        const double chWidth = CodepointWidth(font, (uint16_t)word[ch], scale);
                        if (lineWidth + partWidth + chWidth > width && (ch > partStart || !line.empty())) {
                            if (ch > partStart) {
                                AppendStyledWord(line, word.substr(partStart, ch - partStart), false, span.bold,
                                    span.italic, span.strike, span.url, span.code);
                            }
                            newLine();
                            partStart = ch;
                            partWidth = 0.0;
                        }
                        partWidth += chWidth;
                    }
                    AppendStyledWord(line, word.substr(partStart), false, span.bold, span.italic, span.strike,
                        span.url, span.code);
                    lineWidth += partWidth;
                    continue;
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
            if (!source.empty()) endsInWord = !IsWideSpace(source.back());
        }

        if (!line.empty()) lines.push_back(std::move(line));
        if (lines.empty()) lines.push_back({});
        return lines;
    }

    void NewPage() {
        pageStarts.push_back(content.size());
        pageLinks.push_back({});
        y = pageH - margin;
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

    // The strip behind one line of quoted content, `height` tall: the tint, and the bar at its
    // left in the colour of the quote.
    void QuoteStrip(double height) {
        DrawRect(margin, y + 2.0, pageW - margin * 2.0, height, "0.94 0.95 0.96");
        DrawRect(margin, y + 2.0, 3.0, height, quoteBar);
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
        AddLinkRect(pageLinks.back(), x, baseline, width, size, url);
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

    // An image that cannot be shown: its alt text, or else its source, as a paragraph. The
    // text of a linked image is a link.
    void RenderImageFallback(const Block& block) {
        const Internal::ImageSource& image = *block.image;
        std::string fallback = block.text.empty() ? image.src : block.text;
        if (fallback.empty()) fallback = "image";
        if (image.link.empty()) {
            RenderParagraph(fallback);
            return;
        }
        const double lh = bodySize * 1.35;
        for (const std::wstring& line : WrapText(font, Utf8ToWide(fallback), pageW - margin * 2.0, bodySize)) {
            Ensure(lh);
            PaintText(margin, y - bodySize, bodySize, line, linkColor);
            AddLink(margin, y - bodySize, TextWidth(font, line, bodySize), bodySize, image.link);
            y -= lh;
        }
        y -= 5.0;
    }

    void RenderImage(const Block& block) {
        if (!images) {
            RenderImageFallback(block);
            return;
        }

        int index = -1;
        if (!images->Resolve(block.image->src, block.text, index)) {
            RenderImageFallback(block);
            return;
        }

        const PdfImage& image = images->Get(index);
        double maxW = pageW - margin * 2.0;
        double maxH = pageH - margin * 2.0;
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
        const std::string& link = block.image->link;
        if (!link.empty() && !pageLinks.empty()) pageLinks.back().push_back({ x, y - h, x + w, y, link });
        y -= h + 10.0;
    }

    // `keep` is the room under its first line that the heading needs on its page (HeadingKeep).
    void RenderHeading(const Block& block, double keep) {
        int level = std::max(1, std::min(6, block.level));
        double size = kHeadingSizes[level];
        if (y < pageH - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;
        Ensure(size * 1.35 + keep);

        if (block.hasMath) {
            RenderMathTextLines(block.text, level, margin, pageW - margin * 2.0, size, headingColor, false, false);
            y -= level <= 2 ? 8.0 : 5.0;
            return;
        }
        MarkHeading(level, block.text, size * 1.35);
        std::wstring text = Utf8ToWide(block.text);
        for (const auto& line : WrapText(font, text, pageW - margin * 2.0, size)) {
            DrawTextLine(margin, size, line, headingColor);
        }
        y -= level <= 2 ? 8.0 : 5.0;
    }

    // Heading lines that contain formulas: plain text and formulas in one colour,
    // each line as tall as its tallest formula needs. `quote` adds the quote strip. The
    // heading, at `level`, is noted once its first line has found its page.
    RAYOMD_MATH_COLD void RenderMathTextLines(const std::string& text, int level, double x, double width, double size,
        const char* color, bool bold, bool quote) {
        math.Clear();
        double lh = size * 1.35;
        bool marked = false;
        for (const auto& line : WrapMathText(text, width, size, bold)) {
            MathLineExtent extent = MeasureMathLine(math, line, size, lh);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lh + 2.0 + extra);
                QuoteStrip(lh + 3.0 + extra);
            } else {
                Ensure(lh + extra);
            }
            if (!marked) {
                MarkHeading(level, text, 0.0);
                marked = true;
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
                QuoteStrip(lineHeight + 3.0 + extra);
            } else {
                Ensure(lineHeight + extra);
            }
            y -= extent.above;
            double cursor = x;
            if (IsDisplayMathLine(math, line)) {
                cursor = x + std::max(0.0, (width - math.Width(line[0].math)) * 0.5);
            }
            double baseline = y - bodySize;
            for (const StyledSpan& span : line) {
                if (span.math >= 0) {
                    cursor += math.Note(span.math) != 0 ? PaintNote(span.math, cursor, baseline)
                        : math.Emit(span.math, content, cursor, baseline, textColor);
                    continue;
                }
                double spanWidth = TextWidth(font, span.text, bodySize);
                if (span.code) CodeBackground(cursor, y, spanWidth, lineHeight, span.text, bodySize, codeFill);
                const char* color = span.url.empty() ? (span.code ? codeColor : textColor) : linkColor;
                PaintText(cursor, baseline, bodySize, span.text, color, span.bold, span.italic, span.strike);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight + extent.below;
        }
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, pageW - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        double lh = bodySize * 1.35;
        if (math.Active()) math.Clear();
        if (WrapStyledRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lh);
                PaintStyledRuns(paragraphRuns, index, lineEnd, x, y, y - bodySize, bodySize, lineHeight, kBodyRunColors);
                index = lineEnd;
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
                if (span.code) CodeBackground(cursor, y, spanWidth, lineHeight, span.text, bodySize, "0.94 0.94 0.92");
                const char* color = span.url.empty() ? (span.code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : linkColor;
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
        double width = pageW - margin * 2.0 - 16.0 - block.level * 18.0;
        RenderParagraph(marker + block.text, x, width);
    }

    void RenderQuote(const Block& block) {
        if (block.alert != 0) {
            RenderAlert(block);
            return;
        }
        if (block.children.empty()) {
            RenderQuote(block.text);
            return;
        }
        RenderQuoteChildren(block.children);
    }

    // A GitHub alert: a quote whose bar takes the alert's colour, under the alert's bold title
    // in that colour, which keeps the first line after it on its page.
    RAYOMD_COLD void RenderAlert(const Block& block) {
        quoteBar = kQuoteBars[block.alert];
        Ensure(lineHeight * 2.0 + 2.0);
        QuoteStrip(lineHeight + 3.0);
        const std::string_view title = kAlertTitles[block.alert];
        PaintText(margin + 14.0, y - bodySize, bodySize, std::wstring(title.begin(), title.end()), quoteBar, true);
        y -= lineHeight;
        if (block.children.empty()) RenderQuote(block.text);
        else RenderQuoteChildren(block.children);
        quoteBar = plainQuoteBar;
    }

    void RenderQuoteChildren(const std::vector<Block>& children) {
        for (const Block& child : children) {
            switch (child.type) {
            case BlockType::Paragraph: RenderQuote(child.text); break;
            case BlockType::Heading: RenderQuoteHeading(child); break;
            case BlockType::Bullet:
                RenderQuote((child.task == 0 ? "- " : child.task == 2 ? "- [x] " : "- [ ] ") + child.text);
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
                item += child.task == 0 ? ". " : child.task == 2 ? ". [x] " : ". [ ] ";
                item += child.text;
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
                // A quote in an alert has a plain quote's bar, as on GitHub.
                double savedMargin = margin;
                const char* savedBar = quoteBar;
                margin += 10.0;
                quoteBar = plainQuoteBar;
                RenderQuote(child);
                margin = savedMargin;
                quoteBar = savedBar;
                break;
            }
            case BlockType::MathBlock: RenderDisplayMath(child.text, true); break;
            case BlockType::Code:
            case BlockType::Table:
            case BlockType::Rule:
            case BlockType::Image: {
                double savedMargin = margin;
                margin += 14.0;
                if (child.type == BlockType::Code) RenderCode(child);
                else if (child.type == BlockType::Table && child.hasMath) RenderMathTable(*this, child);
                else if (child.type == BlockType::Table) RenderTable(child.rows, child.aligns);
                else if (child.type == BlockType::Rule) RenderRule();
                else RenderImage(child);
                margin = savedMargin;
                break;
            }
            case BlockType::PageBreak: RenderPageBreak(); break;
            case BlockType::Contents: break;
            }
        }
    }

    void RenderQuoteHeading(const Block& block) {
        static const double sizes[] = { 0, 18, 16, 14, 13, 12, 11.5 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        double height = size * 1.35;
        double x = margin + 14.0;
        double width = pageW - margin * 2.0 - 22.0;
        if (block.hasMath) {
            RenderMathTextLines(block.text, level, x, width, size, "0.10 0.15 0.18", true, true);
            y -= 7.0;
            return;
        }
        MarkHeading(level, block.text, height + 2.0);
        std::wstring text = Utf8ToWide(block.text);
        for (const std::wstring& line : WrapText(font, text, width, size)) {
            Ensure(height + 2.0);
            QuoteStrip(height + 3.0);
            PaintText(x, y - size, size, line, "0.10 0.15 0.18", true);
            y -= height;
        }
        y -= 7.0;
    }
    void RenderQuote(const std::string& text) {
        double x = margin + 14.0;
        double width = pageW - margin * 2.0 - 22.0;
        if (math.Active()) math.Clear();
        if (WrapStyledRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lineHeight + 2.0);
                QuoteStrip(lineHeight + 3.0);
                PaintStyledRuns(paragraphRuns, index, lineEnd, x, y, y - bodySize, bodySize, lineHeight, kQuoteRunColors);
                index = lineEnd;
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
            QuoteStrip(lineHeight + 3.0);
            double cursor = x;
            double baseline = y - bodySize;
            for (const StyledSpan& span : line) {
                double spanWidth = TextWidth(font, span.text, bodySize);
                if (span.code) CodeBackground(cursor, y, spanWidth, lineHeight, span.text, bodySize, "0.88 0.89 0.88");
                const char* color = span.url.empty() ? (span.code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : linkColor;
                PaintText(cursor, baseline, bodySize, span.text, color, span.bold, span.italic, span.strike);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight;
        }
        y -= 7.0;
    }

    void RenderCode(const Block& block) {
        if (block.codeLanguage != 0 && highlightCode) {
            RenderHighlightedCode(block.text, block.codeLanguage);
            return;
        }
        std::vector<std::string> raw = SplitLines(block.text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = pageW - margin * 2.0 - 16.0;
        bool first = true;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                CodeTile(lh, first, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "0.12 0.12 0.12");
            }
        }
        y -= 8.0;
    }

    // Makes room for a line of code `lh` high and draws its tile. The first line of a block on a
    // page has one that reaches 3 points above it, the others one that starts at their top, below
    // the descenders of the line before: drawn over them, it hid underscores. Each tile reaches 2
    // points into the next, so that the block is one tint. Out of line: written into RenderCode,
    // it moved GCC's inlining and outlined "- " + text in RenderBullet (+0.3 % on baseline.md).
    RAYOMD_SHARED void CodeTile(double lh, bool& first, const char* color) {
        const size_t pages = pageStarts.size();
        Ensure(lh + 4.0);
        if (pageStarts.size() != pages) first = true;
        const double pad = first ? 3.0 : 0.0;
        DrawRect(margin, y + pad, pageW - margin * 2.0, lh + 2.0 + pad, color);
        first = false;
    }

    // A fenced block in a language the lexer knows (Block::codeLanguage): RenderCode's lines and
    // tiles, each run of characters in the colour of its token class; a diff's added and removed
    // lines on tinted tiles. RenderCode decodes each line; here the lexer reads the whole block at
    // once, with each UTF-16 unit above ASCII as one byte from 0x80, which it takes for a letter,
    // so that the classes are those of the units drawn. Out of line, but not cold: it runs for
    // every character of the code, and a cold function is optimized for size.
    RAYOMD_SHARED void RenderHighlightedCode(const std::string& text, uint8_t language) {
        std::string_view body(text);
        if (body.size() >= 3 && memcmp(body.data(), "\xEF\xBB\xBF", 3) == 0) body.remove_prefix(3);   // as SplitLines
        std::wstring wide = Utf8ToWide(body);
        codeText.resize(wide.size());
        for (size_t at = 0; at < wide.size(); at++) {
            codeText[at] = wide[at] < 0x80 ? (char)wide[at] : '\x80';
            if (wide[at] == L'\t') wide[at] = L' ';   // as WrapCodeLine draws a tab
        }
        codeClasses.resize(wide.size());
        Internal::ClassifyCode(language, codeText, codeClasses.data());
        const double size = 9.5;
        const double lh = size * 1.35;
        const double x = margin + 8.0;
        const double width = pageW - margin * 2.0 - 16.0;
        const double scale = size * 0.001;
        bool first = true;
        for (size_t start = 0;;) {
            size_t end = wide.find(L'\n', start);
            const bool last = end == std::wstring::npos;
            if (last) end = wide.size();
            const size_t lineEnd = end > start && wide[end - 1] == L'\r' ? end - 1 : end;   // as SplitLines
            const char* tile = Internal::CodeLineTile(lineEnd > start ? codeClasses[start] : (uint8_t)Internal::kCodePlain);
            auto draw = [&](size_t from, size_t to) {
                CodeTile(lh, first, tile != nullptr ? tile : "0.95 0.95 0.93");
                PaintCodeRuns(x, y - size, size, std::wstring_view(wide.data() + from, to - from), codeClasses.data() + from);
                y -= lh;
            };
            // WrapCodeLine's pieces, read where they are: one ends before the character that would
            // make it wider than the line, and holds at least one. The same sums, so the same breaks.
            size_t piece = start;
            double pieceWidth = 0.0;
            for (size_t at = start; at < lineEnd; at++) {
                const double charWidth = CodepointWidth(font, (uint16_t)wide[at], scale);
                if (at > piece && pieceWidth + charWidth > width) {
                    draw(piece, at);
                    piece = at;
                    pieceWidth = 0.0;
                }
                pieceWidth += charWidth;
            }
            draw(piece, lineEnd);
            if (last) break;
            start = end + 1;
        }
        y -= 8.0;
    }

    // A line of highlighted code at (x, baseline): as PaintText when it is one colour, otherwise
    // one text object with each run after its colour.
    RAYOMD_SHARED void PaintCodeRuns(double x, double baseline, double size, std::wstring_view text, const uint8_t* classes) {
        SplitCodeRuns(text.data(), text.size(), classes, codeRuns);
        if (codeRuns.size() == 1) {
            PaintText(x, baseline, size, text, Internal::CodeColor(codeRuns[0].color));
            return;
        }
        TailWriter w(content, 64 + kOperandBytes * 3 + text.size() * 4 + codeRuns.size() * 32);
        size_t begin = 0;
        for (const CodeRun& run : codeRuns) {
            const char* color = Internal::CodeColor(run.color);
            if (begin == 0) {
                w.Lit("q ");
                w.Bytes(color, strlen(color));
                w.Lit(" rg BT /F1 ");
                w.Fixed(size);
                w.Lit(" Tf 1 0 0 1 ");
                w.Fixed(x);
                w.Lit(" ");
                w.Fixed(baseline);
                w.Lit(" Tm ");
            } else {
                w.Bytes(color, strlen(color));
                w.Lit(" rg ");
            }
            w.cursor = WriteHexText(w.cursor, font, text.substr(begin, run.end - begin), usedCids);
            w.Lit(" Tj ");
            begin = run.end;
        }
        w.Lit("ET Q\n");
    }

    // A book's title, or a part's, alone on a page and centred a little above its middle, as a
    // level-1 heading is drawn; with `mark` its bookmark (RenderBook).
    RAYOMD_COLD void RenderTitlePage(const std::string& text, bool mark) {
        RenderPageBreak();
        const double size = kHeadingSizes[1];
        const std::vector<std::wstring> lines = WrapText(font, Utf8ToWide(text), pageW - margin * 2.0, size);
        y = std::min(pageH - margin, pageH * 0.6 + (double)lines.size() * size * 1.35 * 0.5);
        if (mark) MarkHeading(1, text, 0.0);
        for (const std::wstring& line : lines) DrawTextLine((pageW - TextWidth(font, line, size)) * 0.5, size, line, headingColor);
    }

    // The table of contents (contents.h), once: a title, then each entry indented by its level and
    // linked to its heading, the top level in bold with room above it. The page numbers come later
    // (ContentsNumbers), so the entries' text ends 2.55 em before the right margin.
    RAYOMD_COLD void RenderContents() {
        if (contents == nullptr || contents->empty() || !contentsLines.empty()) return;
        const double titleSize = kHeadingSizes[1];
        if (y < pageH - margin - 4.0) y -= 12.0;
        Ensure(titleSize * 1.35 + lineHeight * 2.0);
        DrawTextLine(margin, titleSize, std::wstring(L"Contents"), headingColor);
        y -= 8.0;
        const double em = bodySize;
        const double lh = bodySize * 1.35;
        contentsLines.resize(contents->size());
        for (size_t index = 0; index < contents->size(); index++) {
            const Internal::ContentsEntry& entry = (*contents)[index];
            const bool top = entry.level == 1;
            if (top && index > 0) y -= em * 0.5;
            const double x = margin + (entry.level - 1) * 1.5 * em;
            char url[24] = "#\x01";
            const std::string_view link(url, (size_t)(std::to_chars(url + 2, url + sizeof(url), index).ptr - url));
            ContentsLine& last = contentsLines[index];
            last = { index, pageStarts.size() - 1, y - bodySize, x };
            for (const std::wstring& line : WrapText(font, Utf8ToWide(entry.text), pageW - margin - x - 2.55 * em, bodySize)) {
                Ensure(lh);
                const double baseline = y - bodySize;
                const double width = TextWidth(font, line, bodySize);
                PaintText(x, baseline, bodySize, line, "0.08 0.08 0.08", top);
                AddLink(x, baseline, width, bodySize, link);
                last = { index, pageStarts.size() - 1, baseline, x + width };
                y -= lh;
            }
        }
        y -= 5.0;
    }

    // Text at (x, baseline) in the body size, appended to `out`, a stream of its own; `spacing` is
    // added after each character (Tc), and `bold` draws it twice, a little apart, as PaintText does.
    RAYOMD_COLD void AppendOverlayText(std::string& out, double x, double baseline, std::wstring_view text,
        const char* color, bool bold, double spacing) {
        TailWriter w(out, 128 + kOperandBytes * 6 + HexTextBytes(text) * 2);
        w.Lit("q ");
        w.Bytes(color, strlen(color));
        w.Lit(" rg BT /F1 ");
        w.Fixed(bodySize);
        w.Lit(" Tf ");
        if (spacing > 0.0) {
            w.Fixed(spacing);
            w.Lit(" Tc ");
        }
        w.Lit("1 0 0 1 ");
        w.Fixed(x);
        w.Lit(" ");
        w.Fixed(baseline);
        w.Lit(" Tm ");
        const char* hex = w.cursor;
        w.cursor = WriteHexText(w.cursor, font, text, usedCids);
        const size_t hexSize = (size_t)(w.cursor - hex);
        w.Lit(" Tj");
        if (bold) {
            w.Lit(" 1 0 0 1 ");
            w.Fixed(x + 0.28);
            w.Lit(" ");
            w.Fixed(baseline);
            w.Lit(" Tm ");
            w.Bytes(hex, hexSize);
            w.Lit(" Tj");
        }
        w.Lit(" ET Q\n");
    }

    void RenderMath(const std::string& text) {
        RenderDisplayMath(text, false);
    }

    // Display math: one unbreakable formula, centred in the available width.
    // `quoted` draws it inside the block-quote strip. A formula that is empty or
    // does not fit the page even at half size is shown as its source.
    RAYOMD_MATH_COLD void RenderDisplayMath(const std::string& tex, bool quoted) {
        double left = quoted ? margin + 14.0 : margin;
        double available = quoted ? pageW - margin * 2.0 - 22.0 : pageW - margin * 2.0;
        double padTop = quoted ? kQuoteMathPad : kDisplayMathAbove;
        double padBottom = quoted ? kQuoteMathPad : 0.0;
        double maxHeight = pageH - margin * 2.0 - padTop - padBottom - 3.0;
        MathFormula formula;
        if (math.Disabled() || !LayoutMathToFit(tex, bodySize, true, false, available, maxHeight, &mathFallback, formula)) {
            double savedMargin = margin;
            if (quoted) margin += 14.0;
            RenderMathSource(tex);
            margin = savedMargin;
            return;
        }
        double total = padTop + formula.Ascent() + formula.Descent() + padBottom;
        Ensure(total + 2.0);
        if (quoted) {
            QuoteStrip(total + 3.0);
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
        double width = pageW - margin * 2.0 - 24.0;
        bool first = true;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                size_t pageCount = pageStarts.size();
                Ensure(pitch + 8.0);
                if (pageStarts.size() != pageCount) first = true;
                // One tile per line, flush with its neighbours, so the tint never covers text.
                double pad = first ? 4.0 : 0.0;
                DrawRect(margin, y + pad, pageW - margin * 2.0, pitch + pad, "0.97 0.97 0.95");
                DrawTextLine(x, size, line, "0.10 0.10 0.10");
                first = false;
            }
        }
        DrawRect(margin, y, pageW - margin * 2.0, 4.0, "0.97 0.97 0.95");
        y -= 12.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;

        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = pageW - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        double cellTextWidth = std::max(16.0, colWidth - pad * 2.0);
        // Grows rarely (to the widest table); a new vector keeps resize code out of the binary.
        if (tableCells.size() < columns) tableCells = std::vector<StyledRuns>(columns);
        static const std::string emptyCell;

        y -= 3.0;
        // A table that goes on to a new page repeats its header row there, unless it and the row
        // that moved do not fit a page together: the loop draws row 0 again, then that row.
        double headerHeight = 0.0;  // the header row's height; 0 when it was cut into slices
        size_t resumeRow = 0;       // the row to draw after a repeated header row, or 0
        for (size_t r = 0; r < rows.size(); r++) {
            size_t maxLines = 1;
            for (size_t c = 0; c < columns; c++) {
                WrapStyledInline(c < rows[r].size() ? rows[r][c] : emptyCell, cellTextWidth, size, tableCells[c]);
                maxLines = std::max(maxLines, tableCells[c].lineEnds.size());
            }

            // A row taller than a page is drawn in slices, each with cell borders of its own.
            size_t first = 0;
            size_t count = maxLines;
            double rowHeight = maxLines * lh + pad * 2.0;
            if (rowHeight + 5.0 > pageH - margin * 2.0) {
                count = TallTableRowSlice(first, maxLines, lh, pad);
            } else if (r == 0) {
                // The header row keeps a line of the next row with it.
                headerHeight = rowHeight;
                Ensure(rowHeight + 5.0 + (rows.size() > 1 ? lh + pad * 2.0 : 0.0));
            } else if (y - rowHeight - 5.0 < margin) {
                NewPage();
                if (headerHeight > 0.0 && headerHeight + rowHeight + 5.0 <= pageH - margin * 2.0) {
                    resumeRow = r;
                    r = static_cast<size_t>(-1);
                    continue;
                }
            }
            for (;;) {
                rowHeight = count * lh + pad * 2.0;
                double top = y;
                if (r == 0) DrawRect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");

                for (size_t c = 0; c < columns; c++) {
                    double cellX = margin + c * colWidth;
                    DrawStrokeRect(cellX, top, colWidth, rowHeight);

                    int align = c < aligns.size() ? aligns[c] : -1;
                    const StyledRuns& cellRuns = tableCells[c];
                    const size_t end = std::min(cellRuns.lineEnds.size(), first + count);
                    for (size_t lineIdx = first; lineIdx < end; lineIdx++) {
                        const size_t runBegin = lineIdx == 0 ? 0 : cellRuns.lineEnds[lineIdx - 1];
                        const size_t runEnd = cellRuns.lineEnds[lineIdx];
                        double tx = cellX + pad;
                        if (align == 0 || align == 1) {
                            double lineWidth = 0.0;
                            for (size_t run = runBegin; run < runEnd; run++) {
                                lineWidth += TextWidth(font, cellRuns.Text(cellRuns.runs[run]), size);
                            }
                            if (align == 0) tx = cellX + (colWidth - lineWidth) * 0.5;
                            else tx = cellX + colWidth - pad - lineWidth;
                        }

                        double baseline = top - pad - size - (lineIdx - first) * lh;
                        PaintStyledRuns(cellRuns, runBegin, runEnd, tx, top - pad - (lineIdx - first) * lh, baseline, size,
                            lh, r == 0 ? kHeaderCellRunColors : kCellRunColors, r == 0);
                    }
                }

                y -= rowHeight;
                first += count;
                if (first == maxLines) break;
                count = TallTableRowSlice(first, maxLines, lh, pad);
            }
            if (resumeRow != 0) {
                r = resumeRow - 1;
                resumeRow = 0;
            }
        }
        y -= 9.0;
    }

    // How many lines of a table row taller than a page go on this page from line `first` on:
    // as many as fit, at least one. The rest of a row, or a row with no room left here, goes
    // on a new page. Drawing stays in RenderTable, so this rare path adds no caller to the
    // helpers the table loop inlines.
    RAYOMD_COLD size_t TallTableRowSlice(size_t first, size_t lineCount, double lh, double pad) {
        auto fitting = [&]() {
            const double room = y - margin - 5.0 - pad * 2.0;
            return room >= lh ? static_cast<size_t>(room / lh) : size_t(0);
        };
        if (first > 0) NewPage();
        size_t fit = fitting();
        if (fit == 0) {
            NewPage();
            fit = std::max<size_t>(1, fitting());
        }
        return std::min(fit, lineCount - first);
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = content;
        c += "q ";
        c += ruleColor;
        c += " RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, pageW - margin);
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

// Whether the glyph data and the tables a subset rewrites can be read: then a subset of the
// font is built, else the whole font is embedded with its own glyph ids.
static bool CanSubset(const TtfFont& font) {
    auto sized = [&](const char* tag, uint32_t size) {
        auto it = font.tables.find(tag);
        return it != font.tables.end() && it->second.length >= size;
    };
    return font.glyphCount != 0 && font.loca.size() >= (size_t)font.glyphCount + 1 && font.advances.size() >= font.glyphCount &&
        sized("glyf", 0) && sized("hmtx", 0) && sized("head", 54) && sized("hhea", 36) && sized("maxp", 6);
}

// The glyphs of the subset for `used`, by their ids in the font, which is also the order of
// their ids in the subset: .notdef, '?', the space, the glyph of every used CID and the
// components of composite glyphs.
static std::vector<uint16_t> SubsetGlyphs(const TtfFont& font, const CidList& used) {
    std::vector<uint8_t> include((size_t)font.glyphCount, 0);
    AddGlyphClosure(font, 0, include);
    AddGlyphClosure(font, font.GlyphFor('?'), include);
    AddGlyphClosure(font, font.GlyphFor(' '), include);
    for (uint16_t cid : used) AddGlyphClosure(font, font.GlyphFor(cid), include);
    std::vector<uint16_t> glyphs;
    for (uint32_t glyph = 0; glyph < font.glyphCount; glyph++) {
        if (include[glyph]) glyphs.push_back((uint16_t)glyph);
    }
    return glyphs;
}

// The id in the subset of `glyphs` of the font's glyph `glyph`; .notdef when it is not in it.
static uint16_t SubsetGlyphId(const std::vector<uint16_t>& glyphs, uint16_t glyph) {
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), glyph);
    return it != glyphs.end() && *it == glyph ? (uint16_t)(it - glyphs.begin()) : 0;
}

// A TrueType font of `glyphs` (SubsetGlyphs) only, numbered from 0 in that order: loca and
// hmtx hold those glyphs, a composite glyph points at the new ids of its components, post
// keeps no glyph names, and the cmap maps nothing, since the PDF's CIDToGIDMap does. The
// font must pass CanSubset.
static bool BuildSubsetFontBytes(const TtfFont& font, const std::vector<uint16_t>& glyphs, std::string& out) {
    const TtfFont::Table& oldGlyf = font.tables.at("glyf");
    const TtfFont::Table& oldHmtx = font.tables.at("hmtx");
    std::string glyfData;
    std::string locaData;
    std::string hmtxData;
    locaData.reserve((glyphs.size() + 1) * 4);
    hmtxData.reserve(glyphs.size() * 4);
    for (uint16_t glyph : glyphs) {
        AppendU32(locaData, (uint32_t)glyfData.size());
        // Every glyph gets a full metric; past numberOfHMetrics the font lists only bearings.
        const size_t bearing = glyph < font.metricCount ? (size_t)glyph * 4 + 2
            : (size_t)font.metricCount * 4 + (size_t)(glyph - font.metricCount) * 2;
        AppendU16(hmtxData, font.advances[glyph]);
        AppendU16(hmtxData, bearing + 2 <= oldHmtx.length ? ReadU16(font.bytes, oldHmtx.offset + bearing) : 0);

        const uint32_t start = font.loca[glyph];
        const uint32_t end = font.loca[(size_t)glyph + 1];
        if (end <= start) continue;
        const size_t at = glyfData.size();
        glyfData.append((const char*)font.bytes.data() + oldGlyf.offset + start, end - start);
        if (end - start >= 10 && ReadS16(font.bytes, (size_t)oldGlyf.offset + start) < 0) {
            // Composite: each component names a glyph, rewritten to its id in the subset.
            for (size_t component = at + 10; component + 4 <= glyfData.size();) {
                const uint16_t flags = (uint16_t)(((uint8_t)glyfData[component] << 8) | (uint8_t)glyfData[component + 1]);
                const uint16_t old = (uint16_t)(((uint8_t)glyfData[component + 2] << 8) | (uint8_t)glyfData[component + 3]);
                SetU16(glyfData, component + 2, SubsetGlyphId(glyphs, old));
                component += 4 + ((flags & 0x0001) ? 4 : 2);
                if (flags & 0x0008) component += 2;
                else if (flags & 0x0040) component += 4;
                else if (flags & 0x0080) component += 8;
                if (!(flags & 0x0020)) break;
            }
        }
        while (glyfData.size() & 3) glyfData.push_back('\0');
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
            table = std::move(glyfData);
        } else if (tagString == "loca") {
            table = std::move(locaData);
        } else if (tagString == "hmtx") {
            table = std::move(hmtxData);
        } else if (tagString == "cmap") {
            // Version 0, one Windows Unicode subtable of format 4 with the closing segment only.
            for (int value : {0, 1, 3, 1, 0, 12, 4, 24, 0, 2, 2, 0, 0, 0xFFFF, 0, 0xFFFF, 1, 0}) AppendU16(table, (uint16_t)value);
        } else {
            if (!OriginalTableBytes(font, tagString, table)) continue;
            if (tagString == "head") {
                SetU32(table, 8, 0);
                SetU16(table, 50, 1);
            } else if (tagString == "hhea") {
                SetU16(table, 34, (uint16_t)glyphs.size());
            } else if (tagString == "maxp") {
                SetU16(table, 4, (uint16_t)glyphs.size());
            } else if (tagString == "post") {
                if (table.size() < 32) continue;
                table.resize(32);
                SetU32(table, 0, 0x00030000);
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
    AppendU32(out, font.sfntVersion);
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

// The font program FlateDecode-compressed, or as BuildFontFileObject has it when that would not
// be smaller. /Length1 stays the size of the font itself.
RAYOMD_COLD static std::string BuildFlateFontFileObject(const std::string& fontBytes) {
    FlateWork& work = ThreadFlateWork();
    const size_t packed = Deflate(fontBytes, work);
    if (packed == 0) return BuildFontFileObject(fontBytes);
    std::string body;
    body.reserve(packed + 128);
    body += "<< /Filter /FlateDecode /Length ";
    AppendSize(body, packed);
    body += " /Length1 ";
    AppendSize(body, fontBytes.size());
    body += " >>\nstream\n";
    body.append(work.out.data(), packed);
    body += "\nendstream";
    TrimFlateWork(work);
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

// The font program of a document: the subset of the glyphs it shows, or the whole font when
// it cannot be subset (CanSubset). With `compress` FlateDecode-compressed, in a cache of its own.
static std::shared_ptr<const std::string> CachedFontFileObject(
    const TtfFont& font, const CidList& used, const std::string& key, bool compress) {
    static BoundedStringCache plainCache(8, 32u * 1024u * 1024u);
    static BoundedStringCache flateCache(8, 32u * 1024u * 1024u);
    BoundedStringCache& cache = compress ? flateCache : plainCache;
    if (auto cached = cache.Get(key)) return cached;

    std::string bytes;
    if (!CanSubset(font) || !BuildSubsetFontBytes(font, SubsetGlyphs(font, used), bytes)) {
        bytes.assign((const char*)font.bytes.data(), font.bytes.size());
    }
    return cache.Insert(key, compress ? BuildFlateFontFileObject(bytes) : BuildFontFileObject(bytes));
}

// The glyph id of every used CID in the font program of CachedFontFileObject.
static std::shared_ptr<const std::string> MakeCidToGidMap(
    const TtfFont& font, const CidList& used, const std::string& key) {
    static BoundedStringCache cache(64, 8u * 1024u * 1024u);
    if (auto cached = cache.Get(key)) return cached;
    uint16_t maxCid = 255;
    for (uint16_t cid : used) maxCid = std::max(maxCid, cid);
    std::string map((size_t)(maxCid + 1) * 2, char(0));
    const bool subset = CanSubset(font);
    const std::vector<uint16_t> glyphs = subset ? SubsetGlyphs(font, used) : std::vector<uint16_t>();
    for (uint16_t cid : used) {
        const uint16_t glyph = subset ? SubsetGlyphId(glyphs, font.GlyphFor(cid)) : font.GlyphFor(cid);
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

// `text` as the bytes of a PDF literal string, ( ) and the backslash escaped and control bytes
// dropped, written straight into a buffer with room for two bytes per input byte.
// Returns the end of what was written.
static char* WriteEscapedLiteral(char* out, std::string_view text) {
    // Most lines hold no parenthesis, backslash or control byte and are copied in one piece.
    // Bytes from 0x80 are WinAnsi codes: the standard renderer gets ASCII or transcoded text.
    if (!RayoMd::Text::ContainsByteClass(text, RayoMd::Text::kByteLiteralSpecial)) {
        if (!text.empty()) memcpy(out, text.data(), text.size());
        return out + text.size();
    }
    for (char c : text) {
        if ((unsigned char)c < 32 || (unsigned char)c == 127) continue;
        if (c == '(' || c == ')' || c == '\\') *out++ = '\\';
        *out++ = c;
    }
    return out;
}

// True for text without a byte >= 0x80, which the standard renderer takes as it is. Other
// text reaches it transcoded to WinAnsiEncoding, or takes the Unicode renderer.
static bool IsPlainAsciiDocument(const std::string& s) {
#ifdef FAST_MD_SSE2
    const char* ptr = s.data();
    const char* end = ptr + s.size();
    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128((const __m128i*)ptr);
        if (_mm_movemask_epi8(chunk) != 0) return false;
        ptr += 16;
    }
    while (ptr < end) {
        if ((unsigned char)*ptr >= 128) return false;
        ptr++;
    }
    return true;
#else
    return IsAllAscii(s);
#endif
}

using Internal::StandardTextFont;
using Internal::StandardWordAdvances;

// The standard renderer measures text with the AFM advances of the font that shows it
// (math_layout.h), in units of 1/1000 em, so a line is exactly as wide as the viewer draws it.
static const StandardWordAdvances& WordAdvances(StandardTextFont font) {
    return Internal::kStandardWordAdvances[static_cast<size_t>(font)];
}

static double UnitsToPoints(uint64_t units, double size) {
    return static_cast<double>(units) * size / 1000.0;
}

// The most AFM units a line `points` wide holds at `size`. Units are whole numbers, so "more
// units than this" is exactly "wider than the line".
static uint64_t MaxUnits(double points, double size) {
    const double units = points * 1000.0 / size;
    return units > 0.0 ? static_cast<uint64_t>(units) : 0;
}

// Width of one byte of a word; white space has no width here.
static double AsciiCharWidth(char c, double size, StandardTextFont font) {
    return UnitsToPoints(WordAdvances(font).byte[static_cast<unsigned char>(c)], size);
}

// Width of `text`, spaces included.
static double AsciiTextWidth(std::string_view text, double size, StandardTextFont font) {
    const StandardWordAdvances& advances = WordAdvances(font);
    uint64_t units = 0;
    for (unsigned char c : text) units += c == ' ' ? advances.space : advances.byte[c];
    return UnitsToPoints(units, size);
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

// Greedy word wrap of text without inline syntax, shown in `font`: emit(line, width) receives
// every line. A line is the piece of `text` from its first to its last word; only when two of
// its words are not exactly one space apart is it rebuilt in a scratch string. The view
// passed to emit is valid until emit returns.
template <typename Emit>
static void WrapAsciiWords(std::string_view text, double maxWidth, double size, StandardTextFont font, Emit emit) {
    const StandardWordAdvances& advances = WordAdvances(font);
    const uint64_t spaceUnits = advances.space;
    const uint64_t maxUnits = MaxUnits(maxWidth, size);
    std::string respaced;
    size_t lineStart = 0;
    size_t lineEnd = 0;         // the line is text[lineStart, lineEnd); empty when they are equal
    bool singleSpaced = true;
    uint64_t lineUnits = 0;
    bool emitted = false;
    auto flush = [&]() RAYOMD_HOT_LAMBDA {
        std::string_view line = text.substr(lineStart, lineEnd - lineStart);
        if (!singleSpaced) {
            respaced.clear();
            AppendSingleSpaced(respaced, line);
            line = respaced;
        }
        emit(line, UnitsToPoints(lineUnits, size));
        emitted = true;
    };
    auto startLine = [&](size_t start, size_t end, uint64_t units) {
        lineStart = start;
        lineEnd = end;
        lineUnits = units;
        singleSpaced = true;
    };

    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && advances.byte[static_cast<unsigned char>(text[i])] == 0) i++;
        const size_t start = i;
        uint64_t wordUnits = 0;
        for (; i < text.size(); i++) {
            const unsigned charUnits = advances.byte[static_cast<unsigned char>(text[i])];
            if (charUnits == 0) break;
            wordUnits += charUnits;
        }
        if (i == start) continue;
        const bool lineEmpty = lineEnd == lineStart;

        if (!lineEmpty && lineUnits + spaceUnits + wordUnits <= maxUnits) {
            if (start - lineEnd != 1 || text[lineEnd] != ' ') singleSpaced = false;
            lineEnd = i;
            lineUnits += spaceUnits + wordUnits;
            continue;
        }
        if (lineEmpty && wordUnits <= maxUnits) {
            startLine(start, i, wordUnits);
            continue;
        }
        if (!lineEmpty) {
            flush();
            lineStart = lineEnd = 0;
            lineUnits = 0;
        }
        if (wordUnits <= maxUnits) {
            startLine(start, i, wordUnits);
            continue;
        }

        // A word wider than the line is cut into pieces; the last piece stays open.
        size_t partStart = start;
        uint64_t partUnits = 0;
        for (size_t at = start; at < i; at++) {
            const unsigned charUnits = advances.byte[static_cast<unsigned char>(text[at])];
            if (at > partStart && partUnits + charUnits > maxUnits) {
                emit(text.substr(partStart, at - partStart), UnitsToPoints(partUnits, size));
                emitted = true;
                partStart = at;
                partUnits = 0;
            }
            partUnits += charUnits;
        }
        startLine(partStart, i, partUnits);
    }
    if (lineEnd != lineStart) flush();
    if (!emitted) emit(std::string_view(""), 0.0);
}

// Wraps text the parser already stripped. It is read a second time, with math delimiters
// literal; text without inline syntax, the usual case, is wrapped in place.
template <typename Emit>
static void ForEachWrappedAsciiLine(const std::string& raw, double maxWidth, double size, StandardTextFont font,
    Emit emit) {
    if (!RayoMd::Text::ContainsByteClass(raw, RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead)) {
        WrapAsciiWords(raw, maxWidth, size, font, emit);
        return;
    }
    const std::string text = StripInlineMarkdown(raw, false);
    WrapAsciiWords(text, maxWidth, size, font, emit);
}

static void WrapAsciiText(const std::string& raw, double maxWidth, double size,
    StandardTextFont font, std::vector<WrappedAsciiLine>& lines) {
    lines.clear();
    ForEachWrappedAsciiLine(raw, maxWidth, size, font, [&](std::string_view line, double width) {
        lines.emplace_back();
        lines.back().text.assign(line.data(), line.size());
        lines.back().width = width;
    });
}

static std::vector<WrappedAsciiLine> WrapAsciiText(
    const std::string& raw, double maxWidth, double size, StandardTextFont font) {
    std::vector<WrappedAsciiLine> lines;
    WrapAsciiText(raw, maxWidth, size, font, lines);
    return lines;
}

// Lines of code and of formula source, shown in Courier: every printable byte is 600 units
// wide and a line breaks at the first byte that no longer fits.
static std::vector<std::string> WrapAsciiLiteral(std::string_view raw, double maxWidth, double size) {
    const uint64_t charUnits = WordAdvances(StandardTextFont::Mono).space;
    const uint64_t maxUnits = MaxUnits(maxWidth, size);
    std::vector<std::string> lines;
    std::string line;
    line.reserve(std::min<size_t>(raw.size(), 256));
    uint64_t lineUnits = 0;

    for (char ch : raw) {
        if (ch == '\t') ch = ' ';
        if ((unsigned char)ch < 32 || (unsigned char)ch >= 127) continue;
        if (!line.empty() && lineUnits + charUnits > maxUnits) {
            lines.push_back(line);
            line.clear();
            lineUnits = 0;
        }
        line.push_back(ch);
        lineUnits += charUnits;
    }

    lines.push_back(line);
    return lines;
}

class StandardRenderer {
public:
    // The content streams of all pages are appended to `output`, one after another. With
    // `winAnsi`, the text is a document transcoded to WinAnsiEncoding (Latin text).
    StandardRenderer(std::string& output, PdfStyle styleValue, const PdfMargin& marginValue, const PdfPageSize& pageSize,
        ImageRegistry* imageRegistry, bool winAnsi, const ThemePalette& palette)
        : images(imageRegistry), style(styleValue), winAnsiText(winAnsi), content(output), headingColor(palette.heading),
          linkColor(palette.link), ruleColor(palette.rule), plainQuoteBar(palette.quoteBar), quoteBar(palette.quoteBar) {
        const PdfPageSize page = ResolvePageSize(pageSize);
        pageW = page.width;
        pageH = page.height;
        margin = ResolveMarginPoints(marginValue, page);
        bodySize = style == PdfStyle::Tech ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        latinMathFallback = { this, &StandardRenderer::MeasureLatinMathFallback,
            &StandardRenderer::EmitLatinMathFallback, 0.718, 0.207 };
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) { RenderBlocks(*this, blocks); }
    // The footnotes the text refers to (ParseMarkdown), which RenderFootnotes draws after it.
    void SetFootnotes(const Internal::Footnotes* notes) { footnotes = notes; }
    void RenderFootnotes() {
        if (footnotes != nullptr && !footnotes->notes.empty()) RenderFootnoteSection(*this, *footnotes);
    }
    // The entries of the table of contents (contents.h), which RenderContents draws.
    void SetContents(const std::vector<Internal::ContentsEntry>* entries) { contents = entries; }

    // The page numbers of the table's entries, right-aligned on the last line of each and after a
    // row of dots below the top level: one stream for each page, for the builder to add to the
    // table's. `pages[i]` is the page of entry i's heading, npos for one not drawn.
    RAYOMD_COLD void ContentsNumbers(const std::vector<size_t>& pages, std::vector<std::string>& overlays) const {
        overlays.resize(pageStarts.size());
        const double em = bodySize;
        const double dotWidth = Internal::StandardTextWidth(".", bodySize, StandardTextFont::Regular);
        const double pitch = em * 0.5 + dotWidth;
        for (const ContentsLine& line : contentsLines) {
            if (pages[line.entry] == std::string::npos) continue;
            char digits[24];
            const std::string_view number(digits,
                (size_t)(std::to_chars(digits, digits + sizeof(digits), pages[line.entry] + 1).ptr - digits));
            const bool top = (*contents)[line.entry].level == 1;
            const double numberLeft = pageW - margin -
                Internal::StandardTextWidth(number, bodySize, top ? StandardTextFont::Bold : StandardTextFont::Regular);
            std::string& out = overlays[line.page];
            // The dots stand on a grid from the left margin, so that those of all entries line up.
            const double first = margin + std::ceil((line.end + em * 0.3 - margin) / pitch) * pitch;
            const double limit = numberLeft - em * 0.3 - dotWidth;
            if (!top && first <= limit) {
                const std::string dots((size_t)((limit - first) / pitch) + 1, '.');
                AppendOverlayText(out, first, line.baseline, dots, "F1", "0.45 0.45 0.45", pitch - dotWidth);
            }
            AppendOverlayText(out, numberLeft, line.baseline, number, top ? "F2" : "F1", "0.08 0.08 0.08", 0.0);
        }
    }

    // Offset in the output buffer at which the content stream of each page starts; with
    // --compress, CompressPageContents moves the streams and these with them.
    std::vector<size_t>& PageStarts() { return pageStarts; }
    const std::vector<std::vector<LinkRect>>& PageLinks() const { return pageLinks; }
    bool MathUsed() const { return math.Used(); }
    const std::vector<HeadingMark>& Headings() const { return headings; }
    const NoteMarks& Notes() const { return noteMarks; }
    double Margin() const { return margin; }
    double PageWidth() const { return pageW; }
    double PageHeight() const { return pageH; }
    // Whether text was shown in /F4 Helvetica-Oblique or /F5 Helvetica-BoldOblique.
    bool ObliqueUsed() const { return (facesUsed & (1u << kStyleItalic)) != 0; }
    bool BoldObliqueUsed() const { return (facesUsed & (1u << (kStyleBold | kStyleItalic))) != 0; }
    // PdfOptions::highlightCode off: every code block in one colour.
    void DisableHighlighting() { highlightCode = false; }

private:
    template <typename RendererType>
    friend void RenderBlocks(RendererType&, const std::vector<Block>&);
    template <typename RendererType>
    friend void RenderMathTable(RendererType&, const Block&);
    template <typename RendererType>
    friend void RenderFootnoteSection(RendererType&, const Internal::Footnotes&);
    template <typename RendererType>
    friend void RenderBook(RendererType&, ImageRegistry&, const BookSource&, BookParse&, bool);

    ImageRegistry* images = nullptr;
    PdfStyle style = PdfStyle::Elegant;
    bool winAnsiText = false;
    double margin = 54.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    double pageW = 595.0;
    double pageH = 842.0;
    // The page being rendered is the tail of `content`, so painting is a plain append.
    std::string& content;
    std::vector<size_t> pageStarts;
    std::vector<std::vector<LinkRect>> pageLinks;
    std::vector<HeadingMark> headings;
    // The theme's colours, and that of the bar beside quoted text: a plain quote's, or that of
    // the alert it is in.
    const char* const headingColor;
    const char* const linkColor;
    const char* const ruleColor;
    const char* const plainQuoteBar;
    const char* quoteBar;

    // Notes where a heading's first line is drawn, for its outline entry and the links to it.
    // A line `firstLine` high that does not fit moves to the next page first, as drawing it
    // would; 0 notes the current place.
    void MarkHeading(int level, const std::string& text, double firstLine) {
        if (text.empty()) return;
        if (firstLine > 0.0) Ensure(firstLine);
        headings.push_back({ level, text, pageStarts.size() - 1, y });
    }
    MathPool math;
    const Internal::Footnotes* footnotes = nullptr;
    NoteMarks noteMarks;
    MathFallbackFont latinMathFallback{};
    // Highlighted code: whether to highlight it, the classes of a block's bytes, one of its lines
    // as WrapAsciiLiteral keeps it when that is not the line as it is, with the classes of its
    // bytes, and the runs of one colour of a line, kept from block to block.
    bool highlightCode = true;
    std::vector<uint8_t> codeClasses;
    std::string codeLine;
    std::vector<uint8_t> codeLineClasses;
    std::vector<CodeRun> codeRuns;
    // The table of contents' entries, and where each ended (RenderContents).
    const std::vector<Internal::ContentsEntry>* contents = nullptr;
    std::vector<ContentsLine> contentsLines;

    double MaxMathHeight() const {
        return (pageH - margin * 2.0) * 0.5;
    }

    // Latin text in a formula (\text{café}, a degree sign) is measured and painted in
    // Helvetica at the text size the formula stands in, as the Unicode renderer does with its
    // font. ASCII documents keep no fallback, so their formulas lay out as they always did.
    static double MeasureLatinMathFallback(void*, std::string_view utf8, double size) {
        std::string text;
        RayoMd::Text::TranscodeToWinAnsiLossy(utf8, text);
        return AsciiTextWidth(text, size / kMathSizeFactor, StandardTextFont::Regular);
    }

    static void EmitLatinMathFallback(void*, std::string& content, std::string_view utf8, double x,
        double baseline, double size, const char* rgb) {
        std::string text;
        RayoMd::Text::TranscodeToWinAnsiLossy(utf8, text);
        TailWriter out(content, 48 + strlen(rgb) + kOperandBytes * 3 + text.size() * 2);
        out.Lit("q ");
        out.Bytes(rgb, strlen(rgb));
        out.Lit(" rg BT /F1 ");
        out.Fixed(size / kMathSizeFactor);
        out.Lit(" Tf 1 0 0 1 ");
        out.Fixed(x);
        out.Lit(" ");
        out.Fixed(baseline);
        out.Lit(" Tm (");
        out.cursor = WriteEscapedLiteral(out.cursor, text);
        out.Lit(") Tj ET Q\n");
    }

    // Formula source as the math module reads it: UTF-8, so transcoded Latin text goes back
    // into `storage`. MathFallback() shows the characters the math fonts do not have.
    std::string_view MathSource(std::string_view tex, std::string& storage) const {
        if (!winAnsiText || IsAllAscii(tex)) return tex;
        storage = RayoMd::Text::WinAnsiToUtf8(tex);
        return storage;
    }
    const MathFallbackFont* MathFallback() const { return winAnsiText ? &latinMathFallback : nullptr; }

    void RenderBullet(const Block& block) {
        if (block.task != 0) {
            RenderTaskItem(block);
            return;
        }
        RenderParagraph("- " + block.text, margin + 16.0 + block.level * 18.0,
            pageW - margin * 2.0 - 16.0 - block.level * 18.0);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    // A task list item: its checkbox where a bullet or number would go, its text after it.
    void RenderTaskItem(const Block& block) {
        const double x = margin + 16.0 + block.level * 18.0;
        const double side = bodySize * 0.7;
        Ensure(bodySize * 1.35);
        AppendCheckbox(content, x, y - bodySize - 0.4, side, block.task == 2);
        RenderParagraph(block.text, x + side + 5.0, pageW - margin * 2.0 - 16.0 - block.level * 18.0 - side - 5.0);
        y -= 2.0;
        if (!block.children.empty()) RenderIndentedBlocks(block.children, 16.0 + block.level * 18.0);
    }

    void RenderNumbered(const Block& block) {
        if (block.task != 0) {
            RenderTaskItem(block);
            return;
        }
        std::string item;
        item.reserve(block.text.size() + 8);
        AppendInt(item, block.number);
        item += ". ";
        item += block.text;
        RenderParagraph(item, margin + 16.0 + block.level * 18.0,
            pageW - margin * 2.0 - 16.0 - block.level * 18.0);
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
        y = pageH - margin;
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

    // The strip behind one line of quoted content, `height` tall: the tint, and the bar at its
    // left in the colour of the quote.
    void QuoteStrip(double height) {
        Rect(margin, y + 2.0, pageW - margin * 2.0, height, "0.94 0.95 0.96");
        Rect(margin, y + 2.0, 3.0, height, quoteBar);
    }

    // Emphasis and code state of a piece of text, one bit each. The standard fonts show bold
    // and italic in faces of their own and strike-through as a line; code keeps Courier
    // whatever its emphasis.
    enum : uint8_t { kStyleBold = 1, kStyleItalic = 2, kStyleCode = 4, kStyleStrike = 8 };
    // A <br> in a run of its own (Internal::InlineRun::lineBreak); never part of a face.
    static constexpr uint8_t kStyleBreak = 16;
    // The state of an Internal::InlineSpan or Internal::InlineRun.
    template <typename Span>
    static uint8_t SpanStyle(const Span& span) {
        return static_cast<uint8_t>((span.bold ? kStyleBold : 0) | (span.italic ? kStyleItalic : 0) |
            (span.code ? kStyleCode : 0) | (span.strike ? kStyleStrike : 0));
    }
    // The same for an Internal::InlineRun, whose <br> (lineBreak) gives a segment of its own.
    static uint8_t SpanStyle(const Internal::InlineRun& run) {
        return static_cast<uint8_t>((run.bold ? kStyleBold : 0) | (run.italic ? kStyleItalic : 0) |
            (run.code ? kStyleCode : 0) | (run.strike ? kStyleStrike : 0) | (run.lineBreak ? kStyleBreak : 0));
    }
    // The font whose advances measure text in `style`: Helvetica-Oblique has those of Helvetica.
    static StandardTextFont StyleFont(uint8_t style) {
        return (style & kStyleCode) ? StandardTextFont::Mono :
            (style & kStyleBold) ? StandardTextFont::Bold : StandardTextFont::Regular;
    }
    // WordAdvances(StyleFont(style)), read from a table of pointers. A loop that looks up a
    // byte at a time then indexes one register; from the computed font index GCC keeps the
    // table base and the font offset apart and adds them for every byte.
    static const StandardWordAdvances& StyleAdvances(uint8_t style) {
        constexpr const StandardWordAdvances* R = &Internal::kStandardWordAdvances[0];
        constexpr const StandardWordAdvances* B = &Internal::kStandardWordAdvances[1];
        constexpr const StandardWordAdvances* M = &Internal::kStandardWordAdvances[2];
        static constexpr const StandardWordAdvances* kByStyle[16] = { R, B, R, B, M, M, M, M, R, B, R, B, M, M, M, M };
        return *kByStyle[style & 15];
    }
    // /F1 Helvetica, /F2 Helvetica-Bold, /F4 Helvetica-Oblique, /F5 Helvetica-BoldOblique or
    // /F3 Courier. The oblique faces go into the PDF only when text was shown in them.
    const char* StyleFontName(uint8_t style) {
        static const char* const kFaceNames[8] = { "F1", "F2", "F4", "F5", "F3", "F3", "F3", "F3" };
        const unsigned face = style & (kStyleBold | kStyleItalic | kStyleCode);
        facesUsed = static_cast<uint8_t>(facesUsed | (1u << face));
        return kFaceNames[face];
    }
    uint8_t facesUsed = 0;      // bit n set: text was shown in the face of style n

    // The background of code `text`, which starts at x and is `width` wide, from its first
    // glyph on: the space shown before a code word stays clear, and so does the word before.
    void CodeBackground(double x, double top, double width, double lh, std::string_view text, double size,
        const char* fill) {
        size_t spaces = 0;
        while (spaces < text.size() && text[spaces] == ' ') spaces++;
        const double skip = UnitsToPoints(WordAdvances(StandardTextFont::Mono).space * spaces, size);
        Rect(x + skip - 1.5, top + 1.0, width - skip + 3.0, lh, fill);
    }

    // A line through struck text, as the Unicode renderer draws it. A leading space of the
    // piece is not struck.
    RAYOMD_COLD void StrikeThrough(double x, double baseline, double width, double size, std::string_view text,
        uint8_t style, const char* color) {
        size_t spaces = 0;
        while (spaces < text.size() && text[spaces] == ' ') spaces++;
        const double skip = UnitsToPoints(StyleAdvances(style).space * spaces, size);
        if (width <= skip) return;
        const size_t colorSize = strlen(color);
        TailWriter out(content, 32 + colorSize + kOperandBytes * 4);
        out.Lit("q ");
        out.Bytes(color, colorSize);
        out.Lit(" RG 0.55 w ");
        out.Fixed(x + skip);
        out.Lit(" ");
        out.Fixed(baseline + size * 0.34);
        out.Lit(" m ");
        out.Fixed(x + width);
        out.Lit(" ");
        out.Fixed(baseline + size * 0.34);
        out.Lit(" l S Q\n");
    }

    struct AsciiSpan {
        std::string text;
        std::string url;
        uint8_t style = 0;  // kStyle* bits
        int math = -1;   // index into `math` when the span is a formula; text is empty then
    };

    struct AsciiWord {
        std::string text;
        std::string url;
        uint8_t style = 0;
        int math = -1;
    };

    void AddLink(double x, double baseline, double width, double size, std::string_view url) {
        if (url.empty() || width <= 0.0 || pageLinks.empty()) return;
        AddLinkRect(pageLinks.back(), x, baseline, width, size, url);
    }

    void PushAsciiSpan(std::vector<AsciiSpan>& spans, std::string&& text, std::string&& url, uint8_t style) {
        if (text.empty()) return;
        if (!spans.empty() && spans.back().url == url && spans.back().style == style && spans.back().math < 0) {
            spans.back().text += text;
        } else {
            spans.emplace_back();
            AsciiSpan& span = spans.back();
            span.text = std::move(text);
            span.url = std::move(url);
            span.style = style;
        }
    }

    // A formula becomes one span that refers to the pool. A formula that is empty
    // or cannot fit the line is shown as its TeX source in code style instead.
    RAYOMD_MATH_COLD void PushAsciiMathSpan(std::vector<AsciiSpan>& spans, const Internal::InlineSpan& span, double size,
        double width, bool bold) {
        const int note = FootnoteNumber(footnotes, span);
        if (note != 0 || span.math == Internal::InlineMath::Note) {
            PushAsciiNoteSpan(spans, note, span.text, size);
            return;
        }
        std::string utf8;
        int index = math.Add(MathSource(span.text, utf8), size, span.math == Internal::InlineMath::Display, bold,
            width, MaxMathHeight(), MathFallback());
        AsciiSpan& added = MathAppend(spans);
        if (index < 0) {
            added.text = MathSourceText(span);
            added.style = kStyleCode;
            math.MarkSourceShown();
            return;
        }
        added.math = index;
    }

    // A footnote reference as one box in the pool; one to a note the document lost, as written.
    RAYOMD_MATH_COLD void PushAsciiNoteSpan(std::vector<AsciiSpan>& spans, int note, const std::string& label, double size) {
        if (note == 0) {
            MathAppend(spans).text = "[^" + label + "]";
            return;
        }
        const std::string number = std::to_string(note);
        const double width = AsciiTextWidth(number, size * kNoteSizeFactor, StandardTextFont::Regular);
        MathAppend(spans).math = math.AddNote(note, width, size);
    }

    // A footnote reference: its number raised, in the link colour, linking to the note ("#^N").
    // The first reference of a note is where the note's number links back to ("#^rN").
    RAYOMD_MATH_COLD double PaintNote(int index, double x, double baseline) {
        const int note = math.Note(index);
        const double size = math.NoteSize(index);
        const double raised = baseline + size * kNoteRiseFactor;
        const double width = math.Width(index);
        const std::string number = std::to_string(note);
        Text(x, raised, size * kNoteSizeFactor, number, "F1", linkColor);
        AddLink(x, raised, width, size * kNoteSizeFactor, "#^" + number);
        noteMarks.MarkReference(note, pageStarts.size() - 1, baseline + size);
        return width;
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
            PushAsciiSpan(spans, std::move(span.text), std::move(span.url), SpanStyle(span));
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
                    word.style = span.style;
                }
                start = end;
            }
        }
        return words;
    }

    // Appends one word to the line being filled, with a leading space when `spaced`. It is
    // merged into the previous span when link, code and emphasis are the same.
    void AppendAsciiWord(std::vector<AsciiSpan>& line, std::string_view word, bool spaced, const std::string& url,
        uint8_t style) {
        if (!line.empty()) {
            AsciiSpan& last = line.back();
            if (last.url == url && last.style == style) {
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
        span.style = style;
    }

    // Exact width of text next to formulas and of heading and table-cell text with formulas,
    // in the font that shows it.
    static double MathTextWidth(std::string_view text, double size, bool mono, bool bold) {
        return Internal::StandardTextWidth(text, size, mono ? Internal::StandardTextFont::Mono :
            (bold ? Internal::StandardTextFont::Bold : Internal::StandardTextFont::Regular));
    }

    // Line filling for text that contains at least one formula. Same greedy rule as
    // WrapAsciiRuns: words that touch in the source (joins) stay together without a space,
    // a formula is one unbreakable word, and a $$...$$ formula gets a line of its own.
    // `bold` text (headings, header cells) is measured in Helvetica-Bold, like bold emphasis.
    RAYOMD_MATH_COLD std::vector<std::vector<AsciiSpan>> WrapAsciiMathWords(const std::vector<AsciiWord>& words,
        const std::vector<unsigned char>& joins, double width, double size, bool bold) {
        std::vector<std::vector<AsciiSpan>> lines;
        std::vector<AsciiSpan> line;
        double lineWidth = 0.0;
        const double spaceWidth = UnitsToPoints(WordAdvances(StandardTextFont::Regular).space, size);
        const double codeSpaceWidth = UnitsToPoints(WordAdvances(StandardTextFont::Mono).space, size);
        auto wordFont = [bold](const AsciiWord& word) {
            return StyleFont(bold ? static_cast<uint8_t>(word.style | kStyleBold) : word.style);
        };
        std::vector<double> widths(words.size());
        for (size_t index = 0; index < words.size(); index++) {
            const AsciiWord& word = words[index];
            widths[index] = word.math >= 0 ? math.Width(word.math) : AsciiTextWidth(word.text, size, wordFont(word));
        }
        // Appends text to the line: merged into the previous span when the style is the
        // same (never into a formula), as AppendAsciiWord does for plain text.
        auto appendText = [&](std::string text, const std::string& url, uint8_t style) {
            if (!line.empty()) {
                AsciiSpan& last = line.back();
                if (last.math < 0 && last.style == style && last.url == url) {
                    last.text += text;
                    return;
                }
            }
            AsciiSpan& added = MathAppend(line);
            added.text = std::move(text);
            added.url = url;
            added.style = style;
        };
        auto flush = [&]() {
            if (!line.empty()) MathAppend(lines).swap(line);
            line.clear();
            lineWidth = 0.0;
        };

        bool groupOverflows = false;
        for (size_t index = 0; index < words.size(); index++) {
            const AsciiWord& word = words[index];
            if (word.math >= 0 && math.IsDisplay(word.math)) {
                flush();
                MathAppend(MathAppend(lines)).math = word.math;
                continue;
            }
            // The space before a word is shown, and measured, in the font of that word.
            const double space = (word.style & kStyleCode) ? codeSpaceWidth : spaceWidth;
            const bool joined = joins[index] != 0;
            bool needsSpace = !line.empty() && !joined;
            if (!joined) {
                double groupWidth = widths[index];
                for (size_t next = index + 1; next < words.size() && joins[next] != 0 &&
                    !(words[next].math >= 0 && math.IsDisplay(words[next].math)); next++) {
                    groupWidth += widths[next];
                }
                if (!line.empty() && lineWidth + groupWidth + (needsSpace ? space : 0.0) > width) {
                    flush();
                    needsSpace = false;
                }
                groupOverflows = lineWidth + groupWidth + (needsSpace ? space : 0.0) > width;
            } else if (groupOverflows && !line.empty() && lineWidth + widths[index] > width) {
                // Words that touch but are wider than a line together break where they touch.
                flush();
            }
            if (word.math >= 0) {
                if (needsSpace) {
                    appendText(" ", std::string(), 0);
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
                    double chWidth = AsciiCharWidth(ch, size, wordFont(word));
                    if (!part.empty() && partWidth + chWidth > width) {
                        appendText(std::move(part), word.url, word.style);
                        flush();
                        part.clear();
                        partWidth = 0.0;
                    }
                    part.push_back(ch);
                    partWidth += chWidth;
                }
                appendText(std::move(part), word.url, word.style);
                lineWidth = partWidth;
                continue;
            }
            std::string textRun = word.text;
            double wordWidth = widths[index];
            if (needsSpace) {
                textRun.insert(textRun.begin(), ' ');
                wordWidth += space;
            }
            appendText(std::move(textRun), word.url, word.style);
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
        return WrapAsciiMathWords(words, MarkWordJoins(spans, words.size()), width, size, bold);
    }

    // A table cell inside a table with formulas. A cell without a formula wraps
    // exactly as RenderTable wraps it.
    RAYOMD_MATH_COLD std::vector<std::vector<AsciiSpan>> WrapMathCell(const std::string& cell, double width, double size, bool bold) {
        if (cell.find(Internal::kMathTextOpen) != std::string::npos) return WrapMathText(cell, width, size, bold);
        std::vector<std::vector<AsciiSpan>> lines;
        for (WrappedAsciiLine& line : WrapAsciiText(cell, width, size, bold ? StandardTextFont::Bold : StandardTextFont::Regular)) {
            MathAppend(MathAppend(lines)).text = std::move(line.text);
        }
        return lines;
    }

    double MathLineWidth(const std::vector<AsciiSpan>& line, double size, bool bold) const {
        double width = 0.0;
        for (const AsciiSpan& span : line) {
            width += span.math >= 0 ? math.Width(span.math) : MathTextWidth(span.text, size, false, bold);
        }
        return width;
    }

    // Paints a line of plain text and formulas in one colour (headings, table cells).
    RAYOMD_MATH_COLD void PaintMathTextLine(const std::vector<AsciiSpan>& line, double x, double baseline, double size,
        const char* color, bool bold) {
        double cursor = x;
        for (const AsciiSpan& span : line) {
            if (span.math >= 0) {
                cursor += math.Note(span.math) != 0 ? PaintNote(span.math, cursor, baseline)
                    : math.Emit(span.math, content, cursor, baseline, color);
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
    // link, code and emphasis state of the span it came from.
    struct AsciiRun {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t widthUnits = 0;        // advance of the text in AFM units; a run never outgrows its line
        uint8_t style = 0;              // kStyle* bits
        std::string_view url;           // link target, in AsciiRuns::parsed
    };

    // A stretch of the parsed text in one link, code and emphasis state.
    struct AsciiSegment {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint8_t style = 0;
        std::string_view url;
    };

    // The wrapped lines of one paragraph, kept flat: the text of every run lies in one
    // buffer and a line is a range of runs. One object serves all paragraphs, so wrapping
    // builds no string and no vector per line.
    struct AsciiRuns {
        Internal::InlineRuns parsed;        // the paragraph as parsed; holds the link targets
        std::vector<AsciiSegment> segments;
        std::string text;
        std::vector<AsciiRun> runs;
        std::vector<uint32_t> lineEnds;     // one past the last run of each line

        std::string_view Text(const AsciiRun& run) const {
            return std::string_view(text.data() + run.begin, run.end - run.begin);
        }
        // AsciiTextWidth(Text(run), size, StyleFont(run.style)), without reading the text again.
        double Width(const AsciiRun& run, double size) const {
            return UnitsToPoints(run.widthUnits, size);
        }
    };
    AsciiRuns paragraphRuns;
    std::vector<AsciiRuns> tableCells;      // the cells with inline Markdown of a table row, by column

    // Colors of text runs: plain text, code and the background of code.
    struct RunColors { const char* text; const char* code; const char* codeFill; };
    static constexpr RunColors kBodyRunColors{ "0.08 0.08 0.08", "0.18 0.18 0.17", "0.94 0.94 0.92" };
    static constexpr RunColors kQuoteRunColors{ "0.18 0.22 0.25", "0.16 0.16 0.15", "0.88 0.89 0.88" };

    // Paints runs [begin, end) of `runs` as one line from x: code backgrounds `lh` high from
    // `top`, the text on `baseline`, strike lines and link rectangles.
    RAYOMD_SHARED void PaintAsciiRuns(const AsciiRuns& runs, size_t begin, size_t end, double x, double top,
        double baseline, double size, double lh, const RunColors& colors) {
        for (size_t index = begin; index < end; index++) {
            const AsciiRun& run = runs.runs[index];
            const double spanWidth = runs.Width(run, size);
            const bool code = (run.style & kStyleCode) != 0;
            if (code) CodeBackground(x, top, spanWidth, lh, runs.Text(run), size, colors.codeFill);
            const char* color = run.url.empty() ? (code ? colors.code : colors.text) : linkColor;
            Text(x, baseline, size, runs.Text(run), StyleFontName(run.style), color);
            if (run.style & kStyleStrike) StrikeThrough(x, baseline, spanWidth, size, runs.Text(run), run.style, color);
            AddLink(x, baseline, spanWidth, size, run.url);
            x += spanWidth;
        }
    }

    // AFM units of parsed text [begin, end) in the font of `segment`.
    static uint64_t AsciiUnits(const AsciiSegment& segment, const char* source, size_t begin, size_t end) {
        const uint16_t* advances = StyleAdvances(segment.style).byte;
        uint64_t units = 0;
        for (size_t at = begin; at < end; at++) units += advances[static_cast<unsigned char>(source[at])];
        return units;
    }

    // Appends `length` bytes of parsed text at `from`, `units` wide, in link and style state
    // `url` and `style`, to the line whose first run is `lineStart`, after a space `spaceUnits`
    // wide unless that is 0. The text joins the last run of the line when the state is the
    // same. All of it comes by value: a byte written through `cursor` might alias `out`.
    static RAYOMD_HOT_INLINE void AppendAsciiRunText(AsciiRuns& out, char* textBegin, char*& cursor, size_t lineStart,
        uint8_t style, std::string_view url, const char* from, size_t length, uint32_t units, uint32_t spaceUnits) {
        AsciiRun* run = out.runs.size() != lineStart ? &out.runs.back() : nullptr;
        if (!run || run->style != style || !SameText(run->url, url)) {
            out.runs.emplace_back();
            run = &out.runs.back();
            run->begin = static_cast<uint32_t>(cursor - textBegin);
            run->style = style;
            run->url = url;
        }
        if (spaceUnits != 0) *cursor++ = ' ';
        memcpy(cursor, from, length);
        cursor += length;
        run->widthUnits += units + spaceUnits;
        run->end = static_cast<uint32_t>(cursor - textBegin);
    }

    // A word wider than a whole line, cut by character where the line is full; the last part
    // stays open for the words after it. The line is empty when this is called.
    RAYOMD_COLD static void PlaceWideAsciiWord(AsciiRuns& out, char*& cursor, size_t& lineStart, uint64_t& lineUnits,
        size_t firstSegment, size_t wordStart, size_t wordEnd, uint64_t maxUnits) {
        char* const textBegin = &out.text[0];
        const char* const source = out.parsed.text.data();
        for (size_t piece = firstSegment, begin = wordStart; begin < wordEnd; piece++) {
            const AsciiSegment& current = out.segments[piece];
            const uint16_t* advances = StyleAdvances(current.style).byte;
            const size_t end = std::min<size_t>(wordEnd, current.end);
            size_t partStart = begin;
            uint32_t partUnits = 0;
            for (size_t ch = begin; ch < end; ch++) {
                const unsigned charUnits = advances[static_cast<unsigned char>(source[ch])];
                if (lineUnits + partUnits + charUnits > maxUnits && (ch > partStart || out.runs.size() != lineStart)) {
                    if (ch > partStart) {
                        AppendAsciiRunText(out, textBegin, cursor, lineStart, current.style, current.url, source + partStart,
                            ch - partStart, partUnits, 0);
                    }
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineUnits = 0;
                    partStart = ch;
                    partUnits = 0;
                }
                partUnits += charUnits;
            }
            if (end > partStart) {
                AppendAsciiRunText(out, textBegin, cursor, lineStart, current.style, current.url, source + partStart,
                    end - partStart, partUnits, 0);
                lineUnits += partUnits;
            }
            begin = end;
        }
    }

    // Wraps `text` into `out` with the line breaks and span boundaries of WrapAsciiLinks. A word
    // is everything between two white spaces of the source, so it may cross link, code and
    // emphasis boundaries ("[link](u).", "`code`s"): it gets no space and no line break inside,
    // and is cut by character only when it is wider than a whole line. Returns false for text
    // with explicit line breaks or possible formulas, which take the general path.
    bool WrapAsciiRuns(const std::string& text, double width, double size, AsciiRuns& out) {
        if (text.find('\n') != std::string::npos || text.find('$') != std::string::npos ||
            text.find("\\(") != std::string::npos || (footnotes != nullptr && text.find("[^") != std::string::npos)) {
            return false;
        }
        return WrapAsciiInline(text, width, size, 0, out);
    }

    // WrapAsciiRuns for one line of inline Markdown without formulas, such as a table cell;
    // every run gets the style bits `baseStyle` too (a bold header). Returns false only for
    // text too long for the run offsets.
    bool WrapAsciiInline(std::string_view text, double width, double size, uint8_t baseStyle, AsciiRuns& out) {
        if (text.size() >= 0x40000000u) return false;
        Internal::ParseInlineRuns(text, out.parsed);
        out.segments.clear();
        out.runs.clear();
        out.lineEnds.clear();
        const std::vector<Internal::InlineRun>& parsed = out.parsed.runs;
        for (size_t first = 0; first < parsed.size();) {
            const uint8_t style = static_cast<uint8_t>(SpanStyle(parsed[first]) | baseStyle);
            const std::string_view url = out.parsed.Url(parsed[first]);
            size_t last = first;
            while (last + 1 < parsed.size() && (SpanStyle(parsed[last + 1]) | baseStyle) == style &&
                out.parsed.Url(parsed[last + 1]) == url) {
                last++;
            }
            out.segments.push_back({ static_cast<uint32_t>(parsed[first].begin), static_cast<uint32_t>(parsed[last].end),
                style, url });
            first = last + 1;
        }
        // Every word is written once, with at most one space in front of it.
        out.text.resize(out.parsed.text.size() * 2 + 1);
        char* const textBegin = &out.text[0];
        char* cursor = textBegin;
        const char* const source = out.parsed.text.data();
        size_t lineStart = 0;       // index of the first run of the line being filled
        uint64_t lineUnits = 0;
        const uint64_t maxUnits = MaxUnits(width, size);
        const StandardWordAdvances& regular = WordAdvances(StandardTextFont::Regular);

        const size_t segmentCount = out.segments.size();
        const size_t total = segmentCount != 0 ? out.segments.back().end : 0;
        size_t at = 0;
        for (size_t segment = 0; segment < segmentCount; segment++) {
            const uint8_t style = out.segments[segment].style;
            const std::string_view url = out.segments[segment].url;
            const size_t segmentEnd = out.segments[segment].end;
            if (style & kStyleBreak) {
                // A <br> is one space: the line ends there, and each one more after it leaves an
                // empty line.
                for (; at < segmentEnd; at++) {
                    if (out.runs.size() == lineStart && out.lineEnds.empty()) continue;
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineUnits = 0;
                }
                continue;
            }
            const StandardWordAdvances& font = StyleAdvances(style);
            const uint16_t* const advances = font.byte;
            const uint32_t segmentSpace = font.space;
            while (at < segmentEnd) {
                if (regular.byte[static_cast<unsigned char>(source[at])] == 0) {
                    at++;
                    continue;
                }
                const size_t wordStart = at;
                uint64_t wordUnits = 0;
                for (; at < segmentEnd; at++) {
                    const unsigned charUnits = advances[static_cast<unsigned char>(source[at])];
                    if (charUnits == 0) break;
                    wordUnits += charUnits;
                }
                // A word that reaches the end of its segment goes on in the segments after it
                // unless white space follows: "[link](u)." and "`code`s" are one word each.
                size_t lastSegment = segment;
                if (at == segmentEnd && at < total && regular.byte[static_cast<unsigned char>(source[at])] != 0) {
                    do {
                        const AsciiSegment& next = out.segments[++lastSegment];
                        const uint16_t* const nextAdvances = StyleAdvances(next.style).byte;
                        for (; at < next.end; at++) {
                            const unsigned charUnits = nextAdvances[static_cast<unsigned char>(source[at])];
                            if (charUnits == 0) break;
                            wordUnits += charUnits;
                        }
                    } while (at == out.segments[lastSegment].end && at < total &&
                        regular.byte[static_cast<unsigned char>(source[at])] != 0);
                }
                const size_t wordEnd = at;

                uint32_t spaceUnits = out.runs.size() != lineStart ? segmentSpace : 0;
                if (spaceUnits != 0 && lineUnits + spaceUnits + wordUnits > maxUnits) {
                    out.lineEnds.push_back(static_cast<uint32_t>(out.runs.size()));
                    lineStart = out.runs.size();
                    lineUnits = 0;
                    spaceUnits = 0;
                }
                if (wordUnits > maxUnits) {
                    PlaceWideAsciiWord(out, cursor, lineStart, lineUnits, segment, wordStart, wordEnd, maxUnits);
                } else {
                    lineUnits += spaceUnits + wordUnits;
                    if (lastSegment == segment) {
                        AppendAsciiRunText(out, textBegin, cursor, lineStart, style, url, source + wordStart,
                            wordEnd - wordStart, static_cast<uint32_t>(wordUnits), spaceUnits);
                    } else {
                        // In one piece per segment the word touches.
                        for (size_t piece = segment, begin = wordStart; begin < wordEnd; piece++) {
                            const AsciiSegment& part = out.segments[piece];
                            const size_t end = std::min<size_t>(wordEnd, part.end);
                            if (end == begin) continue;
                            AppendAsciiRunText(out, textBegin, cursor, lineStart, part.style, part.url, source + begin,
                                end - begin, static_cast<uint32_t>(AsciiUnits(part, source, begin, end)), spaceUnits);
                            spaceUnits = 0;
                            begin = end;
                        }
                    }
                }
                if (lastSegment != segment) {
                    segment = lastSegment - 1;  // go on in the segment that holds the end of the word
                    break;
                }
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
            return WrapAsciiMathWords(mathWords, MarkWordJoins(spans, mathWords.size()), width, size, false);
        }
        std::vector<std::vector<AsciiSpan>> lines;
        lines.reserve(std::max<size_t>(1, text.size() / 72));
        std::vector<AsciiSpan> line;
        double lineWidth = 0.0;
        auto newLine = [&]() {
            lines.push_back(std::move(line));
            line.clear();
            lineWidth = 0.0;
        };
        // Width of what continues the word that ends span `index` without a space: the first
        // word of each following span, for as long as the span before it ends inside a word.
        auto gluedWidth = [&](size_t index) {
            double glued = 0.0;
            for (size_t next = index + 1; next < spans.size(); next++) {
                const std::string& following = spans[next].text;
                size_t end = 0;
                while (end < following.size() && !IsSpace(following[end])) end++;
                glued += AsciiTextWidth(std::string_view(following.data(), end), size, StyleFont(spans[next].style));
                if (end < following.size()) break;
            }
            return glued;
        };

        // The words are views into the spans: nothing is copied until a word lands in a line.
        // A word that starts a span where the span before ends inside a word continues that
        // word ("[link](u).", "`code`s"): no space and no line break between them.
        bool endsInWord = false;
        bool groupOverflows = false;
        for (size_t index = 0; index < spans.size(); index++) {
            const AsciiSpan& span = spans[index];
            const std::string& source = span.text;
            const StandardTextFont font = StyleFont(span.style);
            for (size_t at = 0; at < source.size();) {
                const bool glued = at == 0 && endsInWord && !IsSpace(source[0]);
                while (at < source.size() && IsSpace(source[at])) at++;
                size_t end = at;
                while (end < source.size() && !IsSpace(source[end])) end++;
                if (end == at) break;
                std::string_view word(source.data() + at, end - at);
                at = end;

                double wordWidth = AsciiTextWidth(word, size, font);
                const double spaceWidth = UnitsToPoints(WordAdvances(font).space, size);
                bool needsSpace = false;
                if (glued) {
                    // Only words glued into something wider than a line break where they touch.
                    if (groupOverflows && !line.empty() && lineWidth + wordWidth > width) newLine();
                } else {
                    const double groupWidth = end == source.size() ? wordWidth + gluedWidth(index) : wordWidth;
                    needsSpace = !line.empty();
                    double addWidth = groupWidth + (needsSpace ? spaceWidth : 0.0);
                    if (!line.empty() && lineWidth + addWidth > width) {
                        newLine();
                        needsSpace = false;
                    }
                    groupOverflows = lineWidth + groupWidth + (needsSpace ? spaceWidth : 0.0) > width;
                }

                if (wordWidth > width) {
                    // Wider than a whole line: cut by character; the last part stays open.
                    size_t partStart = 0;
                    double partWidth = 0.0;
                    for (size_t ch = 0; ch < word.size(); ch++) {
                        const double chWidth = AsciiCharWidth(word[ch], size, font);
                        if (lineWidth + partWidth + chWidth > width && (ch > partStart || !line.empty())) {
                            if (ch > partStart) AppendAsciiWord(line, word.substr(partStart, ch - partStart), false, span.url, span.style);
                            newLine();
                            partStart = ch;
                            partWidth = 0.0;
                        }
                        partWidth += chWidth;
                    }
                    AppendAsciiWord(line, word.substr(partStart), false, span.url, span.style);
                    lineWidth += partWidth;
                    continue;
                }

                if (needsSpace) wordWidth += spaceWidth;
                AppendAsciiWord(line, word, needsSpace, span.url, span.style);
                lineWidth += wordWidth;
            }
            if (!source.empty()) endsInWord = !IsSpace(source.back());
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

    // An image that cannot be shown: its alt text, or else its source, as a paragraph. The
    // text of a linked image is a link.
    void RenderImageFallback(const Block& block) {
        const Internal::ImageSource& image = *block.image;
        std::string fallback = block.text.empty() ? image.src : block.text;
        if (fallback.empty()) fallback = "image";
        if (image.link.empty()) {
            RenderParagraph(fallback);
            return;
        }
        const double lh = bodySize * 1.35;
        for (const WrappedAsciiLine& line : WrapAsciiText(fallback, pageW - margin * 2.0, bodySize, StandardTextFont::Regular)) {
            Ensure(lh);
            Text(margin, y - bodySize, bodySize, line.text, "F1", linkColor);
            AddLink(margin, y - bodySize, line.width, bodySize, image.link);
            y -= lh;
        }
        y -= 5.0;
    }

    void RenderImage(const Block& block) {
        if (!images) {
            RenderImageFallback(block);
            return;
        }

        int index = -1;
        // Paths and URLs are UTF-8; transcoded Latin text goes back first.
        std::string utf8Source;
        const std::string& source = block.image->src;
        if (winAnsiText && !IsAllAscii(source)) utf8Source = RayoMd::Text::WinAnsiToUtf8(source);
        if (!images->Resolve(utf8Source.empty() ? source : utf8Source, block.text, index)) {
            RenderImageFallback(block);
            return;
        }

        const PdfImage& image = images->Get(index);
        double maxW = pageW - margin * 2.0;
        double maxH = pageH - margin * 2.0;
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
        const std::string& link = block.image->link;
        if (!link.empty() && !pageLinks.empty()) pageLinks.back().push_back({ x, y - h, x + w, y, link });
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

    // `keep` is the room under its first line that the heading needs on its page (HeadingKeep).
    void RenderHeading(const Block& block, double keep) {
        int level = std::max(1, std::min(6, block.level));
        double size = kHeadingSizes[level];
        if (y < pageH - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;
        Ensure(size * 1.35 + keep);
        if (block.hasMath) {
            RenderMathTextLines(block.text, level, margin, pageW - margin * 2.0, size, headingColor, true, false);
            y -= level <= 2 ? 8.0 : 5.0;
            return;
        }
        MarkHeading(level, block.text, size * 1.35);
        ForEachWrappedAsciiLine(block.text, pageW - margin * 2.0, size, StandardTextFont::Bold, [&](std::string_view line, double) {
            DrawTextLine(margin, size, line, "F2", headingColor);
        });
        y -= level <= 2 ? 8.0 : 5.0;
    }

    // Heading lines that contain formulas: plain text and formulas in one colour,
    // each line as tall as its tallest formula needs. `quote` adds the quote strip. The
    // heading, at `level`, is noted once its first line has found its page.
    RAYOMD_MATH_COLD void RenderMathTextLines(const std::string& text, int level, double x, double width, double size,
        const char* color, bool bold, bool quote) {
        math.Clear();
        double lh = size * 1.35;
        bool marked = false;
        for (const auto& line : WrapMathText(text, width, size, bold)) {
            MathLineExtent extent = MeasureMathLine(math, line, size, lh);
            double extra = extent.above + extent.below;
            if (quote) {
                Ensure(lh + 2.0 + extra);
                QuoteStrip(lh + 3.0 + extra);
            } else {
                Ensure(lh + extra);
            }
            if (!marked) {
                MarkHeading(level, text, 0.0);
                marked = true;
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
                QuoteStrip(lineHeight + 3.0 + extra);
            } else {
                Ensure(lineHeight + extra);
            }
            y -= extent.above;
            double cursor = x;
            if (IsDisplayMathLine(math, line)) {
                cursor = x + std::max(0.0, (width - math.Width(line[0].math)) * 0.5);
            }
            double baseline = y - bodySize;
            for (const AsciiSpan& span : line) {
                if (span.math >= 0) {
                    cursor += math.Note(span.math) != 0 ? PaintNote(span.math, cursor, baseline)
                        : math.Emit(span.math, content, cursor, baseline, textColor);
                    continue;
                }
                const bool code = (span.style & kStyleCode) != 0;
                double spanWidth = MathTextWidth(span.text, bodySize, code, (span.style & kStyleBold) != 0);
                if (code) CodeBackground(cursor, y, spanWidth, lineHeight, span.text, bodySize, codeFill);
                const char* color = span.url.empty() ? (code ? codeColor : textColor) : linkColor;
                Text(cursor, baseline, bodySize, span.text, StyleFontName(span.style), color);
                if (span.style & kStyleStrike) StrikeThrough(cursor, baseline, spanWidth, bodySize, span.text, span.style, color);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight + extent.below;
        }
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, pageW - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        if (Internal::NeedsInlineParse(text, RayoMd::Text::kByteLineFeed)) {
            if (math.Active()) math.Clear();
            if (WrapAsciiRuns(text, width, bodySize, paragraphRuns)) {
                const double lh = bodySize * 1.35;
                size_t index = 0;
                for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                    Ensure(lh);
                    PaintAsciiRuns(paragraphRuns, index, lineEnd, x, y, y - bodySize, bodySize, lh, kBodyRunColors);
                    index = lineEnd;
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
                    const bool code = (span.style & kStyleCode) != 0;
                    double spanWidth = AsciiTextWidth(span.text, bodySize, StyleFont(span.style));
                    if (code) CodeBackground(cursor, y, spanWidth, lh, span.text, bodySize, "0.94 0.94 0.92");
                    const char* color = span.url.empty() ? (code ? "0.18 0.18 0.17" : "0.08 0.08 0.08") : linkColor;
                    Text(cursor, baseline, bodySize, span.text, StyleFontName(span.style), color);
                    if (span.style & kStyleStrike) StrikeThrough(cursor, baseline, spanWidth, bodySize, span.text, span.style, color);
                    AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                    cursor += spanWidth;
                }
                y -= lh;
            }
            y -= 5.0;
            return;
        }

        ForEachWrappedAsciiLine(text, width, bodySize, StandardTextFont::Regular, [&](std::string_view line, double) {
            DrawTextLine(x, bodySize, line);
        });
        y -= 5.0;
    }

    void RenderQuote(const Block& block) {
        if (block.alert != 0) {
            RenderAlert(block);
            return;
        }
        if (block.children.empty()) {
            RenderQuote(block.text);
            return;
        }
        RenderQuoteChildren(block.children);
    }

    // A GitHub alert: a quote whose bar takes the alert's colour, under the alert's bold title
    // in that colour, which keeps the first line after it on its page.
    RAYOMD_COLD void RenderAlert(const Block& block) {
        quoteBar = kQuoteBars[block.alert];
        Ensure(lineHeight * 2.0 + 2.0);
        QuoteStrip(lineHeight + 3.0);
        Text(margin + 14.0, y - bodySize, bodySize, kAlertTitles[block.alert], "F2", quoteBar);
        y -= lineHeight;
        if (block.children.empty()) RenderQuote(block.text);
        else RenderQuoteChildren(block.children);
        quoteBar = plainQuoteBar;
    }

    void RenderQuoteChildren(const std::vector<Block>& children) {
        for (const Block& child : children) {
            switch (child.type) {
            case BlockType::Paragraph: RenderQuote(child.text); break;
            case BlockType::Heading: RenderQuoteHeading(child); break;
            case BlockType::Bullet:
                RenderQuote((child.task == 0 ? "- " : child.task == 2 ? "- [x] " : "- [ ] ") + child.text);
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
                item += child.task == 0 ? ". " : child.task == 2 ? ". [x] " : ". [ ] ";
                item += child.text;
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
                // A quote in an alert has a plain quote's bar, as on GitHub.
                double savedMargin = margin;
                const char* savedBar = quoteBar;
                margin += 10.0;
                quoteBar = plainQuoteBar;
                RenderQuote(child);
                margin = savedMargin;
                quoteBar = savedBar;
                break;
            }
            case BlockType::MathBlock: RenderDisplayMath(child.text, true); break;
            case BlockType::Code:
            case BlockType::Table:
            case BlockType::Rule:
            case BlockType::Image: {
                double savedMargin = margin;
                margin += 14.0;
                if (child.type == BlockType::Code) RenderCode(child);
                else if (child.type == BlockType::Table && child.hasMath) RenderMathTable(*this, child);
                else if (child.type == BlockType::Table) RenderTable(child.rows, child.aligns);
                else if (child.type == BlockType::Rule) RenderRule();
                else RenderImage(child);
                margin = savedMargin;
                break;
            }
            case BlockType::PageBreak: RenderPageBreak(); break;
            case BlockType::Contents: break;
            }
        }
    }

    void RenderQuoteHeading(const Block& block) {
        static const double sizes[] = { 0, 18, 16, 14, 13, 12, 11.5 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        double height = size * 1.35;
        double x = margin + 14.0;
        double width = pageW - margin * 2.0 - 22.0;
        if (block.hasMath) {
            RenderMathTextLines(block.text, level, x, width, size, "0.10 0.15 0.18", true, true);
            y -= 7.0;
            return;
        }
        MarkHeading(level, block.text, height + 2.0);
        for (const WrappedAsciiLine& line : WrapAsciiText(block.text, width, size, StandardTextFont::Bold)) {
            Ensure(height + 2.0);
            QuoteStrip(height + 3.0);
            Text(x, y - size, size, line.text, "F2", "0.10 0.15 0.18");
            y -= height;
        }
        y -= 7.0;
    }
    void RenderQuote(const std::string& text) {
        double x = margin + 14.0;
        double width = pageW - margin * 2.0 - 22.0;
        if (math.Active()) math.Clear();
        if (WrapAsciiRuns(text, width, bodySize, paragraphRuns)) {
            size_t index = 0;
            for (const uint32_t lineEnd : paragraphRuns.lineEnds) {
                Ensure(lineHeight + 2.0);
                QuoteStrip(lineHeight + 3.0);
                PaintAsciiRuns(paragraphRuns, index, lineEnd, x, y, y - bodySize, bodySize, lineHeight, kQuoteRunColors);
                index = lineEnd;
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
            QuoteStrip(lineHeight + 3.0);
            double cursor = x;
            double baseline = y - bodySize;
            for (const AsciiSpan& span : line) {
                const bool code = (span.style & kStyleCode) != 0;
                double spanWidth = AsciiTextWidth(span.text, bodySize, StyleFont(span.style));
                if (code) CodeBackground(cursor, y, spanWidth, lineHeight, span.text, bodySize, "0.88 0.89 0.88");
                const char* color = span.url.empty() ? (code ? "0.16 0.16 0.15" : "0.18 0.22 0.25") : linkColor;
                Text(cursor, baseline, bodySize, span.text, StyleFontName(span.style), color);
                if (span.style & kStyleStrike) StrikeThrough(cursor, baseline, spanWidth, bodySize, span.text, span.style, color);
                AddLink(cursor, baseline, spanWidth, bodySize, span.url);
                cursor += spanWidth;
            }
            y -= lineHeight;
        }
        y -= 7.0;
    }

    void RenderCode(const Block& block) {
        if (block.codeLanguage != 0 && highlightCode) {
            RenderHighlightedCode(block.text, block.codeLanguage);
            return;
        }
        std::vector<std::string> raw = SplitLines(block.text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = pageW - margin * 2.0 - 16.0;
        bool first = true;
        for (const auto& rawLine : raw) {
            for (const auto& line : WrapAsciiLiteral(rawLine, width, size)) {
                CodeTile(lh, first, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "F3", "0.12 0.12 0.12", true);
            }
        }
        y -= 8.0;
    }

    // Makes room for a line of code `lh` high and draws its tile. The first line of a block on a
    // page has one that reaches 3 points above it, the others one that starts at their top, below
    // the descenders of the line before: drawn over them, it hid underscores. Each tile reaches 2
    // points into the next, so that the block is one tint. Out of line: written into RenderCode,
    // it moved GCC's inlining and outlined "- " + text in RenderBullet (+0.3 % on baseline.md).
    RAYOMD_SHARED void CodeTile(double lh, bool& first, const char* color) {
        const size_t pages = pageStarts.size();
        Ensure(lh + 4.0);
        if (pageStarts.size() != pages) first = true;
        const double pad = first ? 3.0 : 0.0;
        Rect(margin, y + pad, pageW - margin * 2.0, lh + 2.0 + pad, color);
        first = false;
    }

    // A fenced block in a language the lexer knows (Block::codeLanguage): RenderCode's lines and
    // tiles, each run of characters in the colour of its token class; a diff's added and removed
    // lines on tinted tiles. Out of line, but not cold: it runs for every character of the code,
    // and a cold function is optimized for size.
    RAYOMD_SHARED void RenderHighlightedCode(const std::string& text, uint8_t language) {
        codeClasses.resize(text.size());
        Internal::ClassifyCode(language, text, codeClasses.data());
        const double size = 9.5;
        const double lh = size * 1.35;
        const double x = margin + 8.0;
        const double width = pageW - margin * 2.0 - 16.0;
        // WrapAsciiLiteral's lines: as many Courier characters as fit, and at least one.
        const size_t perLine = (size_t)std::max<uint64_t>(1, MaxUnits(width, size) / WordAdvances(StandardTextFont::Mono).space);
        bool first = true;
        for (size_t start = 0;;) {
            size_t end = text.find('\n', start);
            const bool last = end == std::string::npos;
            if (last) end = text.size();
            // The bytes WrapAsciiLiteral keeps: tabs as spaces, and no control or non-ASCII bytes.
            // A line of printable ASCII, the usual one, is read where it is.
            std::string_view line(text.data() + start, end - start);
            const uint8_t* lineClasses = codeClasses.data() + start;
            size_t printable = 0;
            while (printable < line.size() && (unsigned char)line[printable] - 32u < 95u) printable++;
            if (printable < line.size()) {
                codeLine.resize(line.size());
                codeLineClasses.resize(line.size());
                size_t kept = 0;
                for (size_t at = 0; at < line.size(); at++) {
                    const unsigned char ch = (unsigned char)line[at];
                    if (ch != '\t' && (ch < 32 || ch >= 127)) continue;
                    codeLine[kept] = ch == '\t' ? ' ' : (char)ch;
                    codeLineClasses[kept++] = lineClasses[at];
                }
                line = std::string_view(codeLine.data(), kept);
                lineClasses = codeLineClasses.data();
            }
            const char* tile = Internal::CodeLineTile(end > start ? codeClasses[start] : (uint8_t)Internal::kCodePlain);
            size_t piece = 0;
            do {
                const size_t count = std::min(perLine, line.size() - piece);
                CodeTile(lh, first, tile != nullptr ? tile : "0.95 0.95 0.93");
                PaintCodeRuns(x, y - size, size, line.substr(piece, count), lineClasses + piece);
                y -= lh;
                piece += count;
            } while (piece < line.size());
            if (last) break;
            start = end + 1;
        }
        y -= 8.0;
    }

    // A line of highlighted code at (x, baseline) in Courier: as Text when it is one colour,
    // otherwise one text object with each run after its colour.
    RAYOMD_SHARED void PaintCodeRuns(double x, double baseline, double size, std::string_view text, const uint8_t* classes) {
        if (text.empty()) return;
        SplitCodeRuns(text.data(), text.size(), classes, codeRuns);
        if (codeRuns.size() == 1) {
            Text(x, baseline, size, text, "F3", Internal::CodeColor(codeRuns[0].color));
            return;
        }
        TailWriter out(content, 48 + kOperandBytes * 3 + text.size() * 2 + codeRuns.size() * 32);
        size_t begin = 0;
        for (const CodeRun& run : codeRuns) {
            const char* color = Internal::CodeColor(run.color);
            if (begin == 0) {
                out.Lit("q ");
                out.Bytes(color, strlen(color));
                out.Lit(" rg BT /F3 ");
                out.Fixed(size);
                out.Lit(" Tf 1 0 0 1 ");
                out.Fixed(x);
                out.Lit(" ");
                out.Fixed(baseline);
                out.Lit(" Tm (");
            } else {
                out.Bytes(color, strlen(color));
                out.Lit(" rg (");
            }
            out.cursor = WriteEscapedLiteral(out.cursor, text.substr(begin, run.end - begin));
            out.Lit(") Tj ");
            begin = run.end;
        }
        out.Lit("ET Q\n");
    }

    // A book's title, or a part's, alone on a page and centred a little above its middle, as a
    // level-1 heading is drawn; with `mark` its bookmark (RenderBook).
    RAYOMD_COLD void RenderTitlePage(const std::string& text, bool mark) {
        RenderPageBreak();
        const double size = kHeadingSizes[1];
        const std::vector<WrappedAsciiLine> lines = WrapAsciiText(text, pageW - margin * 2.0, size, StandardTextFont::Bold);
        y = std::min(pageH - margin, pageH * 0.6 + (double)lines.size() * size * 1.35 * 0.5);
        if (mark) MarkHeading(1, text, 0.0);
        for (const WrappedAsciiLine& line : lines) DrawTextLine((pageW - line.width) * 0.5, size, line.text, "F2", headingColor);
    }

    // The table of contents (contents.h), once: a title, then each entry indented by its level and
    // linked to its heading, the top level in bold with room above it. The page numbers come later
    // (ContentsNumbers), so the entries' text ends 2.55 em before the right margin.
    RAYOMD_COLD void RenderContents() {
        if (contents == nullptr || contents->empty() || !contentsLines.empty()) return;
        const double titleSize = kHeadingSizes[1];
        if (y < pageH - margin - 4.0) y -= 12.0;
        Ensure(titleSize * 1.35 + lineHeight * 2.0);
        DrawTextLine(margin, titleSize, "Contents", "F2", headingColor);
        y -= 8.0;
        const double em = bodySize;
        const double lh = bodySize * 1.35;
        std::vector<WrappedAsciiLine> lines;
        contentsLines.resize(contents->size());
        for (size_t index = 0; index < contents->size(); index++) {
            const Internal::ContentsEntry& entry = (*contents)[index];
            const bool top = entry.level == 1;
            if (top && index > 0) y -= em * 0.5;
            const double x = margin + (entry.level - 1) * 1.5 * em;
            char url[24] = "#\x01";
            const std::string_view link(url, (size_t)(std::to_chars(url + 2, url + sizeof(url), index).ptr - url));
            ContentsLine& last = contentsLines[index];
            last = { index, pageStarts.size() - 1, y - bodySize, x };
            WrapAsciiText(entry.text, pageW - margin - x - 2.55 * em, bodySize,
                top ? StandardTextFont::Bold : StandardTextFont::Regular, lines);
            for (const WrappedAsciiLine& line : lines) {
                Ensure(lh);
                const double baseline = y - bodySize;
                Text(x, baseline, bodySize, line.text, top ? "F2" : "F1", "0.08 0.08 0.08");
                AddLink(x, baseline, line.width, bodySize, link);
                last = { index, pageStarts.size() - 1, baseline, x + line.width };
                y -= lh;
            }
        }
        y -= 5.0;
    }

    // Text at (x, baseline) in the body size and `fontName`, appended to `out`, a stream of its own;
    // `spacing` is added after each character (Tc).
    RAYOMD_COLD void AppendOverlayText(std::string& out, double x, double baseline, std::string_view text,
        const char* fontName, const char* color, double spacing) const {
        TailWriter w(out, 96 + kOperandBytes * 4 + text.size() * 2);
        w.Lit("q ");
        w.Bytes(color, strlen(color));
        w.Lit(" rg BT /");
        w.Bytes(fontName, strlen(fontName));
        w.Lit(" ");
        w.Fixed(bodySize);
        w.Lit(" Tf ");
        if (spacing > 0.0) {
            w.Fixed(spacing);
            w.Lit(" Tc ");
        }
        w.Lit("1 0 0 1 ");
        w.Fixed(x);
        w.Lit(" ");
        w.Fixed(baseline);
        w.Lit(" Tm (");
        w.cursor = WriteEscapedLiteral(w.cursor, text);
        w.Lit(") Tj ET Q\n");
    }

    void RenderMath(const std::string& text) {
        RenderDisplayMath(text, false);
    }

    // Display math: one unbreakable formula, centred in the available width.
    // `quoted` draws it inside the block-quote strip. A formula that is empty or
    // does not fit the page even at half size is shown as its source.
    RAYOMD_MATH_COLD void RenderDisplayMath(const std::string& tex, bool quoted) {
        double left = quoted ? margin + 14.0 : margin;
        double available = quoted ? pageW - margin * 2.0 - 22.0 : pageW - margin * 2.0;
        double padTop = quoted ? kQuoteMathPad : kDisplayMathAbove;
        double padBottom = quoted ? kQuoteMathPad : 0.0;
        double maxHeight = pageH - margin * 2.0 - padTop - padBottom - 3.0;
        MathFormula formula;
        std::string utf8;
        if (!LayoutMathToFit(MathSource(tex, utf8), bodySize, true, false, available, maxHeight, MathFallback(),
            formula)) {
            double savedMargin = margin;
            if (quoted) margin += 14.0;
            RenderMathSource(tex);
            margin = savedMargin;
            return;
        }
        double total = padTop + formula.Ascent() + formula.Descent() + padBottom;
        Ensure(total + 2.0);
        if (quoted) {
            QuoteStrip(total + 3.0);
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
            for (const auto& line : WrapAsciiLiteral(rawLine, pageW - margin * 2.0 - 24.0, size)) {
                size_t pageCount = pageStarts.size();
                Ensure(pitch + 8.0);
                if (pageStarts.size() != pageCount) first = true;
                // One tile per line, flush with its neighbours, so the tint never covers text.
                double pad = first ? 4.0 : 0.0;
                Rect(margin, y + pad, pageW - margin * 2.0, pitch + pad, "0.97 0.97 0.95");
                DrawTextLine(margin + 12.0, size, line, "F3", "0.10 0.10 0.10", true);
                first = false;
            }
        }
        Rect(margin, y, pageW - margin * 2.0, 4.0, "0.97 0.97 0.95");
        y -= 12.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;
        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = pageW - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        y -= 3.0;

        // A cell with inline Markdown is wrapped into runs, any other into plain lines.
        std::vector<std::vector<WrappedAsciiLine>> wrapped(columns);
        std::vector<unsigned char> styled(columns, 0);
        // Grows rarely (to the widest table); a new vector keeps resize code out of the binary.
        if (tableCells.size() < columns) tableCells = std::vector<AsciiRuns>(columns);
        const double cellWidth = std::max(16.0, colWidth - pad * 2.0);
        static const std::string emptyCell;
        // A table that goes on to a new page repeats its header row there, unless it and the row
        // that moved do not fit a page together: the loop draws row 0 again, then that row.
        double headerHeight = 0.0;  // the header row's height; 0 when it was cut into slices
        size_t resumeRow = 0;       // the row to draw after a repeated header row, or 0
        for (size_t r = 0; r < rows.size(); r++) {
            size_t maxLines = 1;
            for (size_t c = 0; c < columns; c++) {
                const std::string& cell = c < rows[r].size() ? rows[r][c] : emptyCell;
                styled[c] = Internal::NeedsInlineParse(cell) &&
                    WrapAsciiInline(cell, cellWidth, size, r == 0 ? kStyleBold : 0, tableCells[c]);
                if (!styled[c]) {
                    WrapAsciiText(cell, cellWidth, size, r == 0 ? StandardTextFont::Bold : StandardTextFont::Regular,
                        wrapped[c]);
                }
                maxLines = std::max(maxLines, styled[c] ? tableCells[c].lineEnds.size() : wrapped[c].size());
            }

            // A row taller than a page is drawn in slices, each with cell borders of its own.
            size_t first = 0;
            size_t count = maxLines;
            double rowHeight = maxLines * lh + pad * 2.0;
            if (rowHeight + 5.0 > pageH - margin * 2.0) {
                count = TallTableRowSlice(first, maxLines, lh, pad);
            } else if (r == 0) {
                // The header row keeps a line of the next row with it.
                headerHeight = rowHeight;
                Ensure(rowHeight + 5.0 + (rows.size() > 1 ? lh + pad * 2.0 : 0.0));
            } else if (y - rowHeight - 5.0 < margin) {
                NewPage();
                if (headerHeight > 0.0 && headerHeight + rowHeight + 5.0 <= pageH - margin * 2.0) {
                    resumeRow = r;
                    r = static_cast<size_t>(-1);
                    continue;
                }
            }
            for (;;) {
                rowHeight = count * lh + pad * 2.0;
                double top = y;
                if (r == 0) Rect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");
                for (size_t c = 0; c < columns; c++) {
                    double cellX = margin + c * colWidth;
                    Rect(cellX, top, colWidth, rowHeight, "0.72 0.72 0.72", true);
                    int align = c < aligns.size() ? aligns[c] : -1;
                    if (styled[c]) {
                        const AsciiRuns& cellRuns = tableCells[c];
                        const size_t end = std::min(cellRuns.lineEnds.size(), first + count);
                        for (size_t li = first; li < end; li++) {
                            const size_t runBegin = li == 0 ? 0 : cellRuns.lineEnds[li - 1];
                            const size_t runEnd = cellRuns.lineEnds[li];
                            double tx = cellX + pad;
                            if (align == 0 || align == 1) {
                                uint64_t units = 0;
                                for (size_t run = runBegin; run < runEnd; run++) units += cellRuns.runs[run].widthUnits;
                                const double lw = UnitsToPoints(units, size);
                                tx = align == 0 ? cellX + (colWidth - lw) * 0.5 : cellX + colWidth - pad - lw;
                            }
                            PaintAsciiRuns(cellRuns, runBegin, runEnd, tx, top - pad - (li - first) * lh,
                                top - pad - size - (li - first) * lh, size, lh, kBodyRunColors);
                        }
                        continue;
                    }
                    const size_t end = std::min(wrapped[c].size(), first + count);
                    for (size_t li = first; li < end; li++) {
                        const WrappedAsciiLine& line = wrapped[c][li];
                        double tx = cellX + pad;
                        double lw = line.width;
                        if (align == 0) tx = cellX + (colWidth - lw) * 0.5;
                        else if (align == 1) tx = cellX + colWidth - pad - lw;
                        Text(tx, top - pad - size - (li - first) * lh, size, line.text, r == 0 ? "F2" : "F1", "0.08 0.08 0.08");
                    }
                }
                y -= rowHeight;
                first += count;
                if (first == maxLines) break;
                count = TallTableRowSlice(first, maxLines, lh, pad);
            }
            if (resumeRow != 0) {
                r = resumeRow - 1;
                resumeRow = 0;
            }
        }
        y -= 9.0;
    }

    // How many lines of a table row taller than a page go on this page from line `first` on:
    // as many as fit, at least one. The rest of a row, or a row with no room left here, goes
    // on a new page. Drawing stays in RenderTable, so this rare path adds no caller to the
    // helpers the table loop inlines.
    RAYOMD_COLD size_t TallTableRowSlice(size_t first, size_t lineCount, double lh, double pad) {
        auto fitting = [&]() {
            const double room = y - margin - 5.0 - pad * 2.0;
            return room >= lh ? static_cast<size_t>(room / lh) : size_t(0);
        };
        if (first > 0) NewPage();
        size_t fit = fitting();
        if (fit == 0) {
            NewPage();
            fit = std::max<size_t>(1, fitting());
        }
        return std::min(fit, lineCount - first);
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = content;
        c += "q ";
        c += ruleColor;
        c += " RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, pageW - margin);
        c += " ";
        AppendF(c, y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

// The page number at the foot of a page: "N / M" centred in the bottom margin, small and grey,
// in Helvetica, which the page names `font`. A content stream of its own, after the page's,
// which was rendered before the page count was known.
RAYOMD_COLD static int AddPageNumber(PdfObjects& pdf, size_t page, size_t pages, double margin, double pageWidth,
    const char* font) {
    char label[48];
    char* end = std::to_chars(label, label + 20, page).ptr;
    memcpy(end, " / ", 3);
    end = std::to_chars(end + 3, end + 23, pages).ptr;
    const std::string_view text(label, (size_t)(end - label));
    constexpr double kSize = 9.0;
    std::string stream = "q 0.45 0.45 0.45 rg BT /";
    stream += font;
    stream += " 9 Tf 1 0 0 1 ";
    AppendF(stream, (pageWidth - Internal::StandardTextWidth(text, kSize, StandardTextFont::Regular)) * 0.5);
    stream += " ";
    AppendF(stream, margin * 0.5 - 3.0);
    stream += " Tm (";
    stream += text;
    stream += ") Tj ET Q";
    return pdf.AddStream("", stream);
}

// ---- A theme's header, footer and cover (laid out in theme.cpp) ------------------------------

// A theme's text in the standard fonts: Helvetica, /F1, or Helvetica-Bold, /F2, in
// WinAnsiEncoding; a character outside it is '?'.
class StandardThemeText final : public Internal::ThemeTextFont {
public:
    double Width(std::string_view utf8, double size, bool bold) const override {
        return Internal::StandardTextWidth(WinAnsi(utf8), size, bold ? StandardTextFont::Bold : StandardTextFont::Regular);
    }
    void Emit(std::string& out, std::string_view utf8, double x, double baseline, double size, bool bold) const override {
        const std::string text = WinAnsi(utf8);
        out += bold ? "BT /F2 " : "BT /F1 ";
        AppendF(out, size);
        out += " Tf 1 0 0 1 ";
        AppendF(out, x);
        out += " ";
        AppendF(out, baseline);
        out += " Tm (";
        const size_t at = out.size();
        out.resize(at + text.size() * 2);
        out.resize((size_t)(WriteEscapedLiteral(&out[at], text) - out.data()));
        out += ") Tj ET\n";
    }
    std::string_view Ellipsis() const override { return "\xE2\x80\xA6"; }

private:
    static std::string WinAnsi(std::string_view utf8) {
        std::string winAnsi;
        if (IsAllAscii(utf8)) return std::string(utf8);
        RayoMd::Text::TranscodeToWinAnsiLossy(utf8, winAnsi);
        return winAnsi;
    }
};

// A theme's text in the document's TrueType font, /F1, whose subset gets its glyphs; bold is
// drawn twice, a little apart, as the renderer does.
class UnicodeThemeText final : public Internal::ThemeTextFont {
public:
    UnicodeThemeText(const TtfFont& documentFont, UsedCidSet& usedCids) : font(documentFont), cids(usedCids) {}
    double Width(std::string_view utf8, double size, bool) const override { return TextWidth(font, Utf8ToWide(utf8), size); }
    void Emit(std::string& out, std::string_view utf8, double x, double baseline, double size, bool bold) const override {
        const std::wstring text = Utf8ToWide(utf8);
        out += "BT /F1 ";
        AppendF(out, size);
        out += " Tf 1 0 0 1 ";
        AppendF(out, x);
        out += " ";
        AppendF(out, baseline);
        out += " Tm ";
        const size_t at = out.size();
        out.resize(at + HexTextBytes(text));
        out.resize((size_t)(WriteHexText(&out[at], font, text, cids) - out.data()));
        const size_t hexBytes = out.size() - at;
        out += " Tj";
        if (bold) {
            out.reserve(out.size() + hexBytes + 64);   // the copy below reads `out` itself
            out += " 1 0 0 1 ";
            AppendF(out, x + size * 0.025);
            out += " ";
            AppendF(out, baseline);
            out += " Tm ";
            out.append(out.data() + at, hexBytes);
            out += " Tj";
        }
        out += " ET\n";
    }
    std::string_view Ellipsis() const override { return font.HasGlyph(0x2026) ? "\xE2\x80\xA6" : "..."; }

private:
    const TtfFont& font;
    UsedCidSet& cids;
};

// The theme's logo registered with the document's images, when the theme has one.
RAYOMD_COLD static Internal::ThemeLogoImage ThemeLogo(const PdfTheme& theme, ImageRegistry& images) {
    Internal::ThemeLogoImage logo;
    if (theme.logoPath.empty() || !images.ResolveThemeFile(theme.logoPath, logo.index)) return logo;
    const PdfImage& image = images.Get(logo.index);
    if (image.width > 0 && image.height > 0) logo.aspect = (double)image.width / (double)image.height;
    return logo;
}

// The theme's header and footer streams, one per page, and its cover page's stream. {title} is
// the front matter's title, else the first heading's text (WinAnsi with `winAnsi`).
RAYOMD_COLD static void LayOutTheme(const PdfOptions& options, const Internal::ThemeTextFont& font,
    const DocumentMetadata& metadata, const std::vector<HeadingMark>& headings, bool winAnsi, size_t pages,
    const Internal::ThemePage& page, const Internal::ThemeLogoImage& logo, const ThemePalette& palette,
    std::vector<std::string>& bands, std::string& cover) {
    const std::string title = !metadata.title.empty() || headings.empty() ? metadata.title
        : winAnsi ? RayoMd::Text::WinAnsiToUtf8(headings.front().text) : std::string(headings.front().text);
    const Internal::ThemeDocument document{ title, metadata.author, metadata.subject, metadata.date };
    // PDF/A's page numbers too: a footer in the document's font, as Helvetica is not embedded.
    if (Internal::HasThemeBands(options.theme) || (options.pdfa && options.pageNumbers)) {
        bands = Internal::ThemeBandStreams(options.theme, options.pageNumbers, document, pages, page, logo, font);
    }
    if (options.theme.cover) cover = Internal::ThemeCoverStream(document, page, logo, font, palette.heading, palette.rule);
}

// The cover as the first page: the fonts it names, `fonts`, and every image, as the other pages
// have them.
RAYOMD_COLD static int AddCoverPage(PdfObjects& pdf, int pagesId, double pageW, double pageH, const std::string& fonts,
    const std::vector<int>& imageObjectIds, const std::string& stream) {
    const int contentId = pdf.AddStream("", stream);
    std::string page = "<< /Type /Page /Parent ";
    AppendInt(page, pagesId);
    page += " 0 R /MediaBox [0 0 ";
    AppendF(page, pageW);
    page += " ";
    AppendF(page, pageH);
    page += "] /Resources << /Font << ";
    page += fonts;
    page += " >>";
    AppendXObjectResources(page, imageObjectIds);
    page += " >> /Contents ";
    AppendInt(page, contentId);
    page += " 0 R >>";
    return pdf.Add(std::move(page));
}

// With a cover, a viewer numbers the pages as the footer does: "Cover", then 1, 2 and on.
constexpr const char* kCoverPageLabels = " /PageLabels << /Nums [0 << /P (Cover) >> 1 << /S /D >>] >>";

// The page objects in /Kids, the cover first when there is one, and their /Count.
static std::string PagesDictionary(const std::vector<int>& pageIds, int coverId) {
    std::string pages;
    pages.reserve(48 + pageIds.size() * 8);
    pages += "<< /Type /Pages /Kids [";
    if (coverId != 0) {
        AppendInt(pages, coverId);
        pages += " 0 R ";
    }
    for (int id : pageIds) {
        AppendInt(pages, id);
        pages += " 0 R ";
    }
    pages += "] /Count ";
    AppendSize(pages, pageIds.size() + (coverId != 0));
    pages += " >>";
    return pages;
}

// A page's /Contents: its own stream, and those of the page number, or the theme's header and
// footer, and of the table of contents' page numbers, when there are.
static void AppendContents(std::string& page, int contentId, int numberId, int contentsId) {
    page += " /Contents ";
    if (numberId == 0 && contentsId == 0) {
        AppendInt(page, contentId);
        page += " 0 R";
        return;
    }
    page += "[";
    AppendInt(page, contentId);
    page += " 0 R";
    if (numberId != 0) {
        page += " ";
        AppendInt(page, numberId);
        page += " 0 R";
    }
    if (contentsId != 0) {
        page += " ";
        AppendInt(page, contentsId);
        page += " 0 R";
    }
    page += "]";
}

// The heading of each entry of the table of contents, an index into the renderer's `headings`, or
// npos for one not drawn: found by the address of its text, which a heading mark keeps. Both are in
// the order of the document, the marks with those of quotes, lists and notes between.
RAYOMD_COLD static std::vector<size_t> ContentsHeadings(const std::vector<Internal::ContentsEntry>& entries,
    const std::vector<HeadingMark>& headings) {
    std::vector<size_t> found(entries.size(), std::string::npos);
    size_t mark = 0;
    for (size_t index = 0; index < entries.size(); index++) {
        size_t at = mark;
        while (at < headings.size() && headings[at].text.data() != entries[index].heading) at++;
        if (at == headings.size()) continue;
        found[index] = at;
        mark = at + 1;
    }
    return found;
}

// The table of contents' page numbers, one stream per page (ContentsNumbers), once the pages of
// its headings are known; `targets` gets the heading of each entry, for the links to them.
template <typename RendererType>
RAYOMD_COLD static std::vector<std::string> ContentsOverlays(RendererType& renderer,
    const std::vector<Internal::ContentsEntry>& entries, std::vector<size_t>& targets) {
    std::vector<std::string> overlays;
    if (entries.empty()) return overlays;
    const std::vector<HeadingMark>& headings = renderer.Headings();
    targets = ContentsHeadings(entries, headings);
    std::vector<size_t> pages(targets.size(), std::string::npos);
    for (size_t index = 0; index < targets.size(); index++) {
        if (targets[index] != std::string::npos) pages[index] = headings[targets[index]].page;
    }
    renderer.ContentsNumbers(pages, overlays);
    return overlays;
}

// A book's files parsed (Internal::ParseBookFiles), and the entries of its table of contents.
RAYOMD_COLD static std::vector<Internal::ContentsEntry> ParseBook(const BookSource& source, const PdfOptions& options,
    BookParse& parse) {
    Internal::ParseBookFiles(source.book->chapters, source.texts, source.titles, options.toc, options.tocDepth, parse.files);
    parse.contentsFirst = options.toc;
    parse.noteMarks.resize(source.book->chapters.size());
    return std::move(parse.files.contents);
}

// A book's metadata: the front matter of its first file, with the book's title.
RAYOMD_COLD static DocumentMetadata BookMetadata(const Book& book) {
    DocumentMetadata metadata;
    for (const BookChapter& chapter : book.chapters) {
        if (chapter.part) continue;
        metadata = FrontMatterMetadata(chapter.markdown);
        break;
    }
    if (!book.title.empty()) metadata.title = book.title;
    return metadata;
}

// The link annotations of a book's pages (AddLinkAnnotationObjects, BookTargets).
RAYOMD_COLD static std::vector<std::vector<int>> AddBookLinkAnnotations(PdfObjects& pdf,
    const std::vector<std::vector<LinkRect>>& links, bool winAnsi, const std::vector<HeadingMark>& headings,
    const Book& book, const BookParse& parse, double pageTop, const std::vector<size_t>* contents,
    std::vector<InternalLink>& internalLinks, bool print) {
    BookTargets targets(headings, book, parse, winAnsi, pageTop, contents);
    return AddLinkAnnotationObjects(pdf, links, winAnsi, targets, internalLinks, print);
}

// Page content is rendered straight into the output buffer. A buffer the caller reuses
// keeps its capacity; a new one starts at the size a document of this length usually
// needs, so it does not grow several times while the pages are rendered.
static void PrepareOutput(std::string& pdfBytes, size_t expectedBytes) {
    constexpr size_t kMaxInitialReserve = 64u * 1024u * 1024u;
    pdfBytes.clear();
    expectedBytes = std::min(expectedBytes, kMaxInitialReserve);
    if (pdfBytes.capacity() < expectedBytes) pdfBytes.reserve(expectedBytes);
}

// `text` is ASCII, or with `winAnsi` the document transcoded to WinAnsiEncoding; `source` is
// the document as given, which a reversible PDF embeds. With `book`, the book's files instead,
// and `text` all of their text.
static bool BuildStandardPdfBytes(const std::string& text, const std::string& source, bool winAnsi,
    const PdfOptions& options, std::string& pdfBytes, BuildResult& stats, const BookSource* book = nullptr) {
    // Character references come out as the WinAnsi codes the standard fonts show.
    const Internal::WinAnsiReferences references;
    std::vector<Block> blocks;
    Internal::Footnotes footnotes;
    std::vector<Internal::ContentsEntry> contents;   // the entries of the table of contents, if any
    std::unique_ptr<BookParse> bookParse;   // a book's files: off the stack, which single documents keep as it was
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Parse);
        if (book == nullptr) {
            blocks = ParseMarkdown(text, &footnotes);
            contents = Internal::PlaceContents(blocks, options.toc, options.tocDepth);
        } else {
            bookParse = std::make_unique<BookParse>();
            contents = ParseBook(*book, options, *bookParse);
        }
    }
    PdfObjects pdf;
    int pagesId = pdf.Reserve();
    // WinAnsiEncoding: without it the fonts' built-in StandardEncoding shows the straight
    // quote and the backtick as curly quotes, and transcoded Latin text has no codes above
    // 0x7F. The text widths in math_font_metrics.inc follow it.
    int fontRegularId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    int fontBoldId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    int fontMonoId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>");

    ImageRegistry imageRegistry(options);
    PrepareOutput(pdfBytes, text.size() * 4 + 32 * 1024);
    const ThemePalette palette(options.theme);
    StandardRenderer renderer(pdfBytes, options.style, options.margin, options.pageSize, &imageRegistry, winAnsi, palette);
    if (!options.highlightCode) renderer.DisableHighlighting();
    if (!contents.empty()) renderer.SetContents(&contents);
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Render);
        if (book != nullptr) RenderBook(renderer, imageRegistry, *book, *bookParse, !book->title.empty() && !options.theme.cover);
        else if (footnotes.notes.empty()) renderer.Render(blocks);
        else RenderWithFootnotes(renderer, blocks, footnotes);
    }
    // The headings without their footnote references, for anchors, bookmarks and titles.
    std::vector<HeadingMark> titledHeadings;
    std::vector<std::string> headingTitles;
    const std::vector<HeadingMark>& headings = footnotes.notes.empty() && (bookParse == nullptr || !bookParse->files.notes)
        ? renderer.Headings()
        : TitledHeadings(renderer.Headings(), titledHeadings, headingTitles);
    RayoMd::Profiling::ScopedPhase assemblyProfile(RayoMd::Profiling::Phase::Assembly);
    const Internal::ThemeLogoImage logo = ThemeLogo(options.theme, imageRegistry);
    std::vector<int> imageObjectIds = AddImageObjects(pdf, imageRegistry.Images());
    std::vector<size_t> contentsTargets;   // the heading of each entry of the table of contents
    const std::vector<std::string> contentsOverlays = ContentsOverlays(renderer, contents, contentsTargets);
    HeadingTargets headingTargets(headings, winAnsi, renderer.PageHeight(), renderer.Notes());
    if (!contents.empty()) headingTargets.SetContents(&contentsTargets);
    std::vector<InternalLink> internalLinks;
    std::vector<std::vector<int>> annotationIds = book == nullptr
        ? AddLinkAnnotationObjects(pdf, renderer.PageLinks(), winAnsi, headingTargets, internalLinks, false)
        : AddBookLinkAnnotations(pdf, renderer.PageLinks(), winAnsi, headings, *book->book, *bookParse,
            renderer.PageHeight(), contents.empty() ? nullptr : &contentsTargets, internalLinks, false);
    std::vector<int> mathFontIds;
    if (renderer.MathUsed()) mathFontIds = AddMathFontObjects(pdf);
    // The italic faces only when emphasis used them, so other documents keep their bytes.
    const int fontObliqueId = renderer.ObliqueUsed()
        ? pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique /Encoding /WinAnsiEncoding >>") : 0;
    const int fontBoldObliqueId = renderer.BoldObliqueUsed()
        ? pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-BoldOblique /Encoding /WinAnsiEncoding >>") : 0;

    const DocumentMetadata metadata = book == nullptr ? FrontMatterMetadata(source) : BookMetadata(*book->book);
    std::vector<std::string> bands;   // a theme's header and footer, one stream per page
    std::string cover;                // and its cover page
    if (Internal::HasThemeBands(options.theme) || options.theme.cover) {
        LayOutTheme(options, StandardThemeText(), metadata, headings, winAnsi, renderer.PageStarts().size(),
            { renderer.PageWidth(), renderer.PageHeight(), renderer.Margin() }, logo, palette, bands, cover);
    }
    std::vector<bool> flatePages;   // --compress: the pages whose content stream is compressed
    if (options.compress) CompressPageContents(pdfBytes, renderer.PageStarts(), flatePages);
    std::vector<int> pageIds;
    const std::vector<size_t>& pageStarts = renderer.PageStarts();
    for (size_t pageIndex = 0; pageIndex < pageStarts.size(); pageIndex++) {
        const size_t pageEnd = pageIndex + 1 < pageStarts.size() ? pageStarts[pageIndex + 1] : pdfBytes.size();
        int contentId = pdf.AddStreamInPlace(!flatePages.empty() && flatePages[pageIndex] ? "/Filter /FlateDecode" : "",
            pageStarts[pageIndex], pageEnd - pageStarts[pageIndex]);
        const int numberId = !bands.empty() ? pdf.AddStream("", bands[pageIndex])
            : options.pageNumbers ? AddPageNumber(pdf, pageIndex + 1, pageStarts.size(), renderer.Margin(), renderer.PageWidth(), "F1")
            : 0;
        const int contentsId = pageIndex < contentsOverlays.size() && !contentsOverlays[pageIndex].empty()
            ? pdf.AddStream("", contentsOverlays[pageIndex]) : 0;
        std::string page;
        page.reserve(192);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, renderer.PageWidth());
        page += " ";
        AppendF(page, renderer.PageHeight());
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, fontRegularId);
        page += " 0 R /F2 ";
        AppendInt(page, fontBoldId);
        page += " 0 R /F3 ";
        AppendInt(page, fontMonoId);
        page += " 0 R";
        if (fontObliqueId != 0) {
            page += " /F4 ";
            AppendInt(page, fontObliqueId);
            page += " 0 R";
        }
        if (fontBoldObliqueId != 0) {
            page += " /F5 ";
            AppendInt(page, fontBoldObliqueId);
            page += " 0 R";
        }
        AppendMathFontResources(page, mathFontIds);
        page += " >>";
        AppendXObjectResources(page, imageObjectIds);
        page += " >>";
        AppendContents(page, contentId, numberId, contentsId);
        if (pageIndex < annotationIds.size()) AppendPageAnnotations(page, annotationIds[pageIndex]);
        page += " >>";
        pageIds.push_back(pdf.Add(std::move(page)));
    }

    int coverId = 0;
    if (options.theme.cover) {
        std::string fonts = "/F1 ";
        AppendInt(fonts, fontRegularId);
        fonts += " 0 R /F2 ";
        AppendInt(fonts, fontBoldId);
        fonts += " 0 R";
        coverId = AddCoverPage(pdf, pagesId, renderer.PageWidth(), renderer.PageHeight(), fonts, imageObjectIds, cover);
    }
    pdf.Set(pagesId, PagesDictionary(pageIds, coverId));
    AddInternalLinks(pdf, internalLinks, pageIds, false);

    std::string catalog;
    catalog.reserve(options.embedSource ? 256 : 64);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R";
    AppendLanguage(catalog, metadata);
    if (coverId != 0) catalog += kCoverPageLabels;
    std::string outline;
    AddOutline(pdf, catalog, headings, pageIds, winAnsi, outline);
    if (options.embedSource) AddReversibleSource(pdf, catalog, source);
    catalog += " >>";
    int catalogId = pdf.Add(std::move(catalog));
    int infoId = pdf.Add(InfoDictionary("RayoMD Native Standard PDF", metadata, headings, winAnsi));

    pdf.BuildInto(catalogId, infoId, pdfBytes, options.embedSource);
    stats.pages = static_cast<uint32_t>(pageStarts.size() + (coverId != 0));
    stats.failedImages = imageRegistry.FailedImages();
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

// A theme's font, loaded once per path and kept, as the default font is: builds on other threads
// may be using it. Null when it cannot be read or is no TrueType font.
RAYOMD_COLD static const TtfFont* ThemeFont(const std::string& pathUtf8) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<TtfFont>> fonts;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = fonts.find(pathUtf8);
    if (found != fonts.end()) return found->second.get();
    auto font = std::make_unique<TtfFont>();
#ifdef _WIN32
    if (!font->TryLoad(Utf8ToWide(pathUtf8))) font.reset();
#else
    if (!font->TryLoad(pathUtf8)) font.reset();
#endif
    return fonts.emplace(pathUtf8, std::move(font)).first->second.get();
}

// Whether the theme's own header and footer text has characters the standard fonts cannot show:
// they have WinAnsi's only.
RAYOMD_COLD static bool ThemeTextNeedsUnicode(const PdfTheme& theme) {
    std::string winAnsi;
    for (const std::string* field : { &theme.headerLeft, &theme.headerCenter, &theme.headerRight, &theme.footerLeft,
             &theme.footerCenter, &theme.footerRight }) {
        if (!IsAllAscii(*field) && !RayoMd::Text::TranscodeToWinAnsi(*field, &winAnsi)) return true;
    }
    return false;
}

// Fonts for documents with characters the default font has no glyph for: RAYOMD_FALLBACK_FONT,
// then broad CJK and multi-script fonts where the common systems install them. Such a document
// is drawn in one font, so a candidate must show Latin text too (Droid Sans Fallback does
// not), and only TrueType outlines can be subset (OpenType CFF fonts such as Noto Sans CJK
// do not load).
#ifdef _WIN32
static const wchar_t* const kFallbackFontNames[] = { L"msyh.ttc", L"msjh.ttc", L"YuGothR.ttc", L"meiryo.ttc",
    L"msgothic.ttc", L"malgun.ttf", L"Nirmala.ttf", L"seguisym.ttf", L"simsun.ttc", L"arialuni.ttf" };
#else
static const char* const kFallbackFontNames[] = {
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",                       // Debian, Ubuntu
    "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",                       // Fedora
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",             // Arch
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",                      // many alphabets, Indic too
    "/usr/share/fonts/gnu-free/FreeSans.ttf",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",                 // macOS
    "/Library/Fonts/Arial Unicode.ttf",
};
#endif
static constexpr size_t kFallbackFontCount = 1 + sizeof(kFallbackFontNames) / sizeof(kFallbackFontNames[0]);

// Candidate `index` of the fallback fonts; 0 is RAYOMD_FALLBACK_FONT.
RAYOMD_COLD static bool LoadFallbackFont(size_t index, TtfFont& font) {
#ifdef _WIN32
    if (index == 0) {
        const wchar_t* path = _wgetenv(L"RAYOMD_FALLBACK_FONT");
        return path && *path && font.TryLoad(std::wstring(path));
    }
    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;
    return font.TryLoad(std::wstring(winDir) + L"\\Fonts\\" + kFallbackFontNames[index - 1]);
#else
    if (index == 0) {
        const char* path = std::getenv("RAYOMD_FALLBACK_FONT");
        return path && *path && font.TryLoad(std::string(path));
    }
    return font.TryLoad(std::string(kFallbackFontNames[index - 1]));
#endif
}

// The font that has glyphs for more of the characters `used` than `font`, the most of them;
// null when no fallback font does better. Each candidate is read once, for the characters it
// covers; one stays in memory once a document is drawn in it, and is never freed then.
RAYOMD_COLD static const TtfFont* BetterFontFor(const TtfFont& font, const CidList& used) {
    // Control characters have no glyph anywhere: no reason to change fonts.
    auto missingIn = [&](auto&& hasGlyph) {
        size_t count = 0;
        for (uint16_t cid : used) count += cid >= 0x20 && !hasGlyph(cid);
        return count;
    };
    size_t fewest = missingIn([&](uint16_t cid) { return font.HasGlyph(cid); });
    if (fewest == 0) return nullptr;

    struct Candidate {
        bool probed = false;
        bool chosen = false;
        std::vector<uint64_t> coverage;     // a bit per BMP code point it has a glyph for; empty if it did not load
        std::unique_ptr<TtfFont> font;
    };
    static std::mutex mutex;
    static Candidate candidates[kFallbackFontCount];
    std::lock_guard<std::mutex> lock(mutex);
    size_t best = kFallbackFontCount;
    for (size_t index = 0; index < kFallbackFontCount && fewest != 0; index++) {
        Candidate& candidate = candidates[index];
        if (!candidate.probed) {
            candidate.probed = true;
            auto loaded = std::make_unique<TtfFont>();
            if (LoadFallbackFont(index, *loaded)) {
                candidate.coverage.assign(1024, 0);
                for (uint32_t cp = 0; cp < 65536; cp++) {
                    if (loaded->HasGlyph(cp)) candidate.coverage[cp >> 6] |= 1ull << (cp & 63);
                }
                candidate.font = std::move(loaded);
            }
        }
        if (candidate.coverage.empty()) continue;
        const size_t count = missingIn([&](uint16_t cid) { return (candidate.coverage[cid >> 6] >> (cid & 63)) & 1; });
        if (count < fewest) {
            best = index;
            fewest = count;
        }
    }
    for (size_t index = 0; index < kFallbackFontCount; index++) {
        if (index != best && !candidates[index].chosen) candidates[index].font.reset();
    }
    if (best == kFallbackFontCount) return nullptr;
    Candidate& chosen = candidates[best];
    if (!chosen.font) {
        chosen.font = std::make_unique<TtfFont>();
        if (!LoadFallbackFont(best, *chosen.font)) {
            chosen.font.reset();
            return nullptr;
        }
    }
    chosen.chosen = true;
    return chosen.font.get();
}

// With `betterFont`, a document with characters that `font` has no glyph for is not built
// when a fallback font has more of them: *betterFont is set, to build it again in that one.
// With `book`, the book's files instead of `markdown`, which then has all of their text.
static bool BuildUnicodePdfBytes(const std::string& markdown, const TtfFont& font, const PdfOptions& options,
    std::string& pdfBytes, const TtfFont** betterFont, size_t& missingCharacters, BuildResult& stats,
    const BookSource* book = nullptr) {
    std::vector<Block> blocks;
    Internal::Footnotes footnotes;
    std::vector<Internal::ContentsEntry> contents;   // the entries of the table of contents, if any
    std::unique_ptr<BookParse> bookParse;   // a book's files: off the stack, which single documents keep as it was
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Parse);
        if (book == nullptr) {
            blocks = ParseMarkdown(markdown, &footnotes);
            contents = Internal::PlaceContents(blocks, options.toc, options.tocDepth);
        } else {
            bookParse = std::make_unique<BookParse>();
            contents = ParseBook(*book, options, *bookParse);
        }
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
    const ThemePalette palette(options.theme);
    Renderer renderer(pdfBytes, font, type0FontId, options.style, options.margin, options.pageSize, &imageRegistry, palette);
    if (options.pdfa) renderer.DisableMath();
    if (!options.highlightCode) renderer.DisableHighlighting();
    if (!contents.empty()) renderer.SetContents(&contents);
    {
        RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Render);
        if (book != nullptr) RenderBook(renderer, imageRegistry, *book, *bookParse, !book->title.empty() && !options.theme.cover);
        else if (footnotes.notes.empty()) renderer.Render(blocks);
        else RenderWithFootnotes(renderer, blocks, footnotes);
    }
    // The headings without their footnote references, for anchors, bookmarks and titles.
    std::vector<HeadingMark> titledHeadings;
    std::vector<std::string> headingTitles;
    const std::vector<HeadingMark>& headings = footnotes.notes.empty() && (bookParse == nullptr || !bookParse->files.notes)
        ? renderer.Headings()
        : TitledHeadings(renderer.Headings(), titledHeadings, headingTitles);
    RayoMd::Profiling::ScopedPhase assemblyProfile(RayoMd::Profiling::Phase::Assembly);
    const Internal::ThemeLogoImage logo = ThemeLogo(options.theme, imageRegistry);
    std::vector<int> imageObjectIds = AddImageObjects(pdf, imageRegistry.Images());
    // The table of contents' page numbers, before the font subset is made, which then has their glyphs.
    std::vector<size_t> contentsTargets;   // the heading of each entry of the table of contents
    const std::vector<std::string> contentsOverlays = ContentsOverlays(renderer, contents, contentsTargets);
    HeadingTargets headingTargets(headings, false, renderer.PageHeight(), renderer.Notes());
    if (!contents.empty()) headingTargets.SetContents(&contentsTargets);
    std::vector<InternalLink> internalLinks;
    std::vector<std::vector<int>> annotationIds = book == nullptr
        ? AddLinkAnnotationObjects(pdf, renderer.PageLinks(), false, headingTargets, internalLinks, options.pdfa)
        : AddBookLinkAnnotations(pdf, renderer.PageLinks(), false, headings, *book->book, *bookParse,
            renderer.PageHeight(), contents.empty() ? nullptr : &contentsTargets, internalLinks, options.pdfa);
    std::vector<int> mathFontIds;
    if (renderer.MathUsed()) mathFontIds = AddMathFontObjects(pdf);

    // A theme's header and footer, one stream per page, PDF/A's page numbers, and a theme's cover
    // page, before the font subset is made, which then has their glyphs.
    DocumentMetadata metadata = book == nullptr ? FrontMatterMetadata(markdown) : BookMetadata(*book->book);
    std::vector<std::string> bands;
    std::string cover;
    if (Internal::HasThemeBands(options.theme) || options.theme.cover || (options.pdfa && options.pageNumbers)) {
        LayOutTheme(options, UnicodeThemeText(font, renderer.Cids()), metadata, headings, false,
            renderer.PageStarts().size(), { renderer.PageWidth(), renderer.PageHeight(), renderer.Margin() }, logo, palette,
            bands, cover);
    }
    const CidList& used = renderer.UsedCids();
    if (betterFont && (*betterFont = BetterFontFor(font, used)) != nullptr) return true;
    missingCharacters = renderer.MissingCharacters();
    std::string cidKey = MakeCidKey(font, used);
    auto cidMapBytes = MakeCidToGidMap(font, used, cidKey);
    auto toUnicodeBytes = MakeToUnicodeCMap(used, cidKey);

    // The cached font objects are referenced, not copied: the shared pointers keep them
    // alive until BuildInto below.
    std::shared_ptr<const std::string> fontFileObject = CachedFontFileObject(font, used, cidKey, options.compress);
    pdf.SetView(fontFileId, *fontFileObject);

    // With --compress, the compressed CMaps, which have to outlive BuildInto.
    std::string cidMapFlate;
    std::string toUnicodeFlate;
    SetPayloadStream(pdf, cidMapId, *cidMapBytes, options.compress, cidMapFlate);
    SetPayloadStream(pdf, toUnicodeId, *toUnicodeBytes, options.compress, toUnicodeFlate);

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

    // Helvetica for the page numbers; a theme's header and footer, and PDF/A's numbers, use the document's font.
    const int numberFontId = options.pageNumbers && bands.empty()
        ? pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>") : 0;
    std::vector<bool> flatePages;   // --compress: the pages whose content stream is compressed
    if (options.compress) CompressPageContents(pdfBytes, renderer.PageStarts(), flatePages);
    std::vector<int> pageIds;
    const std::vector<size_t>& pageStarts = renderer.PageStarts();
    for (size_t pageIndex = 0; pageIndex < pageStarts.size(); pageIndex++) {
        const size_t pageEnd = pageIndex + 1 < pageStarts.size() ? pageStarts[pageIndex + 1] : pdfBytes.size();
        int contentId = pdf.AddStreamInPlace(!flatePages.empty() && flatePages[pageIndex] ? "/Filter /FlateDecode" : "",
            pageStarts[pageIndex], pageEnd - pageStarts[pageIndex]);
        const int numberId = !bands.empty() ? pdf.AddStream("", bands[pageIndex])
            : options.pageNumbers ? AddPageNumber(pdf, pageIndex + 1, pageStarts.size(), renderer.Margin(), renderer.PageWidth(), "FN")
            : 0;
        const int contentsId = pageIndex < contentsOverlays.size() && !contentsOverlays[pageIndex].empty()
            ? pdf.AddStream("", contentsOverlays[pageIndex]) : 0;
        std::string page;
        page.reserve(160);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, renderer.PageWidth());
        page += " ";
        AppendF(page, renderer.PageHeight());
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, type0FontId);
        page += " 0 R";
        if (numberFontId != 0) {
            page += " /FN ";
            AppendInt(page, numberFontId);
            page += " 0 R";
        }
        AppendMathFontResources(page, mathFontIds);
        page += " >>";
        AppendXObjectResources(page, imageObjectIds);
        page += " >>";
        AppendContents(page, contentId, numberId, contentsId);
        if (pageIndex < annotationIds.size()) AppendPageAnnotations(page, annotationIds[pageIndex]);
        page += " >>";
        pageIds.push_back(pdf.Add(std::move(page)));
    }

    int coverId = 0;
    if (options.theme.cover) {
        std::string fonts = "/F1 ";
        AppendInt(fonts, type0FontId);
        fonts += " 0 R";
        coverId = AddCoverPage(pdf, pagesId, renderer.PageWidth(), renderer.PageHeight(), fonts, imageObjectIds, cover);
    }
    pdf.Set(pagesId, PagesDictionary(pageIds, coverId));
    AddInternalLinks(pdf, internalLinks, pageIds, options.pdfa);

    std::string catalog;
    catalog.reserve(options.embedSource || options.pdfa ? 512 : 64);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R";
    AppendLanguage(catalog, metadata);
    if (coverId != 0) catalog += kCoverPageLabels;
    std::string outline;
    AddOutline(pdf, catalog, headings, pageIds, false, outline);
    const char* const producer = "RayoMD Native Tiny PDF";
    if (options.pdfa) {
        AddPdfaCatalogEntries(pdf, catalog, metadata, headings, producer,
            options.embedSource ? std::string_view(markdown) : std::string_view());
    }
    if (options.embedSource) AddReversibleSource(pdf, catalog, markdown, !options.pdfa);
    catalog += " >>";
    int catalogId = pdf.Add(std::move(catalog));
    const std::vector<HeadingMark> noHeadings;      // PDF/A: `metadata` has the title
    int infoId = pdf.Add(InfoDictionary(producer, metadata, options.pdfa ? noHeadings : headings, false));

    // PDF/A-3 is PDF 1.7, also with the source embedded, and has a file identifier.
    pdf.BuildInto(catalogId, infoId, pdfBytes, options.embedSource && !options.pdfa, options.pdfa);
    stats.pages = static_cast<uint32_t>(pageStarts.size() + (coverId != 0));
    stats.failedImages = imageRegistry.FailedImages();
    return true;
}

// The fonts that show a document's text (BuildPdf, BuildBookPdf): the standard fonts for ASCII,
// and in WinAnsiEncoding for Latin text, which `winAnsi` then gets; else a TrueType font.
enum class TextFonts : uint8_t { Ascii, WinAnsi, TrueType };

static TextFonts ChooseFonts(const std::string& markdown, const PdfOptions& options, std::string& winAnsi) {
    // A theme's font shows all text, and a theme's header and footer text that is not WinAnsi
    // needs a TrueType font too, as PDF/A, which embeds every font, does for all text.
    if (!options.theme.fontPath.empty() || options.pdfa || ThemeTextNeedsUnicode(options.theme)) return TextFonts::TrueType;
    // A character reference counts as the character it stands for: "&copy;" needs the WinAnsi
    // text of the standard fonts, "&rarr;" a Unicode font. A document is searched for them only
    // where it would otherwise go to the standard fonts.
    const bool ascii = IsPlainAsciiDocument(markdown);
    Internal::ReferenceNeed references = Internal::ReferenceNeed::Ascii;
    if (ascii && (references = Internal::CharacterReferenceNeed(markdown)) == Internal::ReferenceNeed::Ascii) {
        return TextFonts::Ascii;
    }
    // Latin text needs no font file: the standard fonts show it in WinAnsiEncoding.
    if (references != Internal::ReferenceNeed::Unicode && RayoMd::Text::TranscodeToWinAnsi(markdown, &winAnsi) &&
        (ascii || Internal::CharacterReferenceNeed(markdown) != Internal::ReferenceNeed::Unicode)) {
        return TextFonts::WinAnsi;
    }
    return TextFonts::TrueType;
}

// A document in a TrueType font (TextFonts::TrueType): the theme's, without a fallback font: the
// company's look; else the system's, built again in a fallback font that has more of the
// characters it lacks. Without a font PDF/A fails, and other documents take the standard fonts,
// which show what they can. `unicode(font, better, missing)` builds in a TrueType font and
// `lossy(missing)` in the standard fonts; false with g_lastError set when nothing is built.
template <typename Unicode, typename Lossy>
static bool BuildInTrueType(const PdfOptions& options, uint32_t& missingCharacters, Unicode unicode, Lossy lossy) {
    size_t missing = 0;
    bool built = false;
    if (!options.theme.fontPath.empty()) {
        const TtfFont* font = ThemeFont(options.theme.fontPath);
        if (!font) {
            g_lastError = static_cast<int>(BuildError::ThemeFontUnavailable);
            return false;
        }
        built = unicode(*font, nullptr, missing);
    } else {
        const TtfFont* font = nullptr;
        {
            RayoMd::Profiling::ScopedPhase fontProfile(RayoMd::Profiling::Phase::Font);
            font = GetCachedFont();
        }
        if (font) {
            const TtfFont* better = nullptr;
            built = unicode(*font, &better, missing);
            if (built && better) built = unicode(*better, nullptr, missing);
        } else if (options.pdfa) {
            g_lastError = static_cast<int>(BuildError::PdfaFontUnavailable);
            return false;
        } else {
            // The caller learns how many characters the standard fonts could not show.
            built = lossy(missing);
        }
    }
    missingCharacters = static_cast<uint32_t>(std::min<size_t>(missing, UINT32_MAX));
    return built;
}

// A book's text for the standard fonts in WinAnsiEncoding (BuildDocument), with `lossy` what they
// cannot show as they can.
RAYOMD_COLD static void TranscodeBook(BookSource& source, bool lossy) {
    const auto transcode = [lossy](std::string& text) {
        std::string latin;
        if (lossy) RayoMd::Text::TranscodeToWinAnsiLossy(text, latin);
        else RayoMd::Text::TranscodeToWinAnsi(text, &latin);
        text = std::move(latin);
    };
    transcode(source.title);
    for (std::string& text : source.texts) transcode(text);
    for (std::string& text : source.titles) transcode(text);
}

// A document, or with `book` that book, of which `markdown` is then all the text, in the fonts its
// text takes (ChooseFonts); `missingCharacters` gets how many characters they could not show.
static bool BuildDocument(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes,
    BuildResult& stats, uint32_t& missingCharacters, BookSource* book) {
    std::string winAnsi;
    switch (ChooseFonts(markdown, options, winAnsi)) {
    case TextFonts::Ascii:
        return BuildStandardPdfBytes(markdown, markdown, false, options, pdfBytes, stats, book);
    case TextFonts::WinAnsi:
        if (book != nullptr) TranscodeBook(*book, false);
        return BuildStandardPdfBytes(winAnsi, markdown, true, options, pdfBytes, stats, book);
    case TextFonts::TrueType:
        break;
    }
    return BuildInTrueType(options, missingCharacters,
        [&](const TtfFont& font, const TtfFont** better, size_t& missing) {
            return BuildUnicodePdfBytes(markdown, font, options, pdfBytes, better, missing, stats, book);
        },
        [&](size_t& missing) {
            missing = RayoMd::Text::TranscodeToWinAnsiLossy(markdown, winAnsi);
            if (book != nullptr) TranscodeBook(*book, true);
            return BuildStandardPdfBytes(winAnsi, markdown, true, options, pdfBytes, stats, book);
        });
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
    uint32_t missingCharacters = 0;
    BuildResult stats;
    bool built = BuildDocument(markdown, options, pdfBytes, stats, missingCharacters, nullptr);
    if (built && options.embedSource && pdfBytes.size() > RayoMd::PdfSource::kMaxPdfBytes) {
        pdfBytes.clear();
        g_lastError = static_cast<int>(BuildError::ReversiblePdfTooLarge);
        built = false;
    }
    RayoMd::Profiling::EmitDelta("build", profileBefore, RayoMd::Profiling::Capture());
    if (!built) return BuildResult{ static_cast<BuildError>(g_lastError) };
    BuildResult result = stats;
    result.missingCharacters = missingCharacters;
    return result;
}

BuildResult BuildBookPdf(const Book& book, const PdfOptions& options, std::string& pdfBytes) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    g_lastError = 0;
    PdfOptions bookOptions = options;
    bookOptions.embedSource = false;
    bookOptions.sourcePath = book.directory.empty() ? std::string() : book.directory + '/';
    // Its text as the renderer reads it, and all of it, for the choice of its fonts and the output
    // buffer's first size.
    BookSource source;
    source.book = &book;
    source.title = book.title;
    std::string all = book.title;
    for (const BookChapter& chapter : book.chapters) {
        source.texts.push_back(chapter.markdown);
        source.titles.push_back(chapter.title);
        all += '\n';
        all += chapter.title;
        all += '\n';
        all += chapter.markdown;
    }
    uint32_t missingCharacters = 0;
    BuildResult stats;
    const bool built = BuildDocument(all, bookOptions, pdfBytes, stats, missingCharacters, &source);
    RayoMd::Profiling::EmitDelta("book", profileBefore, RayoMd::Profiling::Capture());
    if (!built) return BuildResult{ static_cast<BuildError>(g_lastError) };
    BuildResult result = stats;
    result.missingCharacters = missingCharacters;
    return result;
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
