// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

extern "C" LPWSTR* WINAPI __wrap_CommandLineToArgvW(LPCWSTR, int*);

// MinGW headers declare these APIs with dllimport, so callers reference the
// import-address pointer directly. Supplying that pointer here preserves the
// usual indirect call while resolving it to the lazy wrapper.
extern "C" {
decltype(&CommandLineToArgvW) __imp_CommandLineToArgvW = __wrap_CommandLineToArgvW;
}
