// Fast Markdown - ImGui + DirectX11 Edition
// Clean modern UI with minimal footprint
// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#endif
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <codecvt>
#include <locale>

#ifndef FAST_MARKDOWN_VERSION
#define FAST_MARKDOWN_VERSION "0.0.0"
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define FAST_MD_SSE2 1
#endif

#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "imgui/misc/cpp/imgui_stdlib.h"
#include "fast_markdown/tiny_pdf.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

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
#ifdef _WIN32
ID3D11Device*           g_pd3dDevice = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
IDXGISwapChain*         g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
#endif

// ============================================================================
// App State
// ============================================================================
std::string g_markdownText;
int g_selectedEngine = 0;
int g_selectedStyle = 0;
int g_selectedMargin = 1;
float g_customMarginInches = 1.0f;
bool g_openAfterExport = true;
std::atomic<bool> g_isExporting{false};
std::atomic<bool> g_isFileLoading{false};
std::atomic<int> g_forcedRenderFrames{0};
std::string g_statusText = "Ready";
std::wstring g_currentMarkdownPath;
int g_lineCount = 1;
int g_wordCount = 0;
int g_charCount = 0;
ImFont* g_fontBody = nullptr;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontStrong = nullptr;
ImFont* g_fontEditor = nullptr;
const char* g_engines[] = { "Native Tiny PDF", "Pandoc (full)" };
const char* g_styles[] = { "Elegant", "Modern", "Tech" };
const char* g_margins[] = { "Compact", "Normal", "Wide", "Custom" };
const char* g_styleArgs[] = {
    "--pdf-engine=xelatex",
    "--pdf-engine=xelatex -V mainfont=\"Segoe UI\"",
    "--pdf-engine=xelatex -V mainfont=Consolas --highlight-style=breezedark"
};
const char* g_marginArgs[] = {
    "-V geometry:margin=0.55in",
    "-V geometry:margin=1in",
    "-V geometry:margin=1.25in"
};
const char* BASE_ARGS = "--highlight-style=tango -V colorlinks=true "
    "-V header-includes=\"\\usepackage{framed}\\usepackage{xcolor}"
    "\\definecolor{shadecolor}{RGB}{245,245,245}"
    "\\renewenvironment{quote}{\\begin{shaded*}}{\\end{shaded*}}\"";

constexpr long long kMaxMarkdownInputBytes = 128LL * 1024LL * 1024LL;

#ifdef _WIN32
constexpr UINT WM_FAST_MARKDOWN_FILE_LOADED = WM_APP + 1;
constexpr UINT WM_FAST_MARKDOWN_EXPORT_DONE = WM_APP + 2;
#endif

// ============================================================================
// DirectX 11 Helpers
// ============================================================================
#ifdef _WIN32
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
#endif

// ============================================================================
// Utility Functions
// ============================================================================
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    wstr.resize(len - 1);
    return wstr;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str);
#endif
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    str.resize(len - 1);
    return str;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(wstr);
#endif
}

struct MarkdownStats {
    int lines = 1;
    int words = 0;
    int chars = 0;
};

MarkdownStats CalculateMarkdownStats(const std::string& text) {
    MarkdownStats stats;
    stats.chars = (int)text.size();

    bool inWord = false;
    for (char c : text) {
        if (c == '\n') stats.lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            stats.words++;
        }
    }
    return stats;
}

void UpdateMarkdownStats() {
    MarkdownStats stats = CalculateMarkdownStats(g_markdownText);
    g_lineCount = stats.lines;
    g_wordCount = stats.words;
    g_charCount = stats.chars;
}

#ifdef _WIN32
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
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > kMaxMarkdownInputBytes) {
        CloseHandle(hFile);
        return false;
    }

    if (size.QuadPart == 0) {
        content.clear();
        CloseHandle(hFile);
        return true;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (hMap) {
        const char* data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (data) {
            content.assign(data, data + (size_t)size.QuadPart);
            UnmapViewOfFile(data);
            CloseHandle(hMap);
            CloseHandle(hFile);
            return true;
        }
        CloseHandle(hMap);
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

    if (content.size() <= 4 * 1024 * 1024) {
        DWORD written = 0;
        bool ok = WriteFile(hFile, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size();
        CloseHandle(hFile);
        return ok;
    }

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
#endif

bool ExportNativePdf(const std::string& markdown, const std::wstring& outputPath,
    int styleIdx, int marginIdx, const std::wstring& sourcePath = L"") {
    static thread_local std::string pdfBytes;
    pdfBytes.clear();
    pdfBytes.reserve(1024 * 1024);
    TinyPdf::BuildOptions options;
    options.styleIdx = styleIdx;
    options.marginIdx = marginIdx;
    options.sourcePath = WideToUtf8(sourcePath);
    if (!TinyPdf::BuildPdfBytes(markdown, options, pdfBytes)) {
        return false;
    }
    if (!WriteBinaryFile(outputPath, pdfBytes)) {
        TinyPdf::g_lastError = 2;
        return false;
    }
    return true;
}

bool BuildNativePdfBytes(const std::string& markdown, int styleIdx, int marginIdx,
    std::string& pdfBytes, const std::wstring& sourcePath = L"") {
    TinyPdf::BuildOptions options;
    options.styleIdx = styleIdx;
    options.marginIdx = marginIdx;
    options.sourcePath = WideToUtf8(sourcePath);
    return TinyPdf::BuildPdfBytes(markdown, options, pdfBytes);
}

int GetNativePdfLastError() {
    return TinyPdf::g_lastError;
}

bool RunPandoc(const std::wstring& input, const std::wstring& output, int styleIdx, int marginIdx) {
    std::string marginArg;
    if (marginIdx >= 0 && marginIdx <= 2) {
        marginArg = g_marginArgs[marginIdx];
    } else if (marginIdx >= 1000) {
        char buf[96];
        double inches = (double)(marginIdx - 1000) / 72.0;
        snprintf(buf, sizeof(buf), "-V geometry:margin=%.2fin", inches);
        marginArg = buf;
    } else {
        marginArg = g_marginArgs[1];
    }

    std::wstring cmd = L"pandoc \"" + input + L"\" -o \"" + output + L"\" " + 
        Utf8ToWide(BASE_ARGS) + L" " + Utf8ToWide(g_styleArgs[styleIdx]) + L" " + Utf8ToWide(marginArg);
    
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

bool IsEngineArg(const wchar_t* arg) {
    return arg && (lstrcmpiW(arg, L"native") == 0 || lstrcmpiW(arg, L"pandoc") == 0);
}

bool IsStyleArg(const wchar_t* arg) {
    if (!arg) return false;
    return lstrcmpiW(arg, L"elegant") == 0 || lstrcmpiW(arg, L"modern") == 0 ||
        lstrcmpiW(arg, L"tech") == 0 || (arg[0] >= L'0' && arg[0] <= L'2' && arg[1] == 0);
}

int ParseMarginArg(const wchar_t* arg) {
    if (!arg) return 1;
    if (lstrcmpiW(arg, L"compact") == 0) return 0;
    if (lstrcmpiW(arg, L"normal") == 0) return 1;
    if (lstrcmpiW(arg, L"wide") == 0) return 2;

    const wchar_t* value = arg;
    if (_wcsnicmp(arg, L"margin=", 7) == 0) value = arg + 7;

    wchar_t* end = nullptr;
    double number = wcstod(value, &end);
    if (number <= 0.0) return 1;

    double points = number * 72.0;
    if (end && _wcsnicmp(end, L"pt", 2) == 0) points = number;
    else if (end && _wcsnicmp(end, L"in", 2) == 0) points = number * 72.0;

    points = std::max(18.0, std::min(144.0, points));
    return 1000 + (int)(points + 0.5);
}

bool IsMarginArg(const wchar_t* arg) {
    if (!arg) return false;
    return lstrcmpiW(arg, L"compact") == 0 || lstrcmpiW(arg, L"normal") == 0 ||
        lstrcmpiW(arg, L"wide") == 0 || _wcsnicmp(arg, L"margin=", 7) == 0;
}

void ParseExportOptions(int argc, LPWSTR* argv, int start, int& engine, int& style, int& margin) {
    for (int i = start; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"pandoc") == 0) {
            engine = 1;
        } else if (lstrcmpiW(argv[i], L"native") == 0) {
            engine = 0;
        } else if (IsMarginArg(argv[i])) {
            margin = ParseMarginArg(argv[i]);
        } else if (IsStyleArg(argv[i])) {
            style = ParseStyleArg(argv[i]);
        }
    }
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

bool EnsureDirectoryRecursive(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (!ec) return true;
    return std::filesystem::exists(std::filesystem::path(path));
}

std::wstring PdfNameForMarkdown(const std::wstring& name) {
    size_t slash = name.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? name : name.substr(slash + 1);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    return base + L".pdf";
}

bool ExportOneFile(const std::wstring& inputPath, const std::wstring& outputPath, int engine, int style, int margin) {
    if (engine == 0) {
        std::string markdown;
        return ReadUtf8File(inputPath, markdown) && ExportNativePdf(markdown, outputPath, style, margin, inputPath);
    }
    return RunPandoc(inputPath, outputPath, style, margin);
}

bool ExportNativeFileWithBuffer(const std::wstring& inputPath, const std::wstring& outputPath, int style, int margin, std::string& pdfBuffer) {
    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) return false;
    pdfBuffer.clear();
    if (!BuildNativePdfBytes(markdown, style, margin, pdfBuffer, inputPath)) return false;
    return WriteBinaryFile(outputPath, pdfBuffer);
}

int RunBatchExport(const std::wstring& inputDir, const std::wstring& outputDir, int engine, int style, int margin) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    WIN32_FIND_DATAW findData = {};
    HANDLE hFind = FindFirstFileW(JoinPath(inputDir, L"*.md").c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return 3;

    int failures = 0;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = findData.cFileName;
        std::wstring inputPath = JoinPath(inputDir, name);
        std::wstring outputPath = JoinPath(outputDir, PdfNameForMarkdown(name));
        if (!ExportOneFile(inputPath, outputPath, engine, style, margin)) failures++;
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    return failures == 0 ? 0 : (engine == 0 ? 10 + GetNativePdfLastError() : 20);
}

bool ReadAllStdin(std::string& input) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) return false;

    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hIn, buf, sizeof(buf), &read, nullptr) && read > 0) {
        input.append(buf, buf + read);
    }
    return !input.empty();
}

void WriteStdoutLine(const std::string& line) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr) return;
    DWORD written = 0;
    std::string output = line + "\n";
    WriteFile(hOut, output.data(), (DWORD)output.size(), &written, nullptr);
}

int RunStdinBatchExport(const std::wstring& outputDir, int engine, int style, int margin) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    std::string list;
    if (!ReadAllStdin(list)) return 3;

    int failures = 0;
    std::vector<std::string> lines = TinyPdf::SplitLines(list);
    for (std::string line : lines) {
        line = TinyPdf::Trim(line);
        if (line.empty()) continue;
        std::wstring inputPath = Utf8ToWide(line);
        std::wstring outputPath = JoinPath(outputDir, PdfNameForMarkdown(inputPath));
        if (!ExportOneFile(inputPath, outputPath, engine, style, margin)) failures++;
    }

    return failures == 0 ? 0 : (engine == 0 ? 10 + GetNativePdfLastError() : 20);
}

std::string FormatDurationMs(double ms) {
    char buf[64];
    if (ms < 0.0) {
        ms = 0.0;
    }

    if (ms < 1.0) {
        double us = ms * 1000.0;
        if (us < 10.0) {
            snprintf(buf, sizeof(buf), "%.2f us", us);
        } else if (us < 100.0) {
            snprintf(buf, sizeof(buf), "%.1f us", us);
        } else {
            snprintf(buf, sizeof(buf), "%.0f us", us);
        }
    } else if (ms < 10.0) {
        snprintf(buf, sizeof(buf), "%.2f ms", ms);
    } else if (ms < 100.0) {
        snprintf(buf, sizeof(buf), "%.1f ms", ms);
    } else if (ms < 1000.0) {
        snprintf(buf, sizeof(buf), "%.0f ms", ms);
    } else {
        snprintf(buf, sizeof(buf), "%.2f s", ms / 1000.0);
    }
    return buf;
}

int RunServeExport(const std::wstring& outputDir, int engine, int style, int margin) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) return 3;

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    std::string pending;
    char buf[4096];
    DWORD read = 0;
    int failures = 0;

    auto processLine = [&](std::string line) {
        line = TinyPdf::Trim(line);
        if (line.empty()) return true;
        if (line == "quit" || line == "exit") return false;

        std::wstring inputPath = Utf8ToWide(line);
        std::wstring outputPath = JoinPath(outputDir, PdfNameForMarkdown(inputPath));

        LARGE_INTEGER start = {}, end = {};
        QueryPerformanceCounter(&start);
        bool ok = engine == 0
            ? ExportNativeFileWithBuffer(inputPath, outputPath, style, margin, pdfBuffer)
            : ExportOneFile(inputPath, outputPath, engine, style, margin);
        QueryPerformanceCounter(&end);

        double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (!ok) failures++;
        WriteStdoutLine(std::string(ok ? "OK\t" : "ERR\t") + TinyPdf::F(ms) + "\t" + WideToUtf8(outputPath));
        return true;
    };

    while (ReadFile(hIn, buf, sizeof(buf), &read, nullptr) && read > 0) {
        pending.append(buf, buf + read);
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending.erase(0, pos + 1);
            if (!processLine(line)) return failures == 0 ? 0 : 20;
        }
    }

    if (!pending.empty()) processLine(pending);
    return failures == 0 ? 0 : (engine == 0 ? 10 + GetNativePdfLastError() : 20);
}

int RunNativeBench(const std::wstring& inputPath, const std::wstring& outputDir, int iterations, int style, int margin) {
    if (iterations < 1) iterations = 1;
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) return 3;

    std::string pdfBytes;
    if (!BuildNativePdfBytes(markdown, style, margin, pdfBytes, inputPath)) {
        return 10 + GetNativePdfLastError();
    }
    WriteBinaryFile(JoinPath(outputDir, L"sample.pdf"), pdfBytes);

    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        if (!BuildNativePdfBytes(markdown, style, margin, pdfBytes, inputPath)) {
            return 10 + GetNativePdfLastError();
        }
        totalBytes += pdfBytes.size();
    }

    QueryPerformanceCounter(&end);
    double totalMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    double avgMs = totalMs / (double)iterations;

    std::ostringstream report;
    report << "input_bytes=" << markdown.size() << "\n";
    report << "iterations=" << iterations << "\n";
    report << "total_ms=" << TinyPdf::F(totalMs) << "\n";
    report << "avg_ms=" << TinyPdf::F(avgMs) << "\n";
    report << "avg_pdf_bytes=" << (totalBytes / (size_t)iterations) << "\n";
    report << "path=" << (TinyPdf::IsAsciiDocument(markdown) ? "standard-font-ascii" : "unicode-embedded-font") << "\n";

    if (!WriteUtf8File(JoinPath(outputDir, L"bench-results.txt"), report.str())) return 12;
    return 0;
}

int TryCommandLineExport() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;

    if (argc >= 2 && (lstrcmpiW(argv[1], L"--version") == 0 || lstrcmpiW(argv[1], L"-v") == 0)) {
        WriteStdoutLine(std::string("fast-markdown-imgui ") + FAST_MARKDOWN_VERSION);
        LocalFree(argv);
        return 0;
    }

    if (argc < 2 || (lstrcmpiW(argv[1], L"--export") != 0 &&
        lstrcmpiW(argv[1], L"--batch") != 0 && lstrcmpiW(argv[1], L"--stdin-batch") != 0 &&
        lstrcmpiW(argv[1], L"--serve") != 0 && lstrcmpiW(argv[1], L"--bench") != 0)) {
        LocalFree(argv);
        return -1;
    }

    if (argc < 4 && lstrcmpiW(argv[1], L"--stdin-batch") != 0 && lstrcmpiW(argv[1], L"--serve") != 0) {
        LocalFree(argv);
        return 2;
    }

    int engine = 0;
    int style = 0;
    int margin = 1;
    ParseExportOptions(argc, argv, 4, engine, style, margin);

    if (lstrcmpiW(argv[1], L"--stdin-batch") == 0) {
        if (argc < 3) {
            LocalFree(argv);
            return 2;
        }
        style = 0;
        margin = 1;
        ParseExportOptions(argc, argv, 3, engine, style, margin);
        int result = RunStdinBatchExport(argv[2], engine, style, margin);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--serve") == 0) {
        if (argc < 3) {
            LocalFree(argv);
            return 2;
        }
        style = 0;
        margin = 1;
        ParseExportOptions(argc, argv, 3, engine, style, margin);
        int result = RunServeExport(argv[2], engine, style, margin);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--bench") == 0) {
        if (argc < 5) {
            LocalFree(argv);
            return 2;
        }
        style = 0;
        margin = 1;
        ParseExportOptions(argc, argv, 5, engine, style, margin);
        int iterations = _wtoi(argv[4]);
        int result = RunNativeBench(argv[2], argv[3], iterations, style, margin);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--batch") == 0) {
        int result = RunBatchExport(argv[2], argv[3], engine, style, margin);
        LocalFree(argv);
        return result;
    }

    std::wstring inputPath = argv[2];
    std::wstring outputPath = argv[3];
    if (outputPath.size() < 4 || _wcsicmp(outputPath.c_str() + outputPath.size() - 4, L".pdf") != 0) {
        outputPath += L".pdf";
    }

    bool ok = false;
    if (engine == 0) {
        std::string markdown;
        if (!ReadUtf8File(inputPath, markdown)) {
            LocalFree(argv);
            return 3;
        }
        ok = ExportNativePdf(markdown, outputPath, style, margin, inputPath);
    } else {
        ok = RunPandoc(inputPath, outputPath, style, margin);
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
    std::wstring sourcePath;
    std::wstring outputPath;
    int engineIdx;
    int styleIdx;
    int marginIdx;
    bool openAfter;
    HWND hWnd;
};

struct ExportResult {
    bool ok = false;
    double elapsedMs = 0.0;
};

DWORD WINAPI ExportThread(LPVOID lp) {
    auto* p = (ExportParams*)lp;
    bool ok = false;
    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    if (p->engineIdx == 0) {
        ok = ExportNativePdf(p->markdown, p->outputPath, p->styleIdx, p->marginIdx, p->sourcePath);
    } else {
        std::wstring tmp = GetTempFilePath();
        ok = WriteUtf8File(tmp, p->markdown) && RunPandoc(tmp, p->outputPath, p->styleIdx, p->marginIdx);
        DeleteFileW(tmp.c_str());
    }
    QueryPerformanceCounter(&end);
    double elapsedMs = freq.QuadPart > 0
        ? (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart
        : 0.0;
    
    if (ok) {
        if (p->openAfter) {
            ShellExecuteW(nullptr, L"open", p->outputPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    
    auto* result = new ExportResult{ ok, elapsedMs };
    PostMessageW(p->hWnd, WM_FAST_MARKDOWN_EXPORT_DONE, 0, (LPARAM)result);
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
    
    int marginSetting = g_selectedMargin == 3
        ? 1000 + (int)(std::max(0.25f, std::min(2.0f, g_customMarginInches)) * 72.0f + 0.5f)
        : g_selectedMargin;
    auto* params = new ExportParams{ g_markdownText, g_currentMarkdownPath, outPath, g_selectedEngine, g_selectedStyle, marginSetting, g_openAfterExport, hWnd };
    HANDLE hThread = CreateThread(nullptr, 0, ExportThread, params, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

struct FileLoadParams {
    std::wstring path;
    HWND hWnd;
};

struct FileLoadResult {
    bool ok = false;
    std::wstring path;
    std::string content;
    MarkdownStats stats;
    double elapsedMs = 0.0;
};

DWORD WINAPI FileLoadThread(LPVOID lp) {
    auto* p = (FileLoadParams*)lp;
    auto* result = new FileLoadResult();

    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    result->path = p->path;
    result->ok = ReadUtf8File(p->path, result->content);
    if (result->ok) {
        result->stats = CalculateMarkdownStats(result->content);
    }

    QueryPerformanceCounter(&end);
    result->elapsedMs = freq.QuadPart > 0
        ? (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart
        : 0.0;

    PostMessageW(p->hWnd, WM_FAST_MARKDOWN_FILE_LOADED, 0, (LPARAM)result);
    delete p;
    return 0;
}

void LoadMarkdownFileAsync(HWND hWnd, const std::wstring& path) {
    if (g_isFileLoading.exchange(true)) {
        g_statusText = "Still loading the previous file.";
        return;
    }

    g_statusText = "Loading file...";
    auto* params = new FileLoadParams{ path, hWnd };
    HANDLE hThread = CreateThread(nullptr, 0, FileLoadThread, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        g_isFileLoading = false;
        g_statusText = "File load failed.";
    }
}

// ============================================================================
// ImGui Theme
// ============================================================================
void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    colors[ImGuiCol_Text]                   = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.46f, 0.51f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.04f, 0.05f, 0.07f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.10f, 0.14f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.25f, 0.33f, 0.72f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.14f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.05f, 0.06f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.05f, 0.06f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.05f, 0.06f, 0.08f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.08f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.04f, 0.05f, 0.07f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.20f, 0.25f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.28f, 0.35f, 0.45f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.36f, 0.45f, 0.57f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.39f, 0.86f, 0.95f, 0.92f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.49f, 0.92f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.14f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.12f, 0.16f, 0.21f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.18f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.22f, 0.30f, 0.39f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.20f, 0.25f, 0.33f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.16f, 0.21f, 0.28f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.18f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.15f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.15f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.67f, 0.75f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.63f, 0.42f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.95f, 0.75f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.84f, 0.48f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.39f, 0.86f, 0.95f, 0.28f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.96f, 0.73f, 0.35f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.39f, 0.86f, 0.95f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    
    style.WindowRounding = 0.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 999.0f;
    style.GrabRounding = 999.0f;
    style.TabRounding = 8.0f;
    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(12, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.DisplaySafeAreaPadding = ImVec2(0, 0);
}

static ImVec4 Rgba(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static ImU32 Rgba32(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

static void DrawSectionTitle(ImDrawList* draw, const char* text, ImVec2 pos) {
    ImFont* font = g_fontStrong ? g_fontStrong : ImGui::GetFont();
    draw->AddText(font, font->LegacySize, pos, Rgba32(218, 226, 236), text);
}

static bool DrawQuietChip(const char* id, const char* label, bool selected, ImVec2 size) {
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImU32 bg = selected ? Rgba32(20, 44, 52) : (hovered ? Rgba32(23, 29, 39) : Rgba32(14, 18, 25));
    ImU32 border = selected ? Rgba32(84, 205, 229, 190) : (hovered ? Rgba32(70, 82, 104, 170) : Rgba32(42, 51, 68, 150));
    ImU32 text = selected ? Rgba32(228, 250, 255) : Rgba32(154, 166, 182);

    draw->AddRectFilled(min, max, bg, 8.0f);
    draw->AddRect(min, max, border, 8.0f);
    if (selected) {
        draw->AddCircleFilled(ImVec2(min.x + 10.0f, min.y + size.y * 0.5f), 3.0f, Rgba32(93, 220, 242));
    }

    ImVec2 textSize = ImGui::CalcTextSize(label);
    float x = min.x + (size.x - textSize.x) * 0.5f + (selected ? 5.0f : 0.0f);
    draw->AddText(ImVec2(x, min.y + (size.y - textSize.y) * 0.5f - 1.0f), text, label);
    ImGui::PopID();
    return pressed;
}

static bool DrawOptionTile(const char* id, const char* title, const char* detail, bool selected, ImVec2 size) {
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("tile", size);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImU32 bg = selected ? Rgba32(17, 31, 38) : (hovered ? Rgba32(16, 21, 30) : Rgba32(12, 16, 23));
    ImU32 border = selected ? Rgba32(84, 205, 229, 180) : Rgba32(39, 48, 64, hovered ? 210 : 150);

    draw->AddRectFilled(min, max, bg, 10.0f);
    draw->AddRect(min, max, border, 10.0f);
    if (selected) {
        draw->AddRectFilled(ImVec2(min.x, min.y + 8.0f), ImVec2(min.x + 3.0f, max.y - 8.0f), Rgba32(94, 218, 242), 3.0f);
    }

    ImFont* font = g_fontStrong ? g_fontStrong : ImGui::GetFont();
    draw->AddText(font, font->LegacySize, ImVec2(min.x + 14.0f, min.y + 9.0f), Rgba32(225, 233, 242), title);
    draw->AddText(ImVec2(min.x + 14.0f, min.y + 31.0f), Rgba32(122, 136, 153), detail);
    ImGui::PopID();
    return pressed;
}

static bool DrawSwitch(const char* id, bool* value) {
    ImGui::PushID(id);
    ImVec2 size(46.0f, 26.0f);
    bool pressed = ImGui::InvisibleButton("switch", size);
    if (pressed) *value = !*value;
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImU32 bg = *value ? Rgba32(36, 139, 162) : (hovered ? Rgba32(38, 47, 62) : Rgba32(26, 32, 43));
    draw->AddRectFilled(min, max, bg, 999.0f);
    draw->AddRect(min, max, *value ? Rgba32(94, 218, 242, 160) : Rgba32(62, 73, 94, 160), 999.0f);
    float knobX = *value ? max.x - 21.0f : min.x + 5.0f;
    draw->AddCircleFilled(ImVec2(knobX + 8.0f, min.y + 13.0f), 8.0f, Rgba32(235, 244, 248));
    ImGui::PopID();
    return pressed;
}

static bool DrawActionButton(const char* id, const char* label, ImVec2 size, bool disabled) {
    ImGui::PushID(id);
    if (disabled) ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton("action", size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (disabled) ImGui::EndDisabled();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImU32 bg = disabled ? Rgba32(31, 39, 50) : (active ? Rgba32(31, 133, 159) : (hovered ? Rgba32(69, 188, 216) : Rgba32(51, 163, 191)));
    draw->AddRectFilled(min, max, bg, 12.0f);
    draw->AddRectFilled(min, ImVec2(max.x, min.y + size.y * 0.45f), disabled ? Rgba32(255, 255, 255, 10) : Rgba32(255, 255, 255, 22), 12.0f, ImDrawFlags_RoundCornersTop);
    draw->AddRect(min, max, disabled ? Rgba32(71, 82, 100, 120) : Rgba32(132, 232, 248, 150), 12.0f);

    ImFont* font = g_fontStrong ? g_fontStrong : ImGui::GetFont();
    ImVec2 textSize = font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0.0f, label);
    draw->AddText(font, font->LegacySize, ImVec2(min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f - 1.0f), disabled ? Rgba32(135, 148, 164) : Rgba32(3, 18, 24), label);
    ImGui::PopID();
    return pressed && !disabled;
}

// ============================================================================
// Window Procedure
// ============================================================================
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_DROPFILES &&
        msg != WM_FAST_MARKDOWN_EXPORT_DONE &&
        msg != WM_FAST_MARKDOWN_FILE_LOADED &&
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
    case WM_FAST_MARKDOWN_EXPORT_DONE: {
        auto* result = (ExportResult*)lParam;
        if (result) {
            g_statusText = result->ok
                ? "Export complete in " + FormatDurationMs(result->elapsedMs) + "."
                : "Export failed after " + FormatDurationMs(result->elapsedMs) + ".";
            delete result;
        }
        g_isExporting = false;
        g_forcedRenderFrames = 2;
        return 0;
    }
    case WM_FAST_MARKDOWN_FILE_LOADED: {
        auto* result = (FileLoadResult*)lParam;
        if (result) {
            if (result->ok) {
                g_currentMarkdownPath = std::move(result->path);
                g_markdownText = std::move(result->content);
                g_lineCount = result->stats.lines;
                g_wordCount = result->stats.words;
                g_charCount = result->stats.chars;
                g_statusText = "File loaded in " + FormatDurationMs(result->elapsedMs) + ".";
            } else {
                g_statusText = "File load failed.";
            }
            delete result;
        }
        g_isFileLoading = false;
        g_forcedRenderFrames = 2;
        return 0;
    }
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
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);
        SetActiveWindow(hWnd);
        SetFocus(hWnd);

        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)wParam, 0, path, MAX_PATH)) {
            std::wstring p(path);
            if (p.size() > 3 && _wcsicmp(p.c_str() + p.size() - 3, L".md") == 0) {
                LoadMarkdownFileAsync(hWnd, p);
                g_forcedRenderFrames = 2;
            } else {
                g_statusText = "Drop a .md file.";
                g_forcedRenderFrames = 2;
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
        nullptr, nullptr, hInstance, nullptr);
    DragAcceptFiles(hWnd, TRUE);
    
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
    
    g_fontBody = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    g_fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 28.0f);
    g_fontStrong = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 18.0f);
    g_fontEditor = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\CascadiaMono.ttf", 16.0f);
    if (!g_fontBody) g_fontBody = io.Fonts->AddFontDefault();
    io.FontDefault = g_fontBody;
    
    SetupImGuiStyle();
    
    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    
    // Reserve buffer for text
    g_markdownText.reserve(1024 * 1024); // 1MB
    UpdateMarkdownStats();
    
    // Main loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        if (GetForegroundWindow() != hWnd && !g_isExporting.load() && !g_isFileLoading.load() && g_forcedRenderFrames.load() <= 0) {
            WaitMessage();
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

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 win = ImGui::GetWindowPos();
        const float pad = width < 760.0f ? 14.0f : 18.0f;
        const float gap = 14.0f;
        const float headerH = 76.0f;
        const float bodyTop = headerH + 12.0f;
        const float statusH = 34.0f;
        float sidebarW = std::min(296.0f, std::max(252.0f, width * 0.30f));
        float editorW = width - pad * 2.0f - gap - sidebarW;
        if (editorW < 430.0f) {
            sidebarW = std::max(230.0f, width - pad * 2.0f - gap - 430.0f);
            editorW = width - pad * 2.0f - gap - sidebarW;
        }
        const float editorBottom = height - pad - statusH - 10.0f;
        const float editorH = std::max(140.0f, editorBottom - bodyTop);

        draw->AddRectFilled(win, ImVec2(win.x + width, win.y + height), Rgba32(7, 9, 13));
        draw->AddRectFilled(win, ImVec2(win.x + width, win.y + headerH), Rgba32(9, 12, 18));
        draw->AddLine(ImVec2(win.x, win.y + headerH), ImVec2(win.x + width, win.y + headerH), Rgba32(31, 39, 53, 210));

        ImFont* strongFont = g_fontStrong ? g_fontStrong : ImGui::GetFont();
        float strongSize = strongFont->LegacySize;
        ImFont* titleFont = g_fontTitle ? g_fontTitle : ImGui::GetFont();
        float titleSizePx = titleFont->LegacySize;

        ImVec2 titlePos(win.x + pad + 10.0f, win.y + 16.0f);
        draw->AddRectFilled(ImVec2(win.x + pad, win.y + 22.0f), ImVec2(win.x + pad + 3.0f, win.y + 56.0f), Rgba32(94, 218, 242), 2.0f);
        draw->AddText(titleFont, titleSizePx, titlePos, Rgba32(238, 244, 249), "Fast Markdown");
        char headerDetail[160];
        snprintf(headerDetail, sizeof(headerDetail), "%s / %s / %s", g_engines[g_selectedEngine], g_styles[g_selectedStyle], g_margins[g_selectedMargin]);
        draw->AddText(ImVec2(titlePos.x, titlePos.y + 35.0f), Rgba32(126, 139, 155), headerDetail);

        char headerStats[128];
        snprintf(headerStats, sizeof(headerStats), "%d lines   %d words   %d chars", g_lineCount, g_wordCount, g_charCount);
        ImVec2 headerStatsSize = ImGui::CalcTextSize(headerStats);
        draw->AddText(ImVec2(win.x + width - pad - sidebarW - gap - headerStatsSize.x, win.y + 38.0f), Rgba32(112, 125, 142), headerStats);
        
        // Keyboard shortcut
        if (ImGui::IsKeyPressed(ImGuiKey_E) && io.KeyCtrl && !g_isExporting.load() && !g_isFileLoading.load()) {
            DoExport(hWnd);
        }

        ImVec2 editorMin(win.x + pad, win.y + bodyTop);
        ImVec2 editorMax(editorMin.x + editorW, editorMin.y + editorH);
        const float editorHeaderH = 42.0f;
        draw->AddRectFilled(ImVec2(editorMin.x, editorMin.y + 6.0f), ImVec2(editorMax.x, editorMax.y + 6.0f), Rgba32(0, 0, 0, 70), 16.0f);
        draw->AddRectFilled(editorMin, editorMax, Rgba32(10, 13, 19), 16.0f);
        draw->AddRect(editorMin, editorMax, Rgba32(39, 49, 66, 190), 16.0f);
        draw->AddRectFilled(editorMin, ImVec2(editorMax.x, editorMin.y + editorHeaderH), Rgba32(13, 17, 24), 16.0f, ImDrawFlags_RoundCornersTop);
        draw->AddLine(ImVec2(editorMin.x, editorMin.y + editorHeaderH), ImVec2(editorMax.x, editorMin.y + editorHeaderH), Rgba32(37, 47, 64, 150));
        DrawSectionTitle(draw, "Markdown", ImVec2(editorMin.x + 16.0f, editorMin.y + 11.0f));

        // Draw placeholder if empty
        bool isEmpty = g_markdownText.empty() || g_markdownText[0] == '\0';

        ImGui::SetCursorScreenPos(ImVec2(editorMin.x + 10.0f, editorMin.y + editorHeaderH + 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(7, 10, 15));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(8, 12, 18));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(8, 12, 18));
        ImGui::PushStyleColor(ImGuiCol_Border, Rgba(31, 40, 55, 220));
        if (g_fontEditor) ImGui::PushFont(g_fontEditor);
        if (ImGui::InputTextMultiline("##editor", &g_markdownText,
            ImVec2(std::max(80.0f, editorW - 20.0f), std::max(80.0f, editorH - editorHeaderH - 20.0f)),
            ImGuiInputTextFlags_AllowTabInput)) {
            UpdateMarkdownStats();
        }
        if (g_fontEditor) ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        
        // Draw placeholder on top if empty and not focused
        if (isEmpty && !ImGui::IsItemActive()) {
            ImVec2 pos = ImGui::GetItemRectMin();
            ImFont* editorFont = g_fontEditor ? g_fontEditor : ImGui::GetFont();
            draw->AddText(editorFont, editorFont->LegacySize, ImVec2(pos.x + 14.0f, pos.y + 12.0f), Rgba32(94, 105, 120), "Start typing Markdown, or drop a .md file here.");
        }

        ImVec2 inspectorMin(editorMax.x + gap, editorMin.y);
        ImVec2 inspectorMax(win.x + width - pad, win.y + height - pad);
        draw->AddRectFilled(ImVec2(inspectorMin.x, inspectorMin.y + 6.0f), ImVec2(inspectorMax.x, inspectorMax.y + 6.0f), Rgba32(0, 0, 0, 80), 16.0f);
        draw->AddRectFilled(inspectorMin, inspectorMax, Rgba32(12, 16, 23), 16.0f);
        draw->AddRect(inspectorMin, inspectorMax, Rgba32(42, 52, 70, 185), 16.0f);

        const float panelX = inspectorMin.x + 16.0f;
        const float panelW = inspectorMax.x - inspectorMin.x - 32.0f;
        float panelY = inspectorMin.y + 18.0f;
        DrawSectionTitle(draw, "Export", ImVec2(panelX, panelY));
        draw->AddText(ImVec2(panelX, panelY + 23.0f), Rgba32(117, 131, 149), "PDF settings");
        panelY += 62.0f;

        draw->AddText(ImVec2(panelX, panelY), Rgba32(132, 146, 164), "Engine");
        panelY += 24.0f;
        ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
        if (DrawOptionTile("engine_native", "Native Tiny PDF", "Fast local export", g_selectedEngine == 0, ImVec2(panelW, 58.0f))) g_selectedEngine = 0;
        panelY += 66.0f;
        ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
        if (DrawOptionTile("engine_pandoc", "Pandoc", "Full document pipeline", g_selectedEngine == 1, ImVec2(panelW, 58.0f))) g_selectedEngine = 1;
        panelY += 78.0f;

        draw->AddText(ImVec2(panelX, panelY), Rgba32(132, 146, 164), "Style");
        panelY += 24.0f;
        float styleChipW = (panelW - 10.0f) / 3.0f;
        ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
        if (DrawQuietChip("style_elegant", "Elegant", g_selectedStyle == 0, ImVec2(styleChipW, 34.0f))) g_selectedStyle = 0;
        ImGui::SameLine(0.0f, 5.0f);
        if (DrawQuietChip("style_modern", "Modern", g_selectedStyle == 1, ImVec2(styleChipW, 34.0f))) g_selectedStyle = 1;
        ImGui::SameLine(0.0f, 5.0f);
        if (DrawQuietChip("style_tech", "Tech", g_selectedStyle == 2, ImVec2(styleChipW, 34.0f))) g_selectedStyle = 2;
        panelY += 56.0f;

        draw->AddText(ImVec2(panelX, panelY), Rgba32(132, 146, 164), "Margin");
        panelY += 24.0f;
        float marginChipW = (panelW - 8.0f) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
        if (DrawQuietChip("margin_compact", "Compact", g_selectedMargin == 0, ImVec2(marginChipW, 34.0f))) g_selectedMargin = 0;
        ImGui::SameLine(0.0f, 8.0f);
        if (DrawQuietChip("margin_normal", "Normal", g_selectedMargin == 1, ImVec2(marginChipW, 34.0f))) g_selectedMargin = 1;
        panelY += 42.0f;
        ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
        if (DrawQuietChip("margin_wide", "Wide", g_selectedMargin == 2, ImVec2(marginChipW, 34.0f))) g_selectedMargin = 2;
        ImGui::SameLine(0.0f, 8.0f);
        if (DrawQuietChip("margin_custom", "Custom", g_selectedMargin == 3, ImVec2(marginChipW, 34.0f))) g_selectedMargin = 3;
        panelY += 48.0f;
        if (g_selectedMargin == 3) {
            ImGui::SetCursorScreenPos(ImVec2(panelX, panelY));
            ImGui::SetNextItemWidth(panelW);
            ImGui::SliderFloat("##customMargin", &g_customMarginInches, 0.25f, 2.0f, "%.2fin", ImGuiSliderFlags_AlwaysClamp);
            panelY += 42.0f;
        }

        float actionY = inspectorMax.y - 68.0f;
        float switchY = actionY - 46.0f;
        draw->AddLine(ImVec2(panelX, switchY - 14.0f), ImVec2(panelX + panelW, switchY - 14.0f), Rgba32(39, 49, 66, 160));
        draw->AddText(ImVec2(panelX, switchY + 4.0f), Rgba32(174, 185, 198), "Open after export");
        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 46.0f, switchY));
        DrawSwitch("open_after_export", &g_openAfterExport);

        bool exportDisabled = g_isExporting.load() || g_isFileLoading.load();
        const char* exportLabel = g_isFileLoading.load()
            ? "Loading..."
            : (g_isExporting.load() ? "Exporting..." : "Export PDF");
        ImGui::SetCursorScreenPos(ImVec2(panelX, actionY));
        if (DrawActionButton("export_pdf", exportLabel, ImVec2(panelW, 48.0f), exportDisabled)) {
            DoExport(hWnd);
        }

        ImVec2 statusMin(editorMin.x, editorMax.y + 10.0f);
        ImVec2 statusMax(editorMax.x, statusMin.y + statusH);
        draw->AddRectFilled(statusMin, statusMax, Rgba32(10, 13, 19), 12.0f);
        draw->AddRect(statusMin, statusMax, Rgba32(38, 48, 64, 145), 12.0f);
        bool isBusy = g_isExporting.load() || g_isFileLoading.load();
        ImU32 statusDot = isBusy ? Rgba32(94, 218, 242) : Rgba32(94, 205, 145);
        draw->AddCircleFilled(ImVec2(statusMin.x + 18.0f, statusMin.y + statusH * 0.5f), 4.0f, statusDot);
        draw->AddText(ImVec2(statusMin.x + 30.0f, statusMin.y + 10.0f), Rgba32(168, 180, 194), g_statusText.c_str());

        char stats[128];
        snprintf(stats, sizeof(stats), "%d lines   %d words   %d chars", g_lineCount, g_wordCount, g_charCount);
        ImVec2 statsSize = ImGui::CalcTextSize(stats);
        draw->AddText(ImVec2(statusMax.x - statsSize.x - 16.0f, statusMin.y + 10.0f), Rgba32(122, 134, 150), stats);
        if (isBusy) {
            float progressW = std::min(220.0f, (statusMax.x - statusMin.x) * 0.30f);
            float travel = std::max(1.0f, statusMax.x - statusMin.x - progressW - 12.0f);
            float x = statusMin.x + 6.0f + fmodf((float)ImGui::GetTime() * 180.0f, travel);
            draw->AddRectFilled(ImVec2(x, statusMax.y - 4.0f), ImVec2(x + progressW, statusMax.y - 2.0f), Rgba32(94, 218, 242, 210), 999.0f);
        }
        
        ImGui::End();
        
        // Render
        ImGui::Render();
        const float clear_color[4] = { 0.027f, 0.035f, 0.051f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        g_pSwapChain->Present(1, 0); // Present with vsync
        if (g_forcedRenderFrames.load() > 0) {
            g_forcedRenderFrames--;
        }
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
