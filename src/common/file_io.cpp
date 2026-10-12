#include "file_io.h"

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
    std::FILE* file = Open(path, "wb");
    if (!file) return false;
    bool written = content.empty() || std::fwrite(content.data(), 1, content.size(), file) == content.size();
    if (std::fclose(file) != 0) written = false;
    return written;
}

} // namespace RayoMd::File
