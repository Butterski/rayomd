#include "file_io.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RayoMd::File {

std::FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    wchar_t wideMode[8] = {};
    for (size_t i = 0; i + 1 < sizeof(wideMode) / sizeof(wideMode[0]) && mode[i]; i++) wideMode[i] = (wchar_t)mode[i];
    return _wfopen(path.c_str(), wideMode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

long long SizeOf(std::FILE* file) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
    return _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0) return -1;
    return (long long)ftello(file);
#endif
}

bool SeekTo(std::FILE* file, long long offset) {
#ifdef _WIN32
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

template <class Bytes>
static bool ReadAllInto(const std::filesystem::path& path, size_t maximum, Bytes& content) {
    std::FILE* file = Open(path, "rb");
    if (!file) return false;
    const long long size = SizeOf(file);
    bool read = size >= 0 && (unsigned long long)size <= maximum && SeekTo(file, 0);
    if (read) {
        content.resize((size_t)size);
        read = size == 0 || std::fread(&content[0], 1, (size_t)size, file) == (size_t)size;
    }
    std::fclose(file);
    return read;
}

bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content) {
    return ReadAllInto(path, maximum, content);
}

bool ReadAll(const std::filesystem::path& path, size_t maximum, std::vector<uint8_t>& content) {
    return ReadAllInto(path, maximum, content);
}

bool WriteAll(const std::filesystem::path& path, std::string_view content) {
#ifdef _WIN32
    std::FILE* file = Open(path, "wb");
    if (!file) return false;
    bool written = content.empty() || std::fwrite(content.data(), 1, content.size(), file) == content.size();
    if (std::fclose(file) != 0) written = false;
    return written;
#else
    // Overwrites an existing file in place and cuts it to the new size afterwards, instead of
    // fopen("wb")'s O_TRUNC: truncating an ext4 file to zero frees its blocks, and its close then
    // starts writeback (auto_da_alloc), about 0.1 ms per export onto an existing PDF. The cost: no
    // such flush, so a crash can leave old and new bytes mixed, as any in-place overwrite can.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    struct stat st;
    bool written = ::fstat(fd, &st) == 0;
    const char* data = content.data();
    size_t left = content.size();
    while (written && left != 0) {
        const ssize_t count = ::write(fd, data, left);
        if (count > 0) {
            data += count;
            left -= (size_t)count;
        } else if (count == 0 || errno != EINTR) {
            written = false;
        }
    }
    // Only a regular file that was longer has old bytes left past the end; pipes and devices
    // cannot be truncated.
    if (written && S_ISREG(st.st_mode) && st.st_size > (off_t)content.size() &&
        ::ftruncate(fd, (off_t)content.size()) != 0) {
        written = false;
    }
    if (::close(fd) != 0) written = false;
    return written;
#endif
}

Stamp StampOf(const std::filesystem::path& path) {
    Stamp stamp;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return stamp;
    const auto time = [](FILETIME value) { return (uint64_t)value.dwHighDateTime << 32 | value.dwLowDateTime; };
    stamp.size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
    stamp.modified = time(data.ftLastWriteTime);
    stamp.changed = time(data.ftCreationTime);
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return stamp;
#ifdef __APPLE__
    const struct timespec modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
    const struct timespec modified = info.st_mtim, changed = info.st_ctim;
#endif
    const auto time = [](const struct timespec& value) { return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec; };
    stamp.size = (uint64_t)info.st_size;
    stamp.modified = time(modified);
    stamp.changed = time(changed);
    stamp.id = (uint64_t)info.st_ino ^ (uint64_t)info.st_dev << 32;
#endif
    return stamp;
}

} // namespace RayoMd::File
