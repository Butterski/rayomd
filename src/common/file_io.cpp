// SPDX-License-Identifier: Apache-2.0

#include "file_io.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RayoMd::File {

#ifdef _WIN32
intptr_t OpenRegular(const std::filesystem::path& path, uint64_t& size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return (intptr_t)INVALID_HANDLE_VALUE;
    LARGE_INTEGER length = {};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &length) || length.QuadPart < 0) {
        CloseHandle(file);
        return (intptr_t)INVALID_HANDLE_VALUE;
    }
    size = (uint64_t)length.QuadPart;
    return (intptr_t)file;
}
#else
int OpenRegular(const std::filesystem::path& path, uint64_t& size) {
    // Non-blocking, as opening a FIFO without a writer waits for one; made blocking again once
    // fstat says it is a regular file, where O_NONBLOCK means nothing anyway.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return -1;
    struct stat info {};
    const int flags = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 ? ::fcntl(fd, F_GETFL) : -1;
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }
    size = (uint64_t)info.st_size;
    return fd;
}
#endif

std::FILE* Open(const std::filesystem::path& path, const char* mode) {
    char flags[8] = {};
    size_t length = 0;
    while (length + 2 < sizeof(flags) && mode[length]) {
        flags[length] = mode[length];
        length++;
    }
#ifdef _WIN32
    wchar_t wideMode[8] = {};
    for (size_t i = 0; i < length; i++) wideMode[i] = (wchar_t)flags[i];
    if (flags[0] == 'r' && std::strchr(flags, '+') == nullptr) {
        // The handle OpenRegular checked, not inheritable (no security attributes), under a FILE.
        uint64_t size = 0;
        const intptr_t handle = OpenRegular(path, size);
        if (handle == (intptr_t)INVALID_HANDLE_VALUE) return nullptr;
        const int fd = _open_osfhandle(handle, _O_RDONLY | (std::strchr(flags, 'b') ? _O_BINARY : 0));
        if (fd < 0) {
            CloseHandle((HANDLE)handle);
            return nullptr;
        }
        std::FILE* file = _wfdopen(fd, wideMode);
        if (!file) _close(fd);
        return file;
    }
    wideMode[length] = L'N';   // _wfopen: the handle stays out of child processes
    return _wfopen(path.c_str(), wideMode);
#else
    if (flags[0] == 'r' && std::strchr(flags, '+') == nullptr) {
        uint64_t size = 0;
        const int fd = OpenRegular(path, size);
        if (fd < 0) return nullptr;
        std::FILE* file = ::fdopen(fd, flags);
        if (!file) ::close(fd);
        return file;
    }
    flags[length] = 'e';   // glibc: O_CLOEXEC, so the descriptor never leaks into a child process
    return std::fopen(path.c_str(), flags);
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

// `size` bytes of the open file `file` (OpenRegular) into `content`, at most `maximum`; closes it.
template <class Bytes>
static bool ReadOpened(
#ifdef _WIN32
    HANDLE file,
#else
    int fd,
#endif
    uint64_t size, size_t maximum, Bytes& content) {
    bool read = size <= maximum;
    if (read) content.resize((size_t)size);
#ifdef _WIN32
    for (size_t offset = 0; read && offset < (size_t)size;) {
        DWORD got = 0;
        const DWORD chunk = (DWORD)std::min<uint64_t>(size - offset, 1u << 20);
        read = ReadFile(file, &content[offset], chunk, &got, nullptr) && got != 0;
        offset += got;
    }
    CloseHandle(file);
#else
    // A file that shrinks while it is read fails; one that grows keeps the size fstat said.
    for (size_t offset = 0; read && offset < (size_t)size;) {
        const ssize_t got = ::read(fd, &content[offset], (size_t)size - offset);
        if (got < 0 && errno == EINTR) continue;
        read = got > 0;
        if (read) offset += (size_t)got;
    }
    ::close(fd);
#endif
    return read;
}

template <class Bytes>
static bool ReadAllInto(const std::filesystem::path& path, size_t maximum, Bytes& content) {
    uint64_t size = 0;
#ifdef _WIN32
    const intptr_t handle = OpenRegular(path, size);
    if (handle == (intptr_t)INVALID_HANDLE_VALUE) return false;
    return ReadOpened((HANDLE)handle, size, maximum, content);
#else
    const int fd = OpenRegular(path, size);
    if (fd < 0) return false;
    return ReadOpened(fd, size, maximum, content);
#endif
}

bool ReadCanonical(const std::filesystem::path& canonical, size_t maximum, std::vector<uint8_t>& content) {
#ifdef __linux__
    uint64_t size = 0;
    const int fd = OpenRegular(canonical, size);
    if (fd < 0) return false;
    char link[32];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const std::string& expected = canonical.native();
    // One byte more than expected: a longer target is another file too.
    std::string target(expected.size() + 1, '\0');
    const ssize_t length = ::readlink(link, &target[0], target.size());
    if (length < 0 || (size_t)length != expected.size() || target.compare(0, expected.size(), expected) != 0) {
        ::close(fd);
        return false;
    }
    return ReadOpened(fd, size, maximum, content);
#else
    return ReadAllInto(canonical, maximum, content);
#endif
}

bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content) {
    return ReadAllInto(path, maximum, content);
}

bool ReadAll(const std::filesystem::path& path, size_t maximum, std::vector<uint8_t>& content) {
    return ReadAllInto(path, maximum, content);
}

bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content, FileIdentity& identity) {
    identity = FileIdentity();
#ifdef _WIN32
    const intptr_t handle = OpenRegular(path, identity.size);
    if (handle == (intptr_t)INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle((HANDLE)handle, &info)) {
        CloseHandle((HANDLE)handle);
        return false;
    }
    identity.volume = info.dwVolumeSerialNumber;
    identity.index = (uint64_t)info.nFileIndexHigh << 32 | info.nFileIndexLow;
    identity.opened = true;
    return ReadOpened((HANDLE)handle, identity.size, maximum, content);
#else
    const int fd = OpenRegular(path, identity.size);
    if (fd < 0) return false;
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return false;
    }
    identity.volume = (uint64_t)info.st_dev;
    identity.index = (uint64_t)info.st_ino;
    identity.opened = true;
    return ReadOpened(fd, identity.size, maximum, content);
#endif
}

bool WriteAll(const std::filesystem::path& path, std::string_view content, [[maybe_unused]] bool ownerOnly) {
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
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, ownerOnly ? 0600 : 0666);
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
