#pragma once

// The pieces of a PDF/A-3b file (ISO 19005-3, level B) that ordinary exports do not have: the
// sRGB ICC profile of its output intent, its XMP metadata and its file identifier. Kept out of
// tiny_pdf.cpp, which sits at GCC's inlining limit. Checked with veraPDF.

#include <string>
#include <string_view>

namespace TinyPdf::Internal {

// The 480-byte sRGB ICC profile (version 4.2, display class) of the output intent.
std::string_view SrgbIccProfile();

// Text that the Info dictionary and the XMP packet, which PDF/A needs to tell the same, both
// hold exactly: the valid UTF-8 of `utf8` without control characters, which an Info string
// drops and XML cannot hold, cut at a character so that it takes at most 2 KiB as XML. Four
// such values keep a packet within the 16 KiB that source recovery reads.
std::string PdfaText(std::string_view utf8);

// What the XMP packet tells, as the Info dictionary does (PdfaText values); with `source`, also
// the reversible profile's properties and the extension schema PDF/A needs for them.
struct PdfaMetadata {
    std::string_view producer;
    std::string_view title;
    std::string_view author;
    std::string_view subject;
    std::string_view keywords;
    std::string_view source;    // the embedded Markdown, or empty
    std::string_view version;   // RayoMD's, for rayomd:producer
};

std::string PdfaXmpMetadata(const PdfaMetadata& metadata);

// The file identifier of the trailer's /ID: 128 bits of a hash of the file's body, as 32
// uppercase hex digits.
void FileIdentifier(std::string_view body, char (&hex)[32]);

} // namespace TinyPdf::Internal
