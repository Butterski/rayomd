// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <winhttp.h>
#include <ws2tcpip.h>

extern "C" HRESULT WINAPI __wrap_CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
extern "C" HRESULT WINAPI __wrap_CoInitializeEx(LPVOID, DWORD);
extern "C" void WINAPI __wrap_CoUninitialize();
extern "C" HRESULT WINAPI __wrap_CreateStreamOnHGlobal(HGLOBAL, BOOL, LPSTREAM*);
extern "C" HRESULT WINAPI __wrap_GetHGlobalFromStream(LPSTREAM, HGLOBAL*);
extern "C" void WSAAPI __wrap_FreeAddrInfoW(PADDRINFOW);
extern "C" INT WSAAPI __wrap_GetAddrInfoW(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
extern "C" u_long WSAAPI __wrap_ntohl(u_long);
extern "C" int WSAAPI __wrap_WSAStartup(WORD, LPWSADATA);
extern "C" BOOL WINAPI __wrap_WinHttpCloseHandle(HINTERNET);
extern "C" HINTERNET WINAPI __wrap_WinHttpConnect(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
extern "C" BOOL WINAPI __wrap_WinHttpCrackUrl(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
extern "C" HINTERNET WINAPI __wrap_WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
extern "C" HINTERNET WINAPI __wrap_WinHttpOpenRequest(
    HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
extern "C" BOOL WINAPI __wrap_WinHttpQueryDataAvailable(HINTERNET, LPDWORD);
extern "C" BOOL WINAPI __wrap_WinHttpQueryHeaders(
    HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
extern "C" BOOL WINAPI __wrap_WinHttpReadData(HINTERNET, LPVOID, DWORD, LPDWORD);
extern "C" BOOL WINAPI __wrap_WinHttpReceiveResponse(HINTERNET, LPVOID);
extern "C" BOOL WINAPI __wrap_WinHttpSendRequest(
    HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
extern "C" BOOL WINAPI __wrap_WinHttpSetOption(HINTERNET, DWORD, LPVOID, DWORD);
extern "C" BOOL WINAPI __wrap_WinHttpSetTimeouts(HINTERNET, int, int, int, int);

extern "C" {
decltype(&CoCreateInstance) __imp_CoCreateInstance = __wrap_CoCreateInstance;
decltype(&CoInitializeEx) __imp_CoInitializeEx = __wrap_CoInitializeEx;
decltype(&CoUninitialize) __imp_CoUninitialize = __wrap_CoUninitialize;
decltype(&CreateStreamOnHGlobal) __imp_CreateStreamOnHGlobal = __wrap_CreateStreamOnHGlobal;
decltype(&GetHGlobalFromStream) __imp_GetHGlobalFromStream = __wrap_GetHGlobalFromStream;
decltype(&FreeAddrInfoW) __imp_FreeAddrInfoW = __wrap_FreeAddrInfoW;
decltype(&GetAddrInfoW) __imp_GetAddrInfoW = __wrap_GetAddrInfoW;
decltype(&ntohl) __imp_ntohl = __wrap_ntohl;
decltype(&WSAStartup) __imp_WSAStartup = __wrap_WSAStartup;
decltype(&WinHttpCloseHandle) __imp_WinHttpCloseHandle = __wrap_WinHttpCloseHandle;
decltype(&WinHttpConnect) __imp_WinHttpConnect = __wrap_WinHttpConnect;
decltype(&WinHttpCrackUrl) __imp_WinHttpCrackUrl = __wrap_WinHttpCrackUrl;
decltype(&WinHttpOpen) __imp_WinHttpOpen = __wrap_WinHttpOpen;
decltype(&WinHttpOpenRequest) __imp_WinHttpOpenRequest = __wrap_WinHttpOpenRequest;
decltype(&WinHttpQueryDataAvailable) __imp_WinHttpQueryDataAvailable = __wrap_WinHttpQueryDataAvailable;
decltype(&WinHttpQueryHeaders) __imp_WinHttpQueryHeaders = __wrap_WinHttpQueryHeaders;
decltype(&WinHttpReadData) __imp_WinHttpReadData = __wrap_WinHttpReadData;
decltype(&WinHttpReceiveResponse) __imp_WinHttpReceiveResponse = __wrap_WinHttpReceiveResponse;
decltype(&WinHttpSendRequest) __imp_WinHttpSendRequest = __wrap_WinHttpSendRequest;
decltype(&WinHttpSetOption) __imp_WinHttpSetOption = __wrap_WinHttpSetOption;
decltype(&WinHttpSetTimeouts) __imp_WinHttpSetTimeouts = __wrap_WinHttpSetTimeouts;
}
