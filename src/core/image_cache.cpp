// SPDX-License-Identifier: Apache-2.0

#include "image_cache.h"

#include "flate.h"
#include "../common/file_io.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

namespace TinyPdf::Internal {
namespace {

using RayoMd::File::Path;
using RayoMd::File::PathView;

// The last two digits are the encoding's generation: raise them with any change to the bytes
// BuildDecodedPngImage writes (the encoder, its image mode, the filter choice), so that no build
// reads another's streams under the same version.
constexpr char kMagic[8] = { 'R', 'M', 'D', 'I', 'M', 'G', '0', '2' };
// Streams of --fast-images' mode (PdfOptions::fastImages), with a generation of their own.
constexpr char kFastMagic[8] = { 'R', 'M', 'D', 'I', 'M', 'F', '0', '1' };
constexpr uintmax_t kDirectoryBytes = uintmax_t(256) * 1024 * 1024;

// The magic, the version's length and text, then width, height, components and the lengths of
// the two streams, each 32 bits big-endian.
size_t HeaderBytes(std::string_view version) {
    return sizeof(kMagic) + 1 + version.size() + 5 * 4;
}

void AppendU32(std::string& out, uint32_t value) {
    const char bytes[4] = { (char)(value >> 24), (char)(value >> 16), (char)(value >> 8), (char)value };
    out.append(bytes, 4);
}

uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

Path EntryPath(const std::string& directory, const std::string& digest, bool fast) {
    Path path = RayoMd::File::PathFromUtf8(directory);
    RayoMd::File::AppendPath(path, RayoMd::File::PathOfAscii(digest + (fast ? ".fast.rimg" : ".rimg")));
    return path;
}

// Whether the file name `name` ends in the extension `extension` (ASCII), as path::extension() reads it.
bool HasExtension(PathView name, std::string_view extension) {
    const PathView found = RayoMd::File::ExtensionOf(name);
    return found.size() == extension.size() && std::equal(found.begin(), found.end(), extension.begin());
}

// Removes the least recently used entries until the directory holds 3/4 of kDirectoryBytes, and
// temporary files an hour old, which a process that stopped while writing left.
void Trim(const Path& directory) {
    struct Entry {
        uint64_t time;
        uintmax_t size;
        Path path;
    };
    std::vector<Entry> entries;
    uintmax_t total = 0;
#ifdef _WIN32
    constexpr uint64_t kHour = 3600ull * 10000000u;   // 100 ns
#else
    constexpr uint64_t kHour = 3600ull * 1000000000u;
#endif
    const uint64_t now = RayoMd::File::FileTimeNow();
    const uint64_t staleBefore = now > kHour ? now - kHour : 0;
    RayoMd::File::ForEachEntry(directory, [&](const RayoMd::File::Entry& entry) {
        Path path = directory;
        RayoMd::File::AppendPath(path, entry.name);
        // A regular file's time and size (through a link, as stat gives them); nothing else has a size.
        RayoMd::File::FileStatus status;
        if (!RayoMd::File::StatusOf(path, status) || status.type != RayoMd::File::FileType::Regular) return true;
        if (HasExtension(entry.name, ".tmp")) {
            if (status.modified < staleBefore) RayoMd::File::Remove(path);
        } else if (HasExtension(entry.name, ".rimg")) {
            entries.push_back({ status.modified, status.size, std::move(path) });
            total += status.size;
        }
        return true;
    });
    if (total <= kDirectoryBytes) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const Entry& entry : entries) {
        if (total <= kDirectoryBytes / 4 * 3) break;
        if (RayoMd::File::Remove(entry.path)) total -= entry.size;
    }
}

} // namespace

bool LoadCachedImage(const std::string& directory, const std::string& digest, uint32_t width, uint32_t height,
    uint32_t components, size_t maxBytes, std::string& color, std::string& mask, bool fast) {
    const std::string_view version = RAYOMD_VERSION;
    const size_t header = HeaderBytes(version);
    const char* const magic = fast ? kFastMagic : kMagic;
    uint8_t head[sizeof(kMagic) + 1 + 255 + 5 * 4];
    if (header > sizeof(head)) return false;
    const Path path = EntryPath(directory, digest, fast);
    std::FILE* const file = RayoMd::File::Open(path, "rb");
    if (file == nullptr) return false;
    // The header, then the two streams straight into `color` and `mask`, then the checksum.
    const long long size = RayoMd::File::SizeOf(file);
    bool ok = size >= 0 && (uint64_t)size >= header + 4 && (uint64_t)size <= (uint64_t)header + maxBytes + 4 &&
        RayoMd::File::SeekTo(file, 0) && std::fread(head, 1, header, file) == header &&
        std::memcmp(head, magic, sizeof(kMagic)) == 0 && head[8] == version.size() &&
        std::memcmp(head + 9, version.data(), version.size()) == 0;
    const uint8_t* const fields = head + 9 + version.size();
    const size_t colorBytes = ok ? ReadU32(fields + 12) : 0;
    const size_t maskBytes = ok ? ReadU32(fields + 16) : 0;
    ok = ok && ReadU32(fields) == width && ReadU32(fields + 4) == height && ReadU32(fields + 8) == components &&
        colorBytes != 0 && colorBytes + maskBytes == (uint64_t)size - header - 4;
    uint8_t tail[4];
    if (ok) {
        color.resize(colorBytes);
        mask.resize(maskBytes);
        ok = std::fread(&color[0], 1, colorBytes, file) == colorBytes &&
            (maskBytes == 0 || std::fread(&mask[0], 1, maskBytes, file) == maskBytes) &&
            std::fread(tail, 1, 4, file) == 4;
    }
    std::fclose(file);
    if (!ok) return false;
    uint32_t adler = RayoMd::Flate::Adler32(head, header);
    adler = RayoMd::Flate::Adler32(reinterpret_cast<const uint8_t*>(color.data()), colorBytes, adler);
    adler = RayoMd::Flate::Adler32(reinterpret_cast<const uint8_t*>(mask.data()), maskBytes, adler);
    if (ReadU32(tail) != adler) return false;
    RayoMd::File::SetWriteTimeNow(path);
    return true;
}

void StoreCachedImage(const std::string& directory, const std::string& digest, uint32_t width, uint32_t height,
    uint32_t components, size_t maxBytes, std::string_view color, std::string_view mask, bool fast) {
    const std::string_view version = RAYOMD_VERSION;
    if (version.size() > 255 || color.size() + mask.size() > maxBytes || color.size() > UINT32_MAX ||
        mask.size() > UINT32_MAX) {
        return;
    }
    std::string file;
    file.reserve(HeaderBytes(version) + color.size() + mask.size() + 4);
    file.append(fast ? kFastMagic : kMagic, sizeof(kMagic));
    file += (char)version.size();
    file += version;
    AppendU32(file, width);
    AppendU32(file, height);
    AppendU32(file, components);
    AppendU32(file, (uint32_t)color.size());
    AppendU32(file, (uint32_t)mask.size());
    file += color;
    file += mask;
    AppendU32(file, RayoMd::Flate::Adler32(reinterpret_cast<const uint8_t*>(file.data()), file.size()));

    const Path path = EntryPath(directory, digest, fast);
    const Path folder(RayoMd::File::ParentOf(PathView(path)));
    // Only its user reads the cache: the directory RayoMD creates is 0700 and each entry 0600, as
    // the images of the documents exported are no one else's (POSIX; Windows inherits the ACL
    // of %LOCALAPPDATA%).
    bool created = false;
    if (RayoMd::File::CreateDirectories(folder, nullptr, &created) && created) RayoMd::File::SetOwnerOnly(folder);
    static std::atomic<unsigned> stores{ 0 };
    const unsigned store = stores++;
#ifdef _WIN32
    const unsigned long process = GetCurrentProcessId();
#else
    const unsigned long process = (unsigned long)getpid();
#endif
    Path temporary = path;
    RayoMd::File::ReplaceExtension(temporary, RayoMd::File::PathOfAscii("." + std::to_string(process) + "-" + std::to_string(store) + ".tmp"));
    if (!RayoMd::File::WriteAll(temporary, file, true)) {
        RayoMd::File::Remove(temporary);
        return;
    }
    if (!RayoMd::File::Rename(temporary, path)) RayoMd::File::Remove(temporary);
    if (store % 32 == 0) Trim(folder);
}

} // namespace TinyPdf::Internal
