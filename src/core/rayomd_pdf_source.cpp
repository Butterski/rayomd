#include "rayomd_pdf_source.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <vector>

namespace RayoMd::PdfSource {
namespace {

constexpr size_t kMaxObjects = 1000000;
constexpr size_t kMaxMetadataBytes = 16u * 1024u;

uint32_t RotateRight(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

bool CheckedAdd(size_t left, size_t right, size_t& result) {
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool IsWhite(char ch) {
    return ch == '\0' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r' || ch == ' ';
}

bool IsDelimiter(char ch) {
    return IsWhite(ch) || ch == '(' || ch == ')' || ch == '<' || ch == '>' ||
        ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '/' || ch == '%';
}

class Scanner {
public:
    explicit Scanner(std::string_view input, size_t offset = 0) : input_(input), offset_(offset) {}

    size_t Offset() const { return offset_; }

    void SkipSpace() {
        for (;;) {
            while (offset_ < input_.size() && IsWhite(input_[offset_])) offset_++;
            if (offset_ >= input_.size() || input_[offset_] != '%') return;
            while (offset_ < input_.size() && input_[offset_] != '\r' && input_[offset_] != '\n') offset_++;
        }
    }

    bool Consume(std::string_view token) {
        SkipSpace();
        if (input_.substr(offset_, token.size()) != token) return false;
        size_t end = offset_ + token.size();
        if (!token.empty() && !IsDelimiter(token.back()) && end < input_.size() && !IsDelimiter(input_[end])) return false;
        offset_ = end;
        return true;
    }

    bool ReadUnsigned(size_t& value) {
        SkipSpace();
        size_t start = offset_;
        while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') offset_++;
        if (start == offset_) return false;
        auto result = std::from_chars(input_.data() + start, input_.data() + offset_, value);
        return result.ec == std::errc{} && result.ptr == input_.data() + offset_;
    }

    bool ReadName(std::string_view& name) {
        SkipSpace();
        if (offset_ >= input_.size() || input_[offset_] != '/') return false;
        size_t start = ++offset_;
        while (offset_ < input_.size() && !IsDelimiter(input_[offset_])) offset_++;
        if (offset_ - start > 1024) return false;
        name = input_.substr(start, offset_ - start);
        return true;
    }

    bool ReadLiteral(std::string& value) {
        SkipSpace();
        if (offset_ >= input_.size() || input_[offset_] != '(') return false;
        offset_++;
        value.clear();
        unsigned depth = 1;
        while (offset_ < input_.size() && depth != 0) {
            char ch = input_[offset_++];
            if (ch == '\\') {
                if (offset_ >= input_.size()) return false;
                char escaped = input_[offset_++];
                if (escaped == 'n') value.push_back('\n');
                else if (escaped == 'r') value.push_back('\r');
                else if (escaped == 't') value.push_back('\t');
                else if (escaped == 'b') value.push_back('\b');
                else if (escaped == 'f') value.push_back('\f');
                else if (escaped == '\r' || escaped == '\n') {
                    if (escaped == '\r' && offset_ < input_.size() && input_[offset_] == '\n') offset_++;
                } else value.push_back(escaped);
            } else if (ch == '(') {
                if (++depth > 16) return false;
                value.push_back(ch);
            } else if (ch == ')') {
                depth--;
                if (depth != 0) value.push_back(ch);
            } else {
                value.push_back(ch);
            }
            if (value.size() > 64u * 1024u) return false;
        }
        return depth == 0;
    }

    bool SkipToken() {
        SkipSpace();
        if (offset_ >= input_.size()) return false;
        if (input_[offset_] == '(') {
            std::string ignored;
            return ReadLiteral(ignored);
        }
        if (input_[offset_] == '<' && (offset_ + 1 >= input_.size() || input_[offset_ + 1] != '<')) {
            offset_++;
            size_t count = 0;
            while (offset_ < input_.size() && input_[offset_] != '>') {
                if (!IsWhite(input_[offset_]) && ++count > 128u * 1024u) return false;
                offset_++;
            }
            if (offset_ >= input_.size()) return false;
            offset_++;
            return true;
        }
        if (offset_ + 1 < input_.size()) {
            std::string_view pair = input_.substr(offset_, 2);
            if (pair == "<<" || pair == ">>") {
                offset_ += 2;
                return true;
            }
        }
        if (IsDelimiter(input_[offset_])) {
            offset_++;
            return true;
        }
        size_t start = offset_;
        while (offset_ < input_.size() && !IsDelimiter(input_[offset_])) offset_++;
        return offset_ != start && offset_ - start <= 64u * 1024u;
    }

private:
    std::string_view input_;
    size_t offset_ = 0;
};

bool ReadReference(Scanner& scanner, size_t& objectId) {
    size_t generation = 0;
    return scanner.ReadUnsigned(objectId) && objectId > 0 && objectId <= kMaxObjects &&
        scanner.ReadUnsigned(generation) && generation == 0 && scanner.Consume("R");
}

bool FindReference(std::string_view input, std::string_view key, size_t& objectId) {
    Scanner scanner(input);
    for (size_t tokens = 0; tokens < 4096; tokens++) {
        std::string_view name;
        size_t before = scanner.Offset();
        if (scanner.ReadName(name)) {
            if (name == key) {
                size_t candidate = 0;
                Scanner after(input, scanner.Offset());
                if (ReadReference(after, candidate)) {
                    objectId = candidate;
                    return true;
                }
            }
        } else {
            scanner = Scanner(input, before);
            if (!scanner.SkipToken()) break;
        }
    }
    return false;
}

bool HasName(std::string_view input, std::string_view key) {
    Scanner scanner(input);
    for (size_t tokens = 0; tokens < 4096; tokens++) {
        std::string_view name;
        size_t before = scanner.Offset();

        if (scanner.ReadName(name)) {
            if (name == key) return true;
        } else {
            scanner = Scanner(input, before);
            if (!scanner.SkipToken()) break;
        }
    }
    return false;
}

bool FindArrayReference(std::string_view input, std::string_view key, size_t& objectId) {
    Scanner scanner(input);
    for (size_t tokens = 0; tokens < 4096; tokens++) {
        std::string_view name;
        size_t before = scanner.Offset();
        if (scanner.ReadName(name)) {
            if (name == key) {
                Scanner after(input, scanner.Offset());
                return after.Consume("[") && ReadReference(after, objectId);
            }
        } else {
            scanner = Scanner(input, before);
            if (!scanner.SkipToken()) break;
        }
    }
    return false;
}

bool FindLiteralReference(std::string_view input, std::string_view literal, size_t& objectId) {
    Scanner scanner(input);
    bool found = false;
    size_t foundId = 0;
    for (size_t tokens = 0; tokens < 4096; tokens++) {
        std::string value;
        size_t before = scanner.Offset();
        if (scanner.ReadLiteral(value)) {
            if (value == literal) {
                Scanner after(input, scanner.Offset());
                size_t candidate = 0;
                if (!ReadReference(after, candidate) || found) return false;
                found = true;
                foundId = candidate;
            }
        } else {
            scanner = Scanner(input, before);
            if (!scanner.SkipToken()) break;
        }
    }
    objectId = foundId;
    return found;
}

bool FindUnsignedValue(std::string_view input, std::string_view key, size_t& value) {
    Scanner scanner(input);
    for (size_t tokens = 0; tokens < 4096; tokens++) {
        std::string_view name;
        size_t before = scanner.Offset();
        if (scanner.ReadName(name)) {
            if (name == key) {
                Scanner after(input, scanner.Offset());
                if (after.ReadUnsigned(value)) return true;
            }
        } else {
            scanner = Scanner(input, before);
            if (!scanner.SkipToken()) break;
        }
    }
    return false;
}

struct XrefTable {
    std::vector<size_t> offsets;
    size_t xrefOffset = 0;
    size_t rootId = 0;
};

bool ParseXref(std::string_view pdf, XrefTable& result) {
    size_t tailStart = pdf.size() > 4096 ? pdf.size() - 4096 : 0;
    size_t marker = pdf.rfind("startxref", std::string_view::npos);
    if (marker == std::string_view::npos || marker < tailStart) return false;
    Scanner startScanner(pdf, marker + 9);
    if (!startScanner.ReadUnsigned(result.xrefOffset) || result.xrefOffset >= pdf.size()) return false;

    Scanner scanner(pdf, result.xrefOffset);
    size_t first = 0, count = 0;
    if (!scanner.Consume("xref") || !scanner.ReadUnsigned(first) || first != 0 ||
        !scanner.ReadUnsigned(count) || count < 2 || count > kMaxObjects + 1) return false;
    result.offsets.assign(count, 0);
    for (size_t id = 0; id < count; id++) {
        size_t offset = 0, generation = 0;
        if (!scanner.ReadUnsigned(offset) || !scanner.ReadUnsigned(generation)) return false;
        scanner.SkipSpace();
        std::string_view state;
        if (scanner.Consume("f")) state = "f";
        else if (scanner.Consume("n")) state = "n";
        else return false;
        if (id == 0) {
            if (state != "f" || offset != 0 || generation != 65535) return false;
        } else {
            if (state != "n" || generation != 0 || offset == 0 || offset >= result.xrefOffset) return false;
            result.offsets[id] = offset;
        }
    }
    if (!scanner.Consume("trailer")) return false;
    std::string_view trailer = pdf.substr(scanner.Offset(), marker - scanner.Offset());
    if (HasName(trailer, "Prev") || HasName(trailer, "XRefStm") || HasName(trailer, "Encrypt")) return false;
    return FindReference(trailer, "Root", result.rootId) && result.rootId < result.offsets.size();
}

bool ObjectBody(std::string_view pdf, const XrefTable& xref, size_t objectId, std::string_view& body) {
    if (objectId == 0 || objectId >= xref.offsets.size()) return false;
    size_t offset = xref.offsets[objectId];
    if (offset == 0 || offset >= xref.xrefOffset) return false;
    Scanner scanner(pdf, offset);
    size_t parsedId = 0, generation = 0;
    if (!scanner.ReadUnsigned(parsedId) || parsedId != objectId || !scanner.ReadUnsigned(generation) ||
        generation != 0 || !scanner.Consume("obj")) return false;
    size_t end = xref.xrefOffset;
    for (size_t candidate : xref.offsets) {
        if (candidate > offset && candidate < end) end = candidate;
    }
    if (scanner.Offset() >= end) return false;
    body = pdf.substr(scanner.Offset(), end - scanner.Offset());
    return true;
}

bool StreamData(std::string_view object, size_t maximum, std::string_view& data) {
    size_t length = 0;
    if (!FindUnsignedValue(object, "Length", length) || length > maximum) return false;
    size_t marker = object.find("stream");
    if (marker == std::string_view::npos) return false;
    size_t start = marker + 6;
    if (start < object.size() && object[start] == '\r') start++;
    if (start < object.size() && object[start] == '\n') start++;
    else return false;
    size_t end = 0;
    if (!CheckedAdd(start, length, end) || end > object.size()) return false;
    data = object.substr(start, length);
    return true;
}

bool ExtractAttribute(std::string_view xml, std::string_view name, std::string& value) {
    std::string prefix = "rayomd:" + std::string(name) + "=\"";
    size_t start = xml.find(prefix);
    if (start == std::string_view::npos || xml.find(prefix, start + prefix.size()) != std::string_view::npos) return false;
    start += prefix.size();
    size_t end = xml.find('"', start);
    if (end == std::string_view::npos || end - start > 1024) return false;
    value.assign(xml.substr(start, end - start));
    return value.find('&') == std::string::npos && value.find('<') == std::string::npos;
}

bool ParseSize(std::string_view text, size_t& value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ValidateUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count = 0;
        uint32_t codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; codepoint = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; codepoint = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; codepoint = first & 0x07; }
        else return false;
        if (count > text.size() - i) return false;
        for (unsigned j = 0; j < count; j++) {
            unsigned char next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((count == 2 && codepoint < 0x800) || (count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return false;
    }
    return true;
}

std::string XmlEscape(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (char ch : input) {
        if (ch == '&') output += "&amp;";
        else if (ch == '<') output += "&lt;";
        else if (ch == '>') output += "&gt;";
        else if (ch == '"') output += "&quot;";
        else if (ch == '\'') output += "&apos;";
        else output.push_back(ch);
    }
    return output;
}

} // namespace
bool IsValidUtf8(std::string_view input) {
    return ValidateUtf8(input);
}


std::string Sha256Hex(std::string_view input) {
    static constexpr std::array<uint32_t, 64> constants = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
    };
    std::array<uint32_t, 8> state = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    auto transform = [&](const unsigned char* block) {
        uint32_t words[64];
        for (unsigned i = 0; i < 16; i++) words[i] = (uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|(uint32_t(block[i*4+2])<<8)|block[i*4+3];
        for (unsigned i = 16; i < 64; i++) {
            uint32_t a = RotateRight(words[i-15],7)^RotateRight(words[i-15],18)^(words[i-15]>>3);
            uint32_t b = RotateRight(words[i-2],17)^RotateRight(words[i-2],19)^(words[i-2]>>10);
            words[i] = words[i-16] + a + words[i-7] + b;
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (unsigned i = 0; i < 64; i++) {
            uint32_t s1=RotateRight(e,6)^RotateRight(e,11)^RotateRight(e,25), ch=(e&f)^(~e&g);
            uint32_t t1=h+s1+ch+constants[i]+words[i], s0=RotateRight(a,2)^RotateRight(a,13)^RotateRight(a,22);
            uint32_t t2=s0+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    };
    size_t complete = input.size() / 64;
    for (size_t i = 0; i < complete; i++) transform(reinterpret_cast<const unsigned char*>(input.data() + i * 64));
    unsigned char tail[128] = {};
    size_t remaining = input.size() % 64;
    if (remaining) std::memcpy(tail, input.data() + complete * 64, remaining);
    tail[remaining] = 0x80;
    size_t tailBytes = remaining < 56 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(input.size()) * 8u;
    for (unsigned i = 0; i < 8; i++) tail[tailBytes - 1 - i] = static_cast<unsigned char>(bits >> (i * 8));
    transform(tail); if (tailBytes == 128) transform(tail + 64);
    static constexpr char hex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (unsigned i=0;i<8;i++) for(unsigned j=0;j<4;j++){unsigned v=(state[i]>>((3-j)*8))&255;output[i*8+j*2]=hex[v>>4];output[i*8+j*2+1]=hex[v&15];}
    return output;
}

std::string BuildXmpMetadata(std::string_view source, std::string_view producerVersion) {
    std::string output;
    output.reserve(640);
    output += "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmlns:rayomd=\"https://rayomd.dev/ns/source/1.0/\" rayomd:profile=\"rayomd-source/1\" rayomd:producer=\"";
    output += XmlEscape(producerVersion);
    output += "\" rayomd:encoding=\"UTF-8\" rayomd:length=\"" + std::to_string(source.size());
    output += "\" rayomd:sha256=\"" + Sha256Hex(source);
    output += "\" rayomd:attachment=\"source.md\"/></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
    return output;
}

Result Inspect(std::string_view pdf, bool recoverSource) {
    Result result;
    if (pdf.size() > kMaxPdfBytes) { result.status = Status::LimitExceeded; return result; }
    if (pdf.size() < 16 || pdf.substr(0, 5) != "%PDF-") { result.status = Status::CorruptPdf; return result; }
    XrefTable xref;
    if (!ParseXref(pdf, xref)) { result.status = Status::CorruptPdf; return result; }
    std::string_view catalog;
    if (!ObjectBody(pdf, xref, xref.rootId, catalog)) { result.status = Status::CorruptPdf; return result; }
    size_t metadataId = 0;
    if (!FindReference(catalog, "Metadata", metadataId)) { result.status = Status::NotReversible; return result; }
    size_t fileSpecId = 0, associatedId = 0;
    if (!FindLiteralReference(catalog, "source.md", fileSpecId) || !FindArrayReference(catalog, "AF", associatedId) ||
        fileSpecId != associatedId) { result.status = Status::CorruptPdf; return result; }
    if (pdf.substr(0, 8) != "%PDF-2.0") { result.status = Status::UnsupportedProfile; return result; }

    std::string_view metadataObject, xml;
    if (!ObjectBody(pdf, xref, metadataId, metadataObject) || !HasName(metadataObject, "Metadata") ||
        !HasName(metadataObject, "XML") || !StreamData(metadataObject, kMaxMetadataBytes, xml) ||
        xml.find("<!DOCTYPE") != std::string_view::npos || xml.find("<!ENTITY") != std::string_view::npos) {
        result.status = Status::CorruptPdf; return result;
    }
    std::string lengthText, digest;
    if (!ExtractAttribute(xml, "profile", result.info.profile) ||
        !ExtractAttribute(xml, "producer", result.info.producerVersion) ||
        !ExtractAttribute(xml, "encoding", result.info.encoding) ||
        !ExtractAttribute(xml, "length", lengthText) ||
        !ExtractAttribute(xml, "sha256", digest) ||
        !ExtractAttribute(xml, "attachment", result.info.attachmentName)) {
        result.status = Status::CorruptPdf; return result;
    }
    if (result.info.profile != "rayomd-source/1" || result.info.encoding != "UTF-8") {
        result.status = Status::UnsupportedProfile; return result;
    }
    if (result.info.attachmentName != "source.md" || digest.size() != 64 ||
        !ParseSize(lengthText, result.info.sourceBytes)) { result.status = Status::CorruptPdf; return result; }
    if (result.info.sourceBytes > kMaxSourceBytes) { result.status = Status::LimitExceeded; return result; }

    std::string_view fileSpec;
    size_t sourceId = 0;
    if (!ObjectBody(pdf, xref, fileSpecId, fileSpec) || !HasName(fileSpec, "Filespec") ||
        !HasName(fileSpec, "Source") || !FindReference(fileSpec, "F", sourceId)) {
        result.status = Status::CorruptPdf; return result;
    }
    std::string_view sourceObject, source;
    if (!ObjectBody(pdf, xref, sourceId, sourceObject) || !HasName(sourceObject, "EmbeddedFile") ||
        !HasName(sourceObject, "text#2Fmarkdown") || HasName(sourceObject, "Filter") ||
        !StreamData(sourceObject, kMaxSourceBytes, source) || source.size() != result.info.sourceBytes) {
        result.status = source.size() > kMaxSourceBytes ? Status::LimitExceeded : Status::CorruptPdf; return result;
    }
    result.info.digestValid = Sha256Hex(source) == digest;
    if (!result.info.digestValid) { result.status = Status::IntegrityMismatch; return result; }
    if (!ValidateUtf8(source)) { result.status = Status::InvalidUtf8; return result; }
    if (recoverSource) result.source.assign(source.data(), source.size());
    result.status = Status::Ok;
    return result;
}

const char* StatusMessage(Status status) {
    switch (status) {
    case Status::Ok: return "reversible source is intact";
    case Status::NotReversible: return "not a reversible RayoMD PDF";
    case Status::UnsupportedProfile: return "reversible profile version is not supported";
    case Status::CorruptPdf: return "reversible profile or PDF structure is corrupt";
    case Status::IntegrityMismatch: return "embedded source integrity validation failed";
    case Status::LimitExceeded: return "PDF or embedded source exceeds the configured limit";
    case Status::InvalidUtf8: return "embedded source is not valid UTF-8";
    }
    return "unknown recovery error";
}

} // namespace RayoMd::PdfSource
