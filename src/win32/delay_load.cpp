// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

template <typename Function>
Function LoadSystemProcedure(const wchar_t* moduleName, const char* procedureName) {
    HMODULE module = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    return reinterpret_cast<Function>(GetProcAddress(module, procedureName));
}

#define RAYOMD_DELAY_PROC(moduleName, procedureName) \
    static const auto procedure = \
        LoadSystemProcedure<decltype(&procedureName)>(moduleName, #procedureName)

HRESULT MissingModuleResult() {
    return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

} // namespace

extern "C" BOOL WINAPI __wrap_GetOpenFileNameW(LPOPENFILENAMEW dialog) {
    RAYOMD_DELAY_PROC(L"comdlg32.dll", GetOpenFileNameW);
    return procedure ? procedure(dialog) : FALSE;
}

extern "C" BOOL WINAPI __wrap_GetSaveFileNameW(LPOPENFILENAMEW dialog) {
    RAYOMD_DELAY_PROC(L"comdlg32.dll", GetSaveFileNameW);
    return procedure ? procedure(dialog) : FALSE;
}

extern "C" HRESULT WINAPI __wrap_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext) {
    RAYOMD_DELAY_PROC(L"d3d11.dll", D3D11CreateDeviceAndSwapChain);
    return procedure ? procedure(adapter, driverType, software, flags, featureLevels,
        featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel,
        immediateContext) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_D3DCompile(
    LPCVOID sourceData, SIZE_T sourceSize, LPCSTR sourceName,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entryPoint,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    RAYOMD_DELAY_PROC(L"d3dcompiler_47.dll", D3DCompile);
    return procedure ? procedure(sourceData, sourceSize, sourceName, defines, include,
        entryPoint, target, flags1, flags2, code, errors) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_DwmEnableBlurBehindWindow(
    HWND window, const DWM_BLURBEHIND* blur) {
    RAYOMD_DELAY_PROC(L"dwmapi.dll", DwmEnableBlurBehindWindow);
    return procedure ? procedure(window, blur) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_DwmGetColorizationColor(
    DWORD* colorization, BOOL* opaqueBlend) {
    RAYOMD_DELAY_PROC(L"dwmapi.dll", DwmGetColorizationColor);
    return procedure ? procedure(colorization, opaqueBlend) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_DwmIsCompositionEnabled(BOOL* enabled) {
    RAYOMD_DELAY_PROC(L"dwmapi.dll", DwmIsCompositionEnabled);
    return procedure ? procedure(enabled) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_DwmSetWindowAttribute(
    HWND window, DWORD attribute, LPCVOID value, DWORD valueSize) {
    RAYOMD_DELAY_PROC(L"dwmapi.dll", DwmSetWindowAttribute);
    return procedure ? procedure(window, attribute, value, valueSize) : MissingModuleResult();
}

extern "C" void WINAPI __wrap_DragAcceptFiles(HWND window, BOOL accept) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragAcceptFiles);
    if (procedure) procedure(window, accept);
}

extern "C" void WINAPI __wrap_DragFinish(HDROP drop) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragFinish);
    if (procedure) procedure(drop);
}

extern "C" UINT WINAPI __wrap_DragQueryFileW(
    HDROP drop, UINT fileIndex, LPWSTR fileName, UINT fileNameSize) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragQueryFileW);
    return procedure ? procedure(drop, fileIndex, fileName, fileNameSize) : 0;
}

extern "C" HRESULT WINAPI __wrap_SHGetFolderPathW(
    HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path) {
    RAYOMD_DELAY_PROC(L"shell32.dll", SHGetFolderPathW);
    return procedure ? procedure(owner, folder, token, flags, path) : MissingModuleResult();
}

extern "C" HINSTANCE WINAPI __wrap_ShellExecuteW(
    HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters,
    LPCWSTR directory, INT showCommand) {
    RAYOMD_DELAY_PROC(L"shell32.dll", ShellExecuteW);
    return procedure ? procedure(owner, operation, file, parameters, directory, showCommand)
                     : reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_DLLNOTFOUND));
}
