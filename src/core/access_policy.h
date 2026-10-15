// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace TinyPdf::Internal {

// What a URL image may connect to: false for addresses that reach this machine or a private
// network (loopback, private, link-local, carrier-grade NAT, benchmarking, IETF protocol
// assignments, multicast, reserved), for IPv4 in host byte order.
bool IsUnsafeIpv4Address(uint32_t hostOrder);
// The same for the 16 bytes of an IPv6 address: loopback, unspecified, unique-local, link-local,
// site-local, multicast and local-use NAT64, and the IPv4 address that IPv4-mapped, -compatible,
// -translated, NAT64 (64:ff9b::/96) and 6to4 (2002::/16) addresses carry, checked as IPv4.
bool IsUnsafeIpv6Address(const unsigned char* bytes);

// A path in Windows syntax (UTF-8) that leaves the local drives or is no plain file: UNC
// (\\server\share, //server/share), the \\?\, \\.\ and \??\ prefixes, a drive-relative path
// (C:file), and DOS device names (CON, NUL, COM1, ...) as any component. Without touching the
// file system, so it is tested on every platform; only Windows builds apply it, to theme files,
// whose fonts and logo are local files.
bool IsNetworkOrDevicePath(std::string_view path);

#ifdef _WIN32
// A theme's font or logo before it is opened: not IsNetworkOrDevicePath, on a local drive
// (fixed, removable, optical or RAM disk; never a mapped network drive), and, once opened
// without reading it, a disk file whose final path is still on a local drive.
bool IsLocalThemeFile(const std::string& pathUtf8);
#endif

} // namespace TinyPdf::Internal
