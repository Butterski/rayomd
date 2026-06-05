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

// ============================================================================
// Export Thread
// ============================================================================
struct ExportParams {
    std::string markdown;
    std::wstring outputPath;
    int styleIdx;
    bool openAfter;
    HWND hWnd;
};

DWORD WINAPI ExportThread(LPVOID lp) {
    auto* p = (ExportParams*)lp;
    std::wstring tmp = GetTempFilePath();
    bool ok = WriteUtf8File(tmp, p->markdown) && RunPandoc(tmp, p->outputPath, p->styleIdx);
    DeleteFileW(tmp.c_str());
    
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
    g_statusText = "Converting...";
    
    auto* params = new ExportParams{ g_markdownText, outPath, g_selectedStyle, g_openAfterExport, hWnd };
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
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Style");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::Combo("##style", &g_selectedStyle, g_styles, IM_ARRAYSIZE(g_styles));
        
        ImGui::SameLine(width - 290);
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
        
        ImGui::InputTextMultiline("##editor", &g_markdownText[0], g_markdownText.capacity(),
            ImVec2(-1, editorHeight),
            ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
            [](ImGuiInputTextCallbackData* data) -> int {
                if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                    std::string* str = (std::string*)data->UserData;
                    str->resize(data->BufTextLen);
                    data->Buf = &(*str)[0];
                }
                return 0;
            }, &g_markdownText);
        
        // Draw placeholder on top if empty and not focused
        if (isEmpty && !ImGui::IsItemActive()) {
            ImVec2 pos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + 10, pos.y + 6),
                IM_COL32(100, 100, 100, 255),
                "Write your Markdown here... (or drag & drop a .md file)"
            );
        }
        
        // Recalculate actual length
        g_markdownText.resize(strlen(g_markdownText.c_str()));
        
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
