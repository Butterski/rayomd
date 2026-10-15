// SPDX-License-Identifier: Apache-2.0

#include "access_policy.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstring>

namespace TinyPdf::Internal {

bool IsUnsafeIpv4Address(uint32_t hostOrder) {
    return (hostOrder >> 24) == 0 ||            // 0.0.0.0/8, "this network"
        (hostOrder >> 24) == 10 ||              // 10.0.0.0/8
        (hostOrder >> 22) == 0x191u ||          // 100.64.0.0/10, carrier-grade NAT (also Tailscale)
        (hostOrder >> 24) == 127 ||             // 127.0.0.0/8, loopback
        (hostOrder >> 16) == 0xA9FEu ||         // 169.254.0.0/16, link-local
        (hostOrder >> 20) == 0xAC1u ||          // 172.16.0.0/12
        (hostOrder >> 8) == 0xC00000u ||        // 192.0.0.0/24, IETF protocol assignments
        (hostOrder >> 16) == 0xC0A8u ||         // 192.168.0.0/16
        (hostOrder >> 17) == 0x6309u ||         // 198.18.0.0/15, benchmarking
        (hostOrder >> 28) >= 0xEu;              // 224.0.0.0/4 multicast, 240.0.0.0/4 reserved and broadcast
}

static uint32_t Ipv4At(const unsigned char* b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

static bool AllZero(const unsigned char* b, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (b[i] != 0) return false;
    }
    return true;
}

bool IsUnsafeIpv6Address(const unsigned char* b) {
    // ::/96 (unspecified, loopback and the deprecated IPv4-compatible ::a.b.c.d), ::ffff:0:0/96
    // (IPv4-mapped) and ::ffff:0:0:0/96 (IPv4-translated): the IPv4 address in the last 4 bytes.
    if (AllZero(b, 10) && ((b[10] == 0 && b[11] == 0) || (b[10] == 0xff && b[11] == 0xff))) {
        return IsUnsafeIpv4Address(Ipv4At(b + 12));
    }
    if (AllZero(b, 8) && b[8] == 0xff && b[9] == 0xff && b[10] == 0 && b[11] == 0) {
        return IsUnsafeIpv4Address(Ipv4At(b + 12));
    }
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
        if (AllZero(b + 4, 8)) return IsUnsafeIpv4Address(Ipv4At(b + 12));   // 64:ff9b::/96, NAT64
        if (b[4] == 0x00 && b[5] == 0x01) return true;                          // 64:ff9b:1::/48, local-use NAT64
    }
    if (b[0] == 0x20 && b[1] == 0x02) return IsUnsafeIpv4Address(Ipv4At(b + 2));   // 2002::/16, 6to4
    return (b[0] & 0xfe) == 0xfc ||             // fc00::/7, unique local
        (b[0] == 0xfe && (b[1] & 0x80) != 0) || // fe80::/10 link-local and fec0::/10 site-local
        b[0] == 0xff;                           // ff00::/8, multicast
}

static bool IsPathSeparator(char c) {
    return c == '\\' || c == '/';
}

static char AsciiUpper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

// CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$ and CONOUT$, in any case, with any extension and
// trailing spaces: Windows opens the device for such a name in any folder.
static bool IsDosDeviceName(std::string_view component) {
    const size_t dot = component.find('.');
    std::string_view name = component.substr(0, dot);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    char upper[8] = {};
    if (name.size() < 3 || name.size() > 7) return false;
    for (size_t i = 0; i < name.size(); i++) upper[i] = AsciiUpper(name[i]);
    const std::string_view word(upper, name.size());
    if (word == "CON" || word == "PRN" || word == "AUX" || word == "NUL" || word == "CONIN$" || word == "CONOUT$") return true;
    return word.size() == 4 && (word.compare(0, 3, "COM") == 0 || word.compare(0, 3, "LPT") == 0) &&
        word[3] >= '0' && word[3] <= '9';
}

bool IsNetworkOrDevicePath(std::string_view path) {
    if (path.empty()) return false;
    // \\server\share, //server/share, \\?\..., \\.\... and the NT prefix \??\...; a path rooted on
    // the current drive (\dir\file) is left to the full path, which IsLocalThemeFile checks.
    if (path.size() >= 2 && IsPathSeparator(path[0]) && IsPathSeparator(path[1])) return true;
    if (path.size() >= 4 && IsPathSeparator(path[0]) && path[1] == '?' && path[2] == '?' && IsPathSeparator(path[3])) {
        return true;
    }
    const bool driveLetter = path.size() >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    if (driveLetter && (path.size() == 2 || !IsPathSeparator(path[2]))) return true;   // C:file
    size_t start = driveLetter ? 3 : 0;
    while (start <= path.size()) {
        size_t end = start;
        while (end < path.size() && !IsPathSeparator(path[end])) end++;
        if (IsDosDeviceName(path.substr(start, end - start))) return true;
        start = end + 1;
    }
    return false;
}

#ifdef _WIN32
static bool IsLocalDriveRoot(wchar_t letter) {
    if (!((letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z'))) return false;
    const wchar_t root[4] = { letter, L':', L'\\', L'\0' };
    const UINT type = GetDriveTypeW(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK;
}

bool IsLocalThemeFile(const std::string& pathUtf8) {
    if (pathUtf8.empty() || pathUtf8.size() > 32767 || std::memchr(pathUtf8.data(), 0, pathUtf8.size()) ||
        IsNetworkOrDevicePath(pathUtf8)) return false;
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8.data(), (int)pathUtf8.size(),
        nullptr, 0);
    if (wideLength <= 0) return false;
    std::wstring wide((size_t)wideLength, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8.data(), (int)pathUtf8.size(), &wide[0], wideLength);

    // A relative path is taken in the current directory, which may be a share: the full path,
    // made without touching the file system, must start with a local drive.
    DWORD fullLength = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (fullLength == 0 || fullLength > 32768) return false;
    std::wstring full((size_t)fullLength, L'\0');
    fullLength = GetFullPathNameW(wide.c_str(), (DWORD)full.size(), &full[0], nullptr);
    if (fullLength == 0 || fullLength >= full.size()) return false;
    full.resize(fullLength);
    if (full.size() < 4 || full[1] != L':' || full[2] != L'\\' || !IsLocalDriveRoot(full[0])) return false;

    HANDLE handle = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool local = GetFileType(handle) == FILE_TYPE_DISK;
    if (local) {
        // Succeeds only for a file a redirector serves (FILE_REMOTE_DEVICE), as through a link.
        FILE_REMOTE_PROTOCOL_INFO remote{};
        local = !GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote));
    }
    if (local) {
        const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
        const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
        std::wstring finalPath(needed > 0 && needed <= 32768 ? (size_t)needed : 0, L'\0');
        const DWORD length = finalPath.empty() ? 0
            : GetFinalPathNameByHandleW(handle, &finalPath[0], (DWORD)finalPath.size(), flags);
        // "\\?\C:\...", never "\\?\UNC\...".
        local = length >= 7 && length < finalPath.size() && finalPath.compare(0, 4, L"\\\\?\\") == 0 &&
            finalPath[5] == L':' && finalPath[6] == L'\\' && IsLocalDriveRoot(finalPath[4]);
    }
    CloseHandle(handle);
    return local;
}
#endif

} // namespace TinyPdf::Internal
