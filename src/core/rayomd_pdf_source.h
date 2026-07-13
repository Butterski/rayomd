#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoMd::PdfSource {

constexpr size_t kMaxPdfBytes = 256u * 1024u * 1024u;
constexpr size_t kMaxSourceBytes = 32u * 1024u * 1024u;

enum class Status : uint8_t {
    Ok,
    NotReversible,
    UnsupportedProfile,
    CorruptPdf,
    IntegrityMismatch,
    LimitExceeded,
    InvalidUtf8,
};

struct Info {
    std::string profile;
    std::string producerVersion;
    std::string encoding;
    std::string attachmentName;
    size_t sourceBytes = 0;
    bool digestValid = false;
};

struct Result {
    Status status = Status::CorruptPdf;
    Info info;
    std::string source;

    bool Ok() const { return status == Status::Ok; }
};

std::string Sha256Hex(std::string_view input);
bool IsValidUtf8(std::string_view input);
std::string BuildXmpMetadata(std::string_view source, std::string_view producerVersion);
Result Inspect(std::string_view pdfBytes, bool recoverSource);
const char* StatusMessage(Status status);

} // namespace RayoMd::PdfSource
