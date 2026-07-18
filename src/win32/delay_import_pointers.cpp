// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

extern "C" BOOL WINAPI __wrap_GetOpenFileNameW(LPOPENFILENAMEW);
extern "C" BOOL WINAPI __wrap_GetSaveFileNameW(LPOPENFILENAMEW);
extern "C" HRESULT WINAPI __wrap_DwmEnableBlurBehindWindow(HWND, const DWM_BLURBEHIND*);
extern "C" HRESULT WINAPI __wrap_DwmGetColorizationColor(DWORD*, BOOL*);
extern "C" HRESULT WINAPI __wrap_DwmIsCompositionEnabled(BOOL*);
extern "C" HRESULT WINAPI __wrap_DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD);
extern "C" void WINAPI __wrap_DragAcceptFiles(HWND, BOOL);
extern "C" void WINAPI __wrap_DragFinish(HDROP);
extern "C" UINT WINAPI __wrap_DragQueryFileW(HDROP, UINT, LPWSTR, UINT);
extern "C" HRESULT WINAPI __wrap_SHGetFolderPathW(HWND, int, HANDLE, DWORD, LPWSTR);
extern "C" HINSTANCE WINAPI __wrap_ShellExecuteW(
    HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
extern "C" LPWSTR* WINAPI __wrap_CommandLineToArgvW(LPCWSTR, int*);

// MinGW headers declare these APIs with dllimport, so callers reference the
// import-address pointer directly. Supplying that pointer here preserves the
// usual indirect call while resolving it to the lazy wrapper.
extern "C" {
decltype(&GetOpenFileNameW) __imp_GetOpenFileNameW = __wrap_GetOpenFileNameW;
decltype(&GetSaveFileNameW) __imp_GetSaveFileNameW = __wrap_GetSaveFileNameW;
decltype(&DwmEnableBlurBehindWindow) __imp_DwmEnableBlurBehindWindow = __wrap_DwmEnableBlurBehindWindow;
decltype(&DwmGetColorizationColor) __imp_DwmGetColorizationColor = __wrap_DwmGetColorizationColor;
decltype(&DwmIsCompositionEnabled) __imp_DwmIsCompositionEnabled = __wrap_DwmIsCompositionEnabled;
decltype(&DwmSetWindowAttribute) __imp_DwmSetWindowAttribute = __wrap_DwmSetWindowAttribute;
decltype(&DragAcceptFiles) __imp_DragAcceptFiles = __wrap_DragAcceptFiles;
decltype(&DragFinish) __imp_DragFinish = __wrap_DragFinish;
decltype(&DragQueryFileW) __imp_DragQueryFileW = __wrap_DragQueryFileW;
decltype(&SHGetFolderPathW) __imp_SHGetFolderPathW = __wrap_SHGetFolderPathW;
decltype(&ShellExecuteW) __imp_ShellExecuteW = __wrap_ShellExecuteW;
decltype(&CommandLineToArgvW) __imp_CommandLineToArgvW = __wrap_CommandLineToArgvW;
}
