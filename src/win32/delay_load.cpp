// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace {

template <typename Function>
Function LoadSystemProcedure(const wchar_t* moduleName, const char* procedureName) {
    HMODULE module = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    // Through void (*)(), which -Wcast-function-type accepts as any function type.
    return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(module, procedureName)));
}

#define RAYOMD_DELAY_PROC(moduleName, procedureName) \
    static const auto procedure = \
        LoadSystemProcedure<decltype(&procedureName)>(moduleName, #procedureName)

} // namespace

extern "C" LPWSTR* WINAPI __wrap_CommandLineToArgvW(LPCWSTR commandLine, int* argumentCount) {
    RAYOMD_DELAY_PROC(L"shell32.dll", CommandLineToArgvW);
    return procedure ? procedure(commandLine, argumentCount) : nullptr;
}
