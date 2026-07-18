// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <winhttp.h>
#include <ws2tcpip.h>

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

extern "C" HRESULT WINAPI __wrap_CoCreateInstance(
    REFCLSID classId, LPUNKNOWN outer, DWORD context, REFIID interfaceId, LPVOID* object) {
    RAYOMD_DELAY_PROC(L"ole32.dll", CoCreateInstance);
    return procedure ? procedure(classId, outer, context, interfaceId, object)
                     : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_CoInitializeEx(LPVOID reserved, DWORD concurrency) {
    RAYOMD_DELAY_PROC(L"ole32.dll", CoInitializeEx);
    return procedure ? procedure(reserved, concurrency) : MissingModuleResult();
}

extern "C" void WINAPI __wrap_CoUninitialize() {
    RAYOMD_DELAY_PROC(L"ole32.dll", CoUninitialize);
    if (procedure) procedure();
}

extern "C" HRESULT WINAPI __wrap_CreateStreamOnHGlobal(
    HGLOBAL memory, BOOL deleteOnRelease, LPSTREAM* stream) {
    RAYOMD_DELAY_PROC(L"ole32.dll", CreateStreamOnHGlobal);
    return procedure ? procedure(memory, deleteOnRelease, stream) : MissingModuleResult();
}

extern "C" HRESULT WINAPI __wrap_GetHGlobalFromStream(LPSTREAM stream, HGLOBAL* memory) {
    RAYOMD_DELAY_PROC(L"ole32.dll", GetHGlobalFromStream);
    return procedure ? procedure(stream, memory) : MissingModuleResult();
}

extern "C" void WSAAPI __wrap_FreeAddrInfoW(PADDRINFOW addressInfo) {
    RAYOMD_DELAY_PROC(L"ws2_32.dll", FreeAddrInfoW);
    if (procedure) procedure(addressInfo);
}

extern "C" INT WSAAPI __wrap_GetAddrInfoW(
    PCWSTR nodeName, PCWSTR serviceName, const ADDRINFOW* hints, PADDRINFOW* result) {
    RAYOMD_DELAY_PROC(L"ws2_32.dll", GetAddrInfoW);
    return procedure ? procedure(nodeName, serviceName, hints, result) : WSASYSNOTREADY;
}

extern "C" u_long WSAAPI __wrap_ntohl(u_long networkValue) {
    RAYOMD_DELAY_PROC(L"ws2_32.dll", ntohl);
    return procedure ? procedure(networkValue) : networkValue;
}

extern "C" int WSAAPI __wrap_WSAStartup(WORD version, LPWSADATA data) {
    RAYOMD_DELAY_PROC(L"ws2_32.dll", WSAStartup);
    return procedure ? procedure(version, data) : WSASYSNOTREADY;
}

extern "C" BOOL WINAPI __wrap_WinHttpCloseHandle(HINTERNET internet) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpCloseHandle);
    return procedure ? procedure(internet) : FALSE;
}

extern "C" HINTERNET WINAPI __wrap_WinHttpConnect(
    HINTERNET session, LPCWSTR serverName, INTERNET_PORT serverPort, DWORD reserved) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpConnect);
    return procedure ? procedure(session, serverName, serverPort, reserved) : nullptr;
}

extern "C" BOOL WINAPI __wrap_WinHttpCrackUrl(
    LPCWSTR url, DWORD urlLength, DWORD flags, LPURL_COMPONENTS components) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpCrackUrl);
    return procedure ? procedure(url, urlLength, flags, components) : FALSE;
}

extern "C" HINTERNET WINAPI __wrap_WinHttpOpen(
    LPCWSTR userAgent, DWORD accessType, LPCWSTR proxyName,
    LPCWSTR proxyBypass, DWORD flags) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpOpen);
    return procedure ? procedure(userAgent, accessType, proxyName, proxyBypass, flags) : nullptr;
}

extern "C" HINTERNET WINAPI __wrap_WinHttpOpenRequest(
    HINTERNET connection, LPCWSTR verb, LPCWSTR objectName, LPCWSTR version,
    LPCWSTR referrer, LPCWSTR* acceptTypes, DWORD flags) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpOpenRequest);
    return procedure ? procedure(connection, verb, objectName, version, referrer, acceptTypes, flags)
                     : nullptr;
}

extern "C" BOOL WINAPI __wrap_WinHttpQueryDataAvailable(
    HINTERNET request, LPDWORD availableBytes) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpQueryDataAvailable);
    return procedure ? procedure(request, availableBytes) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpQueryHeaders(
    HINTERNET request, DWORD infoLevel, LPCWSTR name, LPVOID buffer,
    LPDWORD bufferLength, LPDWORD index) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpQueryHeaders);
    return procedure ? procedure(request, infoLevel, name, buffer, bufferLength, index) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpReadData(
    HINTERNET request, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpReadData);
    return procedure ? procedure(request, buffer, bytesToRead, bytesRead) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpReceiveResponse(HINTERNET request, LPVOID reserved) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpReceiveResponse);
    return procedure ? procedure(request, reserved) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpSendRequest(
    HINTERNET request, LPCWSTR headers, DWORD headersLength, LPVOID optional,
    DWORD optionalLength, DWORD totalLength, DWORD_PTR context) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpSendRequest);
    return procedure ? procedure(request, headers, headersLength, optional,
        optionalLength, totalLength, context) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpSetOption(
    HINTERNET internet, DWORD option, LPVOID buffer, DWORD bufferLength) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpSetOption);
    return procedure ? procedure(internet, option, buffer, bufferLength) : FALSE;
}

extern "C" BOOL WINAPI __wrap_WinHttpSetTimeouts(
    HINTERNET internet, int resolveTimeout, int connectTimeout,
    int sendTimeout, int receiveTimeout) {
    RAYOMD_DELAY_PROC(L"winhttp.dll", WinHttpSetTimeouts);
    return procedure ? procedure(internet, resolveTimeout, connectTimeout,
        sendTimeout, receiveTimeout) : FALSE;
}
