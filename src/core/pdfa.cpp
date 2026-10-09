#include "pdfa.h"

#include "rayomd_pdf_source.h"
#include "../common/text_utils.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace TinyPdf::Internal {
namespace {

// sRGB as an ICC v4.2 display profile: D50 white, the IEC 61966-2-1 primaries adapted to D50
// (columns balanced to sum to the encoded white), a Bradford D65-to-D50 matrix and the sRGB
// curve as one shared parametric curve. Big-endian words; its profile ID is the MD5 of the
// profile as ICC.1 specifies. SHA-256 7e0e480dcef2bf947b70ee612fc51de4e26aae67f6b30aa79cc35529783cb568.
constexpr uint32_t kSrgbWords[120] = {
    0x000001e0, 0x00000000, 0x04200000, 0x6d6e7472, 0x52474220, 0x58595a20, 0x07ea0001, 0x00010000,
    0x00000000, 0x61637370, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0000f6d6, 0x00010000, 0x0000d32d, 0x00000000, 0xd7022a23, 0xc8617081, 0x733474da,
    0xcc7da7e2, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0000000a, 0x64657363, 0x000000fc, 0x00000024, 0x63707274, 0x00000120, 0x00000022, 0x77747074,
    0x00000144, 0x00000014, 0x63686164, 0x00000158, 0x0000002c, 0x7258595a, 0x00000184, 0x00000014,
    0x6758595a, 0x00000198, 0x00000014, 0x6258595a, 0x000001ac, 0x00000014, 0x72545243, 0x000001c0,
    0x00000020, 0x67545243, 0x000001c0, 0x00000020, 0x62545243, 0x000001c0, 0x00000020, 0x6d6c7563,
    0x00000000, 0x00000001, 0x0000000c, 0x656e5553, 0x00000008, 0x0000001c, 0x00730052, 0x00470042,
    0x6d6c7563, 0x00000000, 0x00000001, 0x0000000c, 0x656e5553, 0x00000006, 0x0000001c, 0x00430043,
    0x00300000, 0x58595a20, 0x00000000, 0x0000f6d6, 0x00010000, 0x0000d32d, 0x73663332, 0x00000000,
    0x00010c40, 0x000005dd, 0xfffff326, 0x00000791, 0x0000fd92, 0xfffffba1, 0xfffffda2, 0x000003dc,
    0x0000c071, 0x58595a20, 0x00000000, 0x00006fa0, 0x000038f2, 0x0000038f, 0x58595a20, 0x00000000,
    0x00006296, 0x0000b789, 0x000018da, 0x58595a20, 0x00000000, 0x000024a0, 0x00000f85, 0x0000b6c4,
    0x70617261, 0x00000000, 0x00030000, 0x00026666, 0x0000f2a7, 0x00000d59, 0x000013d0, 0x00000a5b,
};

constexpr std::array<char, 480> MakeSrgbProfile() {
    std::array<char, 480> bytes{};
    for (size_t word = 0; word < 120; word++) {
        for (size_t byte = 0; byte < 4; byte++) bytes[word * 4 + byte] = (char)((kSrgbWords[word] >> (24 - byte * 8)) & 0xFF);
    }
    return bytes;
}
constexpr std::array<char, 480> kSrgbProfile = MakeSrgbProfile();

constexpr size_t kMaxTextXmlBytes = 2048;

// What a character of PdfaText takes in XML: a reference for the five special ones.
size_t XmlBytes(uint32_t codePoint, size_t utf8Bytes) {
    switch (codePoint) {
    case '&': return 5;
    case '<': case '>': return 4;
    case '"': case '\'': return 6;
    default: return utf8Bytes;
    }
}

// PdfaText or RayoMD's own text in XML, with the special characters as references.
void AppendXml(std::string& out, std::string_view text) {
    for (const char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += ch;
        }
    }
}

// One property of the reversible profile in the extension schema.
void AppendSchemaProperty(std::string& out, const char* name, const char* type, const char* description) {
    out += "<rdf:li rdf:parseType=\"Resource\"><pdfaProperty:name>";
    out += name;
    out += "</pdfaProperty:name><pdfaProperty:valueType>";
    out += type;
    out += "</pdfaProperty:valueType><pdfaProperty:category>internal</pdfaProperty:category><pdfaProperty:description>";
    out += description;
    out += "</pdfaProperty:description></rdf:li>";
}

uint64_t Load64(const unsigned char* at) {
    uint64_t value;
    memcpy(&value, at, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

uint64_t Rotate(uint64_t value, int bits) {
    return (value << bits) | (value >> (64 - bits));
}

// MurmurHash3's finalizer: every input bit reaches every output bit.
uint64_t Mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return value;
}

// The two halves of the 128-bit product folded together, as wyhash mixes 16 bytes at once.
uint64_t Fold(uint64_t x, uint64_t y) {
#ifdef __SIZEOF_INT128__
    const unsigned __int128 product = (unsigned __int128)x * y;
    return (uint64_t)product ^ (uint64_t)(product >> 64);
#else
    // The same product from four 32-bit ones, where there is no 128-bit type.
    const uint64_t low = (uint32_t)x * (uint64_t)(uint32_t)y;
    const uint64_t cross1 = (uint32_t)x * (y >> 32);
    const uint64_t cross2 = (x >> 32) * (uint64_t)(uint32_t)y;
    const uint64_t middle = (low >> 32) + (uint32_t)cross1 + (uint32_t)cross2;
    const uint64_t high = (x >> 32) * (y >> 32) + (cross1 >> 32) + (cross2 >> 32) + (middle >> 32);
    return ((middle << 32) | (uint32_t)low) ^ high;
#endif
}

// One block of 48 bytes into the three lanes, each with its own multiply.
void Absorb(uint64_t (&lanes)[3], const unsigned char* at) {
    lanes[0] = Fold(Load64(at) ^ 0xE7037ED1A0B428DBull, Load64(at + 8) ^ lanes[0]);
    lanes[1] = Fold(Load64(at + 16) ^ 0x8EBC6AF09C88C6E3ull, Load64(at + 24) ^ lanes[1]);
    lanes[2] = Fold(Load64(at + 32) ^ 0x589965CC75374CC3ull, Load64(at + 40) ^ lanes[2]);
}

} // namespace

std::string_view SrgbIccProfile() {
    return std::string_view(kSrgbProfile.data(), kSrgbProfile.size());
}

std::string PdfaText(std::string_view utf8) {
    std::string out;
    size_t xmlBytes = 0;
    for (size_t at = 0; at < utf8.size();) {
        uint32_t codePoint = (unsigned char)utf8[at];
        size_t length = 1;
        if (codePoint >= 0x80 && !RayoMd::Text::DecodeUtf8(utf8, at, codePoint, length)) {
            at++;   // a byte of no well-formed sequence
            continue;
        }
        // Controls, which an Info string drops, and what XML 1.0 has no character for.
        if (codePoint >= 0x20 && codePoint != 0x7F && codePoint != 0xFFFE && codePoint != 0xFFFF) {
            const size_t cost = XmlBytes(codePoint, length);
            if (xmlBytes + cost > kMaxTextXmlBytes) break;
            xmlBytes += cost;
            out.append(utf8.data() + at, length);
        }
        at += length;
    }
    return out;
}

std::string PdfaXmpMetadata(const PdfaMetadata& metadata) {
    std::string out;
    out.reserve(metadata.source.empty() ? 1024 : 3584);
    out += "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\" pdfaid:part=\"3\" "
        "pdfaid:conformance=\"B\"/>"
        "<rdf:Description rdf:about=\"\" xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\" pdf:Producer=\"";
    AppendXml(out, metadata.producer);
    out += '"';
    if (!metadata.keywords.empty()) {
        out += " pdf:Keywords=\"";
        AppendXml(out, metadata.keywords);
        out += '"';
    }
    out += "/><rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:CreatorTool=\"RayoMD\"/>"
        "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">";
    if (!metadata.title.empty()) {
        out += "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">";
        AppendXml(out, metadata.title);
        out += "</rdf:li></rdf:Alt></dc:title>";
    }
    if (!metadata.author.empty()) {
        out += "<dc:creator><rdf:Seq><rdf:li>";
        AppendXml(out, metadata.author);
        out += "</rdf:li></rdf:Seq></dc:creator>";
    }
    if (!metadata.subject.empty()) {
        out += "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">";
        AppendXml(out, metadata.subject);
        out += "</rdf:li></rdf:Alt></dc:description>";
    }
    out += "</rdf:Description>";
    if (!metadata.source.empty()) {
        // The reversible profile's properties, each once (Inspect reads them), and the extension
        // schema that declares them to PDF/A.
        out += "<rdf:Description rdf:about=\"\" xmlns:rayomd=\"https://rayomd.dev/ns/source/1.0/\" "
            "rayomd:profile=\"rayomd-source/1\" rayomd:producer=\"";
        AppendXml(out, metadata.version);
        out += "\" rayomd:encoding=\"UTF-8\" rayomd:length=\"" + std::to_string(metadata.source.size());
        out += "\" rayomd:sha256=\"" + RayoMd::PdfSource::Sha256Hex(metadata.source);
        out += "\" rayomd:attachment=\"source.md\"/>"
            "<rdf:Description rdf:about=\"\" xmlns:pdfaExtension=\"http://www.aiim.org/pdfa/ns/extension/\" "
            "xmlns:pdfaSchema=\"http://www.aiim.org/pdfa/ns/schema#\" xmlns:pdfaProperty=\"http://www.aiim.org/pdfa/ns/property#\">"
            "<pdfaExtension:schemas><rdf:Bag><rdf:li rdf:parseType=\"Resource\">"
            "<pdfaSchema:schema>RayoMD reversible source</pdfaSchema:schema>"
            "<pdfaSchema:namespaceURI>https://rayomd.dev/ns/source/1.0/</pdfaSchema:namespaceURI>"
            "<pdfaSchema:prefix>rayomd</pdfaSchema:prefix><pdfaSchema:property><rdf:Seq>";
        AppendSchemaProperty(out, "profile", "Text", "RayoMD source profile identifier");
        AppendSchemaProperty(out, "producer", "Text", "RayoMD version that embedded the source");
        AppendSchemaProperty(out, "encoding", "Text", "Character encoding of the source");
        AppendSchemaProperty(out, "length", "Integer", "Size of the source in bytes");
        AppendSchemaProperty(out, "sha256", "Text", "SHA-256 of the source, hexadecimal");
        AppendSchemaProperty(out, "attachment", "Text", "File name of the embedded source");
        out += "</rdf:Seq></pdfaSchema:property></rdf:li></rdf:Bag></pdfaExtension:schemas></rdf:Description>";
    }
    out += "</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
    return out;
}

void FileIdentifier(std::string_view body, char (&hex)[32]) {
    // Three lanes that do not wait on each other: 30 GB/s, where two lanes that did reached 8.
    // Not cryptographic, as an /ID need not be.
    const unsigned char* at = reinterpret_cast<const unsigned char*>(body.data());
    size_t left = body.size();
    uint64_t lanes[3] = { 0xA0761D6478BD642Full ^ left, 0xE7037ED1A0B428DBull + left, 0x8EBC6AF09C88C6E3ull - left };
    for (; left >= 48; at += 48, left -= 48) Absorb(lanes, at);
    unsigned char tail[48] = {};
    if (left != 0) memcpy(tail, at, left);
    Absorb(lanes, tail);
    const uint64_t a = Mix(lanes[0] ^ Rotate(lanes[1], 23) ^ Rotate(lanes[2], 47) ^ body.size());
    const uint64_t b = Mix(lanes[2] ^ Rotate(lanes[0], 29) ^ Rotate(lanes[1], 53) ^ a);
    static const char kDigits[] = "0123456789ABCDEF";
    for (int digit = 0; digit < 16; digit++) {
        hex[digit] = kDigits[(a >> (60 - digit * 4)) & 0xF];
        hex[16 + digit] = kDigits[(b >> (60 - digit * 4)) & 0xF];
    }
}

} // namespace TinyPdf::Internal
