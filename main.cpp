// Fast Markdown - ImGui + DirectX11 Edition
// Clean modern UI with minimal footprint
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "imgui/misc/cpp/imgui_stdlib.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Windows 11 dark mode attribute
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Safe COM release helper
template<typename T>
void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

// ============================================================================
// DirectX 11 State
// ============================================================================
ID3D11Device*           g_pd3dDevice = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
IDXGISwapChain*         g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// ============================================================================
// App State
// ============================================================================
std::string g_markdownText;
int g_selectedEngine = 0;
int g_selectedStyle = 0;
bool g_openAfterExport = true;
std::atomic<bool> g_isExporting{false};
std::string g_statusText = "Ready";
const char* g_engines[] = { "Native Tiny PDF", "Pandoc (full)" };
const char* g_styles[] = { "Elegant", "Modern", "Tech" };
const char* g_styleArgs[] = {
    "--pdf-engine=xelatex -V geometry:margin=1.2in",
    "--pdf-engine=xelatex -V mainfont=\"Segoe UI\" -V geometry:margin=1in",
    "--pdf-engine=xelatex -V mainfont=Consolas -V geometry:margin=0.5in --highlight-style=breezedark"
};
const char* BASE_ARGS = "--highlight-style=tango -V colorlinks=true "
    "-V header-includes=\"\\usepackage{framed}\\usepackage{xcolor}"
    "\\definecolor{shadecolor}{RGB}{245,245,245}"
    "\\renewenvironment{quote}{\\begin{shaded*}}{\\end{shaded*}}\"";

// ============================================================================
// DirectX 11 Helpers
// ============================================================================
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, 
        &g_pd3dDeviceContext) != S_OK)
        return false;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

void CleanupDeviceD3D() {
    SafeRelease(g_mainRenderTargetView);
    SafeRelease(g_pSwapChain);
    SafeRelease(g_pd3dDeviceContext);
    SafeRelease(g_pd3dDevice);
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    SafeRelease(g_mainRenderTargetView);
}

// ============================================================================
// Utility Functions
// ============================================================================
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    wstr.resize(len - 1);
    return wstr;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    str.resize(len - 1);
    return str;
}

std::wstring GetDocumentsPath() {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, path)))
        return path;
    return L"C:\\";
}

std::wstring GetTempFilePath() {
    wchar_t tempDir[MAX_PATH], tempFile[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    GetTempFileNameW(tempDir, L"fmd", 0, tempFile);
    std::wstring path(tempFile);
    auto pos = path.rfind(L'.');
    if (pos != std::wstring::npos) path = path.substr(0, pos);
    return path + L".md";
}

std::wstring ShowSaveDialog(HWND hWnd) {
    wchar_t fileName[MAX_PATH] = L"document.pdf";
    std::wstring initDir = GetDocumentsPath();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.lpstrDefExt = L"pdf";
    ofn.lpstrTitle = L"Export PDF";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn) ? fileName : L"";
}

bool WriteUtf8File(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(hFile, content.c_str(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(hFile);
    return true;
}

bool ReadUtf8File(const std::wstring& path, std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024LL * 1024LL) {
        CloseHandle(hFile);
        return false;
    }

    content.assign((size_t)size.QuadPart, '\0');
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(content.size() - offset, 1024 * 1024);
        DWORD read = 0;
        if (!ReadFile(hFile, &content[0] + offset, chunk, &read, nullptr)) {
            CloseHandle(hFile);
            return false;
        }
        if (read == 0) break;
        offset += read;
    }
    content.resize(offset);

    CloseHandle(hFile);
    return true;
}

bool WriteBinaryFile(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    size_t offset = 0;
    while (offset < content.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(content.size() - offset, 1024 * 1024);
        DWORD written = 0;
        if (!WriteFile(hFile, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            CloseHandle(hFile);
            return false;
        }
        offset += written;
    }

    CloseHandle(hFile);
    return true;
}

// ============================================================================
// Native Tiny Markdown -> PDF Exporter
// ============================================================================
namespace TinyPdf {

constexpr double PAGE_W = 595.0;  // A4, points
constexpr double PAGE_H = 842.0;
int g_lastError = 0;

enum class BlockType {
    Heading,
    Paragraph,
    Bullet,
    Numbered,
    Quote,
    Code,
    Rule
};

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;
    int number = 0;
    std::string text;
};

static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static std::string LTrim(std::string s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) i++;
    return s.substr(i);
}

static std::string RTrim(std::string s) {
    while (!s.empty() && IsSpace(s.back())) s.pop_back();
    return s;
}

static std::string Trim(std::string s) {
    return RTrim(LTrim(std::move(s)));
}

static bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::vector<std::string> SplitLines(std::string text) {
    if (StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    lines.push_back(line);
    return lines;
}

static bool IsRuleLine(const std::string& line) {
    std::string s;
    for (char c : Trim(line)) {
        if (c != ' ') s.push_back(c);
    }
    if (s.size() < 3) return false;
    char c = s[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char x : s) {
        if (x != c) return false;
    }
    return true;
}

static bool ParseHeading(const std::string& line, int& level, std::string& text) {
    std::string s = LTrim(line);
    int count = 0;
    while (count < (int)s.size() && s[count] == '#') count++;
    if (count < 1 || count > 6) return false;
    if ((int)s.size() > count && !IsSpace(s[count])) return false;
    level = count;
    text = Trim(s.substr(count));
    while (!text.empty() && text.back() == '#') text.pop_back();
    text = RTrim(text);
    return true;
}

static bool ParseBullet(const std::string& line, std::string& text) {
    std::string s = LTrim(line);
    if (s.size() < 2) return false;
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && IsSpace(s[1])) {
        text = Trim(s.substr(2));
        return true;
    }
    return false;
}

static bool ParseNumbered(const std::string& line, int& number, std::string& text) {
    std::string s = LTrim(line);
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    if (i == 0 || i >= s.size()) return false;
    if ((s[i] != '.' && s[i] != ')') || i + 1 >= s.size() || !IsSpace(s[i + 1])) return false;
    number = atoi(s.substr(0, i).c_str());
    text = Trim(s.substr(i + 2));
    return true;
}

static bool IsBlockStart(const std::string& line) {
    int level = 0, number = 0;
    std::string text;
    std::string trimmed = Trim(line);
    return trimmed.empty() || StartsWith(trimmed, "```") || StartsWith(trimmed, "~~~") ||
        IsRuleLine(line) || ParseHeading(line, level, text) || ParseBullet(line, text) ||
        ParseNumbered(line, number, text) || StartsWith(LTrim(line), ">");
}

static std::string StripInlineMarkdown(const std::string& input) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size();) {
        if (input[i] == '!' && i + 1 < input.size() && input[i + 1] == '[') {
            size_t close = input.find("](", i + 2);
            size_t end = close == std::string::npos ? std::string::npos : input.find(')', close + 2);
            if (close != std::string::npos && end != std::string::npos) {
                out += "image: ";
                out += input.substr(i + 2, close - (i + 2));
                i = end + 1;
                continue;
            }
        }

        if (input[i] == '[') {
            size_t close = input.find("](", i + 1);
            size_t end = close == std::string::npos ? std::string::npos : input.find(')', close + 2);
            if (close != std::string::npos && end != std::string::npos) {
                out += input.substr(i + 1, close - (i + 1));
                std::string url = input.substr(close + 2, end - (close + 2));
                if (!url.empty()) {
                    out += " <";
                    out += url;
                    out += ">";
                }
                i = end + 1;
                continue;
            }
        }

        if (input[i] == '`') {
            size_t end = input.find('`', i + 1);
            if (end != std::string::npos) {
                out += input.substr(i + 1, end - i - 1);
                i = end + 1;
                continue;
            }
        }

        if (input[i] == '\\' && i + 1 < input.size()) {
            out.push_back(input[i + 1]);
            i += 2;
            continue;
        }

        if (input[i] == '*') {
            i++;
            continue;
        }

        if (input[i] == '_') {
            bool leftWord = i > 0 && std::isalnum((unsigned char)input[i - 1]);
            bool rightWord = i + 1 < input.size() && std::isalnum((unsigned char)input[i + 1]);
            if (!leftWord || !rightWord) {
                i++;
                continue;
            }
        }

        if (input[i] == '~' && i + 1 < input.size() && input[i + 1] == '~') {
            i += 2;
            continue;
        }

        out.push_back(input[i++]);
    }

    return Trim(out);
}

static std::vector<Block> ParseMarkdown(const std::string& markdown) {
    std::vector<std::string> lines = SplitLines(markdown);
    std::vector<Block> blocks;
    size_t i = 0;

    if (!lines.empty() && Trim(lines[0]) == "---") {
        i = 1;
        while (i < lines.size() && Trim(lines[i]) != "---" && Trim(lines[i]) != "...") i++;
        if (i < lines.size()) i++;
    }

    while (i < lines.size()) {
        std::string line = lines[i];
        std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            i++;
            continue;
        }

        if (StartsWith(trimmed, "```") || StartsWith(trimmed, "~~~")) {
            std::string fence = trimmed.substr(0, 3);
            std::string text;
            i++;
            while (i < lines.size() && !StartsWith(Trim(lines[i]), fence.c_str())) {
                text += lines[i];
                if (i + 1 < lines.size()) text += "\n";
                i++;
            }
            if (i < lines.size()) i++;
            blocks.push_back({ BlockType::Code, 0, 0, text });
            continue;
        }

        int level = 0;
        std::string text;
        if (ParseHeading(line, level, text)) {
            blocks.push_back({ BlockType::Heading, level, 0, StripInlineMarkdown(text) });
            i++;
            continue;
        }

        if (IsRuleLine(line)) {
            blocks.push_back({ BlockType::Rule, 0, 0, "" });
            i++;
            continue;
        }

        if (ParseBullet(line, text)) {
            blocks.push_back({ BlockType::Bullet, 0, 0, StripInlineMarkdown(text) });
            i++;
            continue;
        }

        int number = 0;
        if (ParseNumbered(line, number, text)) {
            blocks.push_back({ BlockType::Numbered, 0, number, StripInlineMarkdown(text) });
            i++;
            continue;
        }

        if (StartsWith(LTrim(line), ">")) {
            std::string quote;
            while (i < lines.size() && StartsWith(LTrim(lines[i]), ">")) {
                std::string q = LTrim(lines[i]).substr(1);
                if (!q.empty() && q[0] == ' ') q.erase(0, 1);
                if (!quote.empty()) quote += " ";
                quote += Trim(q);
                i++;
            }
            blocks.push_back({ BlockType::Quote, 0, 0, StripInlineMarkdown(quote) });
            continue;
        }

        std::string paragraph;
        while (i < lines.size() && !IsBlockStart(lines[i])) {
            if (!paragraph.empty()) paragraph += " ";
            paragraph += Trim(lines[i]);
            i++;
        }
        if (paragraph.empty()) {
            paragraph = trimmed;
            i++;
        }
        blocks.push_back({ BlockType::Paragraph, 0, 0, StripInlineMarkdown(paragraph) });
    }

    return blocks;
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

struct TtfFont {
    struct Table {
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    std::vector<uint8_t> bytes;
    std::map<std::string, Table> tables;
    std::vector<uint16_t> glyphForBmp;
    std::vector<uint16_t> advances;
    uint16_t unitsPerEm = 1000;
    int16_t ascent = 900;
    int16_t descent = -220;
    int16_t xMin = 0;
    int16_t yMin = -220;
    int16_t xMax = 1000;
    int16_t yMax = 900;
    bool loaded = false;

    bool Load() {
        wchar_t winDir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;

        std::vector<std::wstring> candidates;
        std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";
        candidates.push_back(fontsDir + L"segoeui.ttf");
        candidates.push_back(fontsDir + L"arial.ttf");
        candidates.push_back(fontsDir + L"tahoma.ttf");

        for (const auto& path : candidates) {
            bytes.clear();
            tables.clear();
            glyphForBmp.clear();
            advances.clear();
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
            !tables.count("hmtx") || !tables.count("cmap")) {
            return false;
        }

        Table head = tables["head"];
        unitsPerEm = ReadU16(bytes, head.offset + 18);
        xMin = ReadS16(bytes, head.offset + 36);
        yMin = ReadS16(bytes, head.offset + 38);
        xMax = ReadS16(bytes, head.offset + 40);
        yMax = ReadS16(bytes, head.offset + 42);
        if (unitsPerEm == 0) unitsPerEm = 1000;

        Table hhea = tables["hhea"];
        ascent = ReadS16(bytes, hhea.offset + 4);
        descent = ReadS16(bytes, hhea.offset + 6);
        uint16_t metricCount = ReadU16(bytes, hhea.offset + 34);

        Table maxp = tables["maxp"];
        uint16_t glyphCount = ReadU16(bytes, maxp.offset + 4);
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

        glyphForBmp.assign(65536, 0);
        if (!ParseCmap()) return false;
        return glyphForBmp['A'] != 0 && glyphForBmp[' '] != 0;
    }

    bool ParseCmap() {
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
                if (cp < glyphForBmp.size()) glyphForBmp[cp] = glyph;
                if (cp == 0xffff) break;
            }
        }

        return true;
    }

    uint16_t GlyphFor(uint32_t cp) const {
        if (cp < glyphForBmp.size() && glyphForBmp[cp] != 0) return glyphForBmp[cp];
        return glyphForBmp['?'];
    }

    uint16_t WidthForCid(uint16_t cid) const {
        uint16_t glyph = GlyphFor(cid);
        if (glyph < advances.size()) {
            return (uint16_t)((advances[glyph] * 1000u + unitsPerEm / 2u) / unitsPerEm);
        }
        return 500;
    }

    int Metric(int value) const {
        return (int)((value * 1000.0) / unitsPerEm);
    }
};

static std::vector<uint32_t> Codepoints(const std::wstring& text) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t cp = (uint16_t)text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            uint32_t lo = (uint16_t)text[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        cps.push_back(cp);
    }
    return cps;
}

static std::wstring NormalizeSpaces(std::wstring text) {
    std::wstring out;
    bool inSpace = false;
    for (wchar_t ch : text) {
        bool space = ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r';
        if (space) {
            if (!inSpace && !out.empty()) out.push_back(L' ');
            inSpace = true;
        } else {
            out.push_back(ch);
            inSpace = false;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static double TextWidth(const TtfFont& font, const std::wstring& text, double size) {
    double w = 0.0;
    for (uint32_t cp : Codepoints(text)) {
        uint16_t cid = (cp <= 0xffff && font.GlyphFor(cp) != 0) ? (uint16_t)cp : (uint16_t)'?';
        w += font.WidthForCid(cid) * size / 1000.0;
    }
    return w;
}

static std::vector<std::wstring> WrapText(const TtfFont& font, const std::wstring& raw, double maxWidth, double size) {
    std::wstring text = NormalizeSpaces(raw);
    std::vector<std::wstring> lines;
    std::wstring line;
    size_t i = 0;

    while (i < text.size()) {
        while (i < text.size() && text[i] == L' ') i++;
        size_t start = i;
        while (i < text.size() && text[i] != L' ') i++;
        std::wstring word = text.substr(start, i - start);
        if (word.empty()) continue;

        std::wstring candidate = line.empty() ? word : line + L" " + word;
        if (TextWidth(font, candidate, size) <= maxWidth) {
            line = candidate;
            continue;
        }

        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
        }

        std::wstring part;
        for (wchar_t ch : word) {
            std::wstring next = part + ch;
            if (!part.empty() && TextWidth(font, next, size) > maxWidth) {
                lines.push_back(part);
                part.clear();
            }
            part.push_back(ch);
        }
        line = part;
    }

    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back(L"");
    return lines;
}

static std::vector<std::wstring> WrapCodeLine(const TtfFont& font, std::wstring line, double maxWidth, double size) {
    for (wchar_t& ch : line) {
        if (ch == L'\t') ch = L' ';
    }

    std::vector<std::wstring> lines;
    std::wstring part;
    for (wchar_t ch : line) {
        std::wstring next = part + ch;
        if (!part.empty() && TextWidth(font, next, size) > maxWidth) {
            lines.push_back(part);
            part.clear();
        }
        part.push_back(ch);
    }
    lines.push_back(part);
    return lines;
}

static std::string F(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s(buf);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

static void AppendHex4(std::string& out, uint16_t value) {
    static const char* h = "0123456789ABCDEF";
    out.push_back(h[(value >> 12) & 0xf]);
    out.push_back(h[(value >> 8) & 0xf]);
    out.push_back(h[(value >> 4) & 0xf]);
    out.push_back(h[value & 0xf]);
}

static std::string HexText(const TtfFont& font, const std::wstring& text, std::set<uint16_t>& usedCids) {
    std::string out = "<";
    for (uint32_t cp : Codepoints(text)) {
        uint16_t cid = (cp <= 0xffff && font.GlyphFor(cp) != 0) ? (uint16_t)cp : (uint16_t)'?';
        usedCids.insert(cid);
        AppendHex4(out, cid);
    }
    out += ">";
    return out;
}

class PdfObjects {
public:
    int Reserve() {
        objects.push_back("");
        return (int)objects.size();
    }

    int Add(const std::string& body) {
        objects.push_back(body);
        return (int)objects.size();
    }

    void Set(int id, const std::string& body) {
        if (id > 0 && id <= (int)objects.size()) objects[id - 1] = body;
    }

    int AddStream(const std::string& dict, const std::string& data) {
        std::ostringstream obj;
        obj << "<< " << dict << " /Length " << data.size() << " >>\nstream\n";
        std::string body = obj.str();
        body += data;
        body += "\nendstream";
        return Add(body);
    }

    std::string Build(int rootId, int infoId) const {
        std::string pdf;
        pdf += "%PDF-1.7\n%";
        pdf.push_back((char)0xE2);
        pdf.push_back((char)0xE3);
        pdf.push_back((char)0xCF);
        pdf.push_back((char)0xD3);
        pdf += "\n";

        std::vector<size_t> offsets(objects.size() + 1, 0);
        for (size_t i = 0; i < objects.size(); i++) {
            offsets[i + 1] = pdf.size();
            pdf += std::to_string(i + 1);
            pdf += " 0 obj\n";
            pdf += objects[i];
            pdf += "\nendobj\n";
        }

        size_t xref = pdf.size();
        pdf += "xref\n0 ";
        pdf += std::to_string(objects.size() + 1);
        pdf += "\n0000000000 65535 f \n";

        char entry[32];
        for (size_t i = 1; i <= objects.size(); i++) {
            snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[i]);
            pdf += entry;
        }

        pdf += "trailer\n<< /Size ";
        pdf += std::to_string(objects.size() + 1);
        pdf += " /Root ";
        pdf += std::to_string(rootId);
        pdf += " 0 R /Info ";
        pdf += std::to_string(infoId);
        pdf += " 0 R >>\nstartxref\n";
        pdf += std::to_string(xref);
        pdf += "\n%%EOF\n";
        return pdf;
    }

private:
    std::vector<std::string> objects;
};

class Renderer {
public:
    Renderer(const TtfFont& f, int fontObject, int styleIndex)
        : font(f), fontId(fontObject), style(styleIndex) {
        margin = style == 2 ? 42.0 : (style == 1 ? 54.0 : 62.0);
        bodySize = style == 2 ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) {
        if (blocks.empty()) {
            RenderParagraph("Empty document");
            return;
        }

        for (const Block& block : blocks) {
            switch (block.type) {
            case BlockType::Heading:
                RenderHeading(block);
                break;
            case BlockType::Paragraph:
                RenderParagraph(block.text);
                break;
            case BlockType::Bullet:
                RenderParagraph("- " + block.text, margin + 16.0, PAGE_W - margin * 2.0 - 16.0);
                y -= 2.0;
                break;
            case BlockType::Numbered:
                RenderParagraph(std::to_string(block.number) + ". " + block.text, margin + 16.0, PAGE_W - margin * 2.0 - 16.0);
                y -= 2.0;
                break;
            case BlockType::Quote:
                RenderQuote(block.text);
                break;
            case BlockType::Code:
                RenderCode(block.text);
                break;
            case BlockType::Rule:
                RenderRule();
                break;
            }
        }
    }

    const std::vector<std::string>& Pages() const { return pages; }
    const std::set<uint16_t>& UsedCids() const { return usedCids; }

private:
    const TtfFont& font;
    int fontId = 0;
    int style = 0;
    double margin = 62.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    std::vector<std::string> pages;
    std::set<uint16_t> usedCids;

    void NewPage() {
        pages.push_back("");
        y = PAGE_H - margin;
    }

    void Ensure(double needed) {
        if (y - needed < margin) NewPage();
    }

    void DrawTextLine(double x, double size, const std::wstring& line, const char* color = "0.08 0.08 0.08") {
        double lh = size * 1.35;
        Ensure(lh);
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " rg BT /F1 ";
        c += F(size);
        c += " Tf 1 0 0 1 ";
        c += F(x);
        c += " ";
        c += F(y - size);
        c += " Tm ";
        c += HexText(font, line, usedCids);
        c += " Tj ET Q\n";
        y -= lh;
    }

    void DrawRect(double x, double top, double w, double h, const char* color) {
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " rg ";
        c += F(x);
        c += " ";
        c += F(top - h);
        c += " ";
        c += F(w);
        c += " ";
        c += F(h);
        c += " re f Q\n";
    }

    void RenderHeading(const Block& block) {
        static const double sizes[] = { 0, 26, 22, 18, 15, 13, 12 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        if (y < PAGE_H - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;

        std::wstring text = Utf8ToWide(block.text);
        for (const auto& line : WrapText(font, text, PAGE_W - margin * 2.0, size)) {
            DrawTextLine(margin, size, line, "0.02 0.02 0.02");
        }
        y -= level <= 2 ? 8.0 : 5.0;
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, PAGE_W - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        std::wstring wide = Utf8ToWide(text);
        for (const auto& line : WrapText(font, wide, width, bodySize)) {
            DrawTextLine(x, bodySize, line);
        }
        y -= 5.0;
    }

    void RenderQuote(const std::string& text) {
        std::wstring wide = Utf8ToWide(text);
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        for (const auto& line : WrapText(font, wide, width, bodySize)) {
            Ensure(lineHeight + 2.0);
            DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
            DrawRect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
            DrawTextLine(x, bodySize, line, "0.18 0.22 0.25");
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

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = pages.back();
        c += "q 0.68 0.68 0.68 RG 0.8 w ";
        c += F(margin);
        c += " ";
        c += F(y);
        c += " m ";
        c += F(PAGE_W - margin);
        c += " ";
        c += F(y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

static std::string MakeCidToGidMap(const TtfFont& font, const std::set<uint16_t>& used) {
    uint16_t maxCid = 255;
    for (uint16_t cid : used) maxCid = std::max(maxCid, cid);

    std::string map((size_t)(maxCid + 1) * 2, '\0');
    for (uint32_t cid = 0; cid <= maxCid; cid++) {
        uint16_t glyph = font.GlyphFor(cid);
        map[(size_t)cid * 2] = (char)((glyph >> 8) & 0xff);
        map[(size_t)cid * 2 + 1] = (char)(glyph & 0xff);
    }
    return map;
}

static std::string MakeToUnicodeCMap(const std::set<uint16_t>& used) {
    std::ostringstream ss;
    ss << "/CIDInit /ProcSet findresource begin\n"
       << "12 dict begin\nbegincmap\n"
       << "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
       << "/CMapName /FastMarkdownUnicode def\n"
       << "/CMapType 2 def\n"
       << "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";

    int count = 0;
    for (auto it = used.begin(); it != used.end();) {
        int chunk = std::min<int>(100, (int)std::distance(it, used.end()));
        ss << chunk << " beginbfchar\n";
        for (int i = 0; i < chunk; i++, ++it) {
            std::string hex;
            AppendHex4(hex, *it);
            ss << "<" << hex << "> <" << hex << ">\n";
            count++;
        }
        ss << "endbfchar\n";
    }

    if (count == 0) {
        ss << "1 beginbfchar\n<0020> <0020>\nendbfchar\n";
    }

    ss << "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return ss.str();
}

static std::string MakeWidths(const TtfFont& font, const std::set<uint16_t>& used) {
    std::ostringstream ss;
    ss << "[ ";
    if (used.empty()) {
        ss << "32 [ " << font.WidthForCid(32) << " ] ";
    } else {
        for (uint16_t cid : used) {
            ss << cid << " [ " << font.WidthForCid(cid) << " ] ";
        }
    }
    ss << "]";
    return ss.str();
}

static std::string EscapeLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) out.push_back(c);
    }
    return out;
}

static bool Export(const std::string& markdown, const std::wstring& outputPath, int styleIdx) {
    g_lastError = 0;
    TtfFont font;
    if (!font.Load()) {
        g_lastError = 1;
        return false;
    }

    std::vector<Block> blocks = ParseMarkdown(markdown);
    PdfObjects pdf;

    int fontFileId = pdf.Reserve();
    int cidMapId = pdf.Reserve();
    int toUnicodeId = pdf.Reserve();
    int descriptorId = pdf.Reserve();
    int cidFontId = pdf.Reserve();
    int type0FontId = pdf.Reserve();
    int pagesId = pdf.Reserve();

    Renderer renderer(font, type0FontId, styleIdx);
    renderer.Render(blocks);

    const std::set<uint16_t>& used = renderer.UsedCids();
    std::string cidMapBytes = MakeCidToGidMap(font, used);
    std::string toUnicodeBytes = MakeToUnicodeCMap(used);

    pdf.Set(fontFileId, "<< /Length " + std::to_string(font.bytes.size()) + " /Length1 " +
        std::to_string(font.bytes.size()) + " >>\nstream\n" +
        std::string((const char*)font.bytes.data(), font.bytes.size()) + "\nendstream");
    pdf.Set(cidMapId, "<< /Length " + std::to_string(cidMapBytes.size()) +
        " >>\nstream\n" + cidMapBytes + "\nendstream");
    pdf.Set(toUnicodeId, "<< /Length " + std::to_string(toUnicodeBytes.size()) +
        " >>\nstream\n" + toUnicodeBytes + "\nendstream");

    std::ostringstream desc;
    desc << "<< /Type /FontDescriptor /FontName /FastMarkdownSegoe /Flags 32"
         << " /FontBBox [" << font.Metric(font.xMin) << " " << font.Metric(font.yMin) << " "
         << font.Metric(font.xMax) << " " << font.Metric(font.yMax) << "]"
         << " /ItalicAngle 0 /Ascent " << font.Metric(font.ascent)
         << " /Descent " << font.Metric(font.descent)
         << " /CapHeight " << (int)(font.Metric(font.ascent) * 0.72)
         << " /StemV 80 /FontFile2 " << fontFileId << " 0 R >>";
    pdf.Set(descriptorId, desc.str());

    std::ostringstream cidFont;
    cidFont << "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /FastMarkdownSegoe"
            << " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
            << " /FontDescriptor " << descriptorId << " 0 R"
            << " /CIDToGIDMap " << cidMapId << " 0 R"
            << " /DW 500 /W " << MakeWidths(font, used) << " >>";
    pdf.Set(cidFontId, cidFont.str());

    std::ostringstream type0;
    type0 << "<< /Type /Font /Subtype /Type0 /BaseFont /FastMarkdownSegoe"
          << " /Encoding /Identity-H /DescendantFonts [" << cidFontId << " 0 R]"
          << " /ToUnicode " << toUnicodeId << " 0 R >>";
    pdf.Set(type0FontId, type0.str());

    std::vector<int> pageIds;
    for (const std::string& pageContent : renderer.Pages()) {
        int contentId = pdf.AddStream("", pageContent);
        std::ostringstream page;
        page << "<< /Type /Page /Parent " << pagesId << " 0 R"
             << " /MediaBox [0 0 " << F(PAGE_W) << " " << F(PAGE_H) << "]"
             << " /Resources << /Font << /F1 " << type0FontId << " 0 R >> >>"
             << " /Contents " << contentId << " 0 R >>";
        pageIds.push_back(pdf.Add(page.str()));
    }

    std::ostringstream pages;
    pages << "<< /Type /Pages /Kids [";
    for (int id : pageIds) pages << id << " 0 R ";
    pages << "] /Count " << pageIds.size() << " >>";
    pdf.Set(pagesId, pages.str());

    int catalogId = pdf.Add("<< /Type /Catalog /Pages " + std::to_string(pagesId) + " 0 R >>");
    int infoId = pdf.Add("<< /Producer (Fast Markdown Native Tiny PDF) /Creator (Fast Markdown) /Title (" +
        EscapeLiteral("Markdown Export") + ") >>");

    std::string pdfBytes = pdf.Build(catalogId, infoId);
    if (!WriteBinaryFile(outputPath, pdfBytes)) {
        g_lastError = 2;
        return false;
    }

    return true;
}

} // namespace TinyPdf

bool ExportNativePdf(const std::string& markdown, const std::wstring& outputPath, int styleIdx) {
    return TinyPdf::Export(markdown, outputPath, styleIdx);
}

int GetNativePdfLastError() {
    return TinyPdf::g_lastError;
}

bool RunPandoc(const std::wstring& input, const std::wstring& output, int styleIdx) {
    std::wstring cmd = L"pandoc \"" + input + L"\" -o \"" + output + L"\" " + 
        Utf8ToWide(BASE_ARGS) + L" " + Utf8ToWide(g_styleArgs[styleIdx]);
    
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

int ParseStyleArg(const wchar_t* arg) {
    if (!arg) return 0;
    if (lstrcmpiW(arg, L"modern") == 0) return 1;
    if (lstrcmpiW(arg, L"tech") == 0) return 2;
    if (lstrcmpiW(arg, L"elegant") == 0) return 0;
    int style = _wtoi(arg);
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    return style;
}

int TryCommandLineExport() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;

    if (argc < 2 || lstrcmpiW(argv[1], L"--export") != 0) {
        LocalFree(argv);
        return -1;
    }

    if (argc < 4) {
        LocalFree(argv);
        return 2;
    }

    std::wstring inputPath = argv[2];
    std::wstring outputPath = argv[3];
    if (outputPath.size() < 4 || _wcsicmp(outputPath.c_str() + outputPath.size() - 4, L".pdf") != 0) {
        outputPath += L".pdf";
    }

    int engine = 0;
    int style = 0;
    if (argc >= 5) {
        if (lstrcmpiW(argv[4], L"pandoc") == 0) {
            engine = 1;
        } else if (lstrcmpiW(argv[4], L"native") == 0) {
            engine = 0;
        } else {
            style = ParseStyleArg(argv[4]);
        }
    }
    if (argc >= 6) style = ParseStyleArg(argv[5]);

    bool ok = false;
    if (engine == 0) {
        std::string markdown;
        if (!ReadUtf8File(inputPath, markdown)) {
            LocalFree(argv);
            return 3;
        }
        ok = ExportNativePdf(markdown, outputPath, style);
    } else {
        ok = RunPandoc(inputPath, outputPath, style);
    }

    LocalFree(argv);
    if (ok) return 0;
    return engine == 0 ? 10 + GetNativePdfLastError() : 20;
}

// ============================================================================
// Export Thread
// ============================================================================
struct ExportParams {
    std::string markdown;
    std::wstring outputPath;
    int engineIdx;
    int styleIdx;
    bool openAfter;
    HWND hWnd;
};

DWORD WINAPI ExportThread(LPVOID lp) {
    auto* p = (ExportParams*)lp;
    bool ok = false;

    if (p->engineIdx == 0) {
        ok = ExportNativePdf(p->markdown, p->outputPath, p->styleIdx);
    } else {
        std::wstring tmp = GetTempFilePath();
        ok = WriteUtf8File(tmp, p->markdown) && RunPandoc(tmp, p->outputPath, p->styleIdx);
        DeleteFileW(tmp.c_str());
    }
    
    if (ok) {
        if (p->openAfter) {
            ShellExecuteW(nullptr, L"open", p->outputPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        g_statusText = "Export complete!";
    } else {
        g_statusText = "Export failed!";
    }
    
    g_isExporting = false;
    delete p;
    return 0;
}

void DoExport(HWND hWnd) {
    if (g_markdownText.empty()) {
        MessageBoxW(hWnd, L"No content to export.", L"Error", MB_ICONERROR);
        return;
    }
    
    std::wstring outPath = ShowSaveDialog(hWnd);
    if (outPath.empty()) return;
    
    // Ensure .pdf extension
    if (outPath.size() < 4 || _wcsicmp(outPath.c_str() + outPath.size() - 4, L".pdf") != 0)
        outPath += L".pdf";
    
    g_isExporting = true;
    g_statusText = g_selectedEngine == 0 ? "Converting with native PDF..." : "Converting with Pandoc...";
    
    auto* params = new ExportParams{ g_markdownText, outPath, g_selectedEngine, g_selectedStyle, g_openAfterExport, hWnd };
    HANDLE hThread = CreateThread(nullptr, 0, ExportThread, params, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

// ============================================================================
// ImGui Theme
// ============================================================================
void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    // Windows 11 Dark Style
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.12f, 0.12f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.38f, 0.80f, 1.00f, 0.80f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.38f, 0.80f, 1.00f, 0.85f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.50f, 0.86f, 1.00f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.30f, 0.70f, 0.90f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.38f, 0.80f, 1.00f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.38f, 0.80f, 1.00f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    
    // Style tweaks
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowPadding = ImVec2(16, 12);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.ScrollbarSize = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
}



// ============================================================================
// Window Procedure
// ============================================================================
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)wParam, 0, path, MAX_PATH)) {
            std::wstring p(path);
            if (p.size() > 3 && _wcsicmp(p.c_str() + p.size() - 3, L".md") == 0) {
                HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD size = GetFileSize(hFile, nullptr);
                    std::string content(size, 0);
                    DWORD read;
                    ReadFile(hFile, &content[0], size, &read, nullptr);
                    CloseHandle(hFile);
                    g_markdownText = content;
                    g_statusText = "File loaded!";
                }
            }
        }
        DragFinish((HDROP)wParam);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// Entry Point
// ============================================================================
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    int cliResult = TryCommandLineExport();
    if (cliResult >= 0) return cliResult;

    // Load custom icon
    HICON hIcon = (HICON)LoadImageW(nullptr, L"catto.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hIcon) hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    
    // Register window class
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, 
        hIcon, LoadCursor(nullptr, IDC_ARROW), 
        nullptr, nullptr, L"FastMarkdownImGui", hIcon };
    RegisterClassExW(&wc);
    
    // Create window
    HWND hWnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Fast Markdown",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);
    
    // Enable dark title bar (Windows 11 style)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    
    // Initialize Direct3D
    if (!CreateDeviceD3D(hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    
    // Show window
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    
    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Disable imgui.ini
    
    // Load font (use Segoe UI for Windows look)
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f);
    
    SetupImGuiStyle();
    
    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    
    // Reserve buffer for text
    g_markdownText.reserve(1024 * 1024); // 1MB
    
    // Main loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        
        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        // Get window size
        RECT rect;
        GetClientRect(hWnd, &rect);
        float width = (float)(rect.right - rect.left);
        float height = (float)(rect.bottom - rect.top);
        
        // Main window (fullscreen, no decoration)
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::Begin("##main", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        
        // Top toolbar
        float toolbarY = ImGui::GetCursorPosY();
        
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Engine");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("##engine", &g_selectedEngine, g_engines, IM_ARRAYSIZE(g_engines));

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Style");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145);
        ImGui::Combo("##style", &g_selectedStyle, g_styles, IM_ARRAYSIZE(g_styles));
        
        ImGui::SameLine(width - 305);
        ImGui::Checkbox("Open after export", &g_openAfterExport);
        
        ImGui::SameLine(width - 116);
        ImGui::BeginDisabled(g_isExporting);
        if (ImGui::Button(g_isExporting ? "Exporting..." : "Export PDF", ImVec2(90, 0))) {
            DoExport(hWnd);
        }
        ImGui::EndDisabled();
        
        // Keyboard shortcut
        if (ImGui::IsKeyPressed(ImGuiKey_E) && io.KeyCtrl && !g_isExporting) {
            DoExport(hWnd);
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        
        // Text editor (fill remaining space)
        float editorHeight = height - 110;
        
        // Draw placeholder if empty
        bool isEmpty = g_markdownText.empty() || g_markdownText[0] == '\0';
        
        ImGui::InputTextMultiline("##editor", &g_markdownText,
            ImVec2(-1, editorHeight),
            ImGuiInputTextFlags_AllowTabInput);
        
        // Draw placeholder on top if empty and not focused
        if (isEmpty && !ImGui::IsItemActive()) {
            ImVec2 pos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + 10, pos.y + 6),
                IM_COL32(100, 100, 100, 255),
                "Write your Markdown here... (or drag & drop a .md file)"
            );
        }
        
        // Status bar
        ImGui::Spacing();
        
        // Count stats
        int lines = 1, words = 0, chars = (int)g_markdownText.size();
        bool inWord = false;
        for (char c : g_markdownText) {
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') inWord = false;
            else if (!inWord) { inWord = true; words++; }
        }
        
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", g_statusText.c_str());
        ImGui::SameLine(width - 220);
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "%d lines  |  %d words  |  %d chars", lines, words, chars);
        
        ImGui::End();
        
        // Render
        ImGui::Render();
        const float clear_color[4] = { 0.05f, 0.05f, 0.05f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        g_pSwapChain->Present(1, 0); // Present with vsync
    }
    
    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupDeviceD3D();
    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    
    return 0;
}
