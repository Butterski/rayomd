// Fast Markdown - ImGui + DirectX11 Edition
// Clean modern UI with minimal footprint
// SPDX-License-Identifier: MIT

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
#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <codecvt>
#include <locale>

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
std::string g_statusText = "Ready";
int g_lineCount = 1;
int g_wordCount = 0;
int g_charCount = 0;
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

void UpdateMarkdownStats() {
    g_charCount = (int)g_markdownText.size();
    g_lineCount = 1;
    g_wordCount = 0;

    bool inWord = false;
    for (char c : g_markdownText) {
        if (c == '\n') g_lineCount++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            g_wordCount++;
        }
    }
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

bool ExportNativePdf(const std::string& markdown, const std::wstring& outputPath, int styleIdx, int marginIdx) {
    static thread_local std::string pdfBytes;
    pdfBytes.clear();
    pdfBytes.reserve(1024 * 1024);
    if (!TinyPdf::BuildPdfBytes(markdown, styleIdx, marginIdx, pdfBytes)) {
        return false;
    }
    if (!WriteBinaryFile(outputPath, pdfBytes)) {
        TinyPdf::g_lastError = 2;
        return false;
    }
    return true;
}

bool BuildNativePdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes) {
    return TinyPdf::BuildPdfBytes(markdown, styleIdx, marginIdx, pdfBytes);
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
        return ReadUtf8File(inputPath, markdown) && ExportNativePdf(markdown, outputPath, style, margin);
    }
    return RunPandoc(inputPath, outputPath, style, margin);
}

bool ExportNativeFileWithBuffer(const std::wstring& inputPath, const std::wstring& outputPath, int style, int margin, std::string& pdfBuffer) {
    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) return false;
    pdfBuffer.clear();
    if (!BuildNativePdfBytes(markdown, style, margin, pdfBuffer)) return false;
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
    if (!BuildNativePdfBytes(markdown, style, margin, pdfBytes)) {
        return 10 + GetNativePdfLastError();
    }
    WriteBinaryFile(JoinPath(outputDir, L"sample.pdf"), pdfBytes);

    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        if (!BuildNativePdfBytes(markdown, style, margin, pdfBytes)) {
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
        ok = ExportNativePdf(markdown, outputPath, style, margin);
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
    std::wstring outputPath;
    int engineIdx;
    int styleIdx;
    int marginIdx;
    bool openAfter;
    HWND hWnd;
};

DWORD WINAPI ExportThread(LPVOID lp) {
    auto* p = (ExportParams*)lp;
    bool ok = false;

    if (p->engineIdx == 0) {
        ok = ExportNativePdf(p->markdown, p->outputPath, p->styleIdx, p->marginIdx);
    } else {
        std::wstring tmp = GetTempFilePath();
        ok = WriteUtf8File(tmp, p->markdown) && RunPandoc(tmp, p->outputPath, p->styleIdx, p->marginIdx);
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
    PostMessageW(p->hWnd, WM_NULL, 0, 0);
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
    auto* params = new ExportParams{ g_markdownText, outPath, g_selectedEngine, g_selectedStyle, marginSetting, g_openAfterExport, hWnd };
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
                    UpdateMarkdownStats();
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

        if (GetForegroundWindow() != hWnd && !g_isExporting.load()) {
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
        
        // Top toolbar
        float toolbarY = ImGui::GetCursorPosY();
        
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Engine");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##engine", &g_selectedEngine, g_engines, IM_ARRAYSIZE(g_engines));

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Style");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        ImGui::Combo("##style", &g_selectedStyle, g_styles, IM_ARRAYSIZE(g_styles));

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Margin");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(105);
        ImGui::Combo("##margin", &g_selectedMargin, g_margins, IM_ARRAYSIZE(g_margins));

        if (g_selectedMargin == 3) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("##customMargin", &g_customMarginInches, 0.25f, 2.0f, "%.2fin", ImGuiSliderFlags_AlwaysClamp);
        }
        
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
        
        if (ImGui::InputTextMultiline("##editor", &g_markdownText,
            ImVec2(-1, editorHeight),
            ImGuiInputTextFlags_AllowTabInput)) {
            UpdateMarkdownStats();
        }
        
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
        
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", g_statusText.c_str());
        ImGui::SameLine(width - 220);
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "%d lines  |  %d words  |  %d chars", g_lineCount, g_wordCount, g_charCount);
        
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
