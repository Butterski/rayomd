// SPDX-License-Identifier: Apache-2.0

#include "image_cache.h"

#include "flate.h"
#include "../common/file_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>
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

namespace fs = std::filesystem;

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

fs::path EntryPath(const std::string& directory, const std::string& digest, bool fast) {
    return fs::u8path(directory) / (digest + (fast ? ".fast.rimg" : ".rimg"));
}

// Removes the least recently used entries until the directory holds 3/4 of kDirectoryBytes, and
// temporary files an hour old, which a process that stopped while writing left.
void Trim(const fs::path& directory) {
    struct Entry {
        fs::file_time_type time;
        uintmax_t size;
        fs::path path;
    };
    std::vector<Entry> entries;
    uintmax_t total = 0;
    const auto staleBefore = fs::file_time_type::clock::now() - std::chrono::hours(1);
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        const fs::path& path = it->path();
        const auto time = it->last_write_time(entryError);
        const uintmax_t size = it->file_size(entryError);
        if (entryError) continue;
        if (path.extension() == ".tmp") {
            if (time < staleBefore) fs::remove(path, entryError);
        } else if (path.extension() == ".rimg") {
            entries.push_back({ time, size, path });
            total += size;
        }
    }
    if (total <= kDirectoryBytes) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const Entry& entry : entries) {
        if (total <= kDirectoryBytes / 4 * 3) break;
        if (fs::remove(entry.path, error)) total -= entry.size;
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
    const fs::path path = EntryPath(directory, digest, fast);
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
    std::error_code ignored;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ignored);
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

    std::error_code error;
    const fs::path path = EntryPath(directory, digest, fast);
    // Only its user reads the cache: the directory RayoMD creates is 0700 and each entry 0600, as
    // the images of the documents exported are no one else's (POSIX; Windows inherits the ACL
    // of %LOCALAPPDATA%).
    if (fs::create_directories(path.parent_path(), error)) {
        fs::permissions(path.parent_path(), fs::perms::owner_all, fs::perm_options::replace, error);
    }
    static std::atomic<unsigned> stores{ 0 };
    const unsigned store = stores++;
#ifdef _WIN32
    const unsigned long process = GetCurrentProcessId();
#else
    const unsigned long process = (unsigned long)getpid();
#endif
    fs::path temporary = path;
    temporary.replace_extension("." + std::to_string(process) + "-" + std::to_string(store) + ".tmp");
    if (!RayoMd::File::WriteAll(temporary, file, true)) {
        fs::remove(temporary, error);
        return;
    }
    fs::rename(temporary, path, error);
    if (error) fs::remove(temporary, error);
    if (store % 32 == 0) Trim(path.parent_path());
}

} // namespace TinyPdf::Internal
