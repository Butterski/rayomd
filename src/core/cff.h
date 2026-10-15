// SPDX-License-Identifier: Apache-2.0

#pragma once

// OpenType fonts with PostScript outlines: the 'CFF ' table read once when the font is loaded,
// and the font programs a PDF embeds made from it, a CID-keyed subset whose CIDs are the code
// points the page content shows (FontFile3 /CIDFontType0C), and the simple fonts of the formula
// faces (FontFile3 /Type1C). Every glyph is written without subroutines, so a subset needs none
// of the font's. Every read is bounds-checked: font files are untrusted. Built at -Os: a font is
// read once per process and a subset made once per document.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal::Cff {

// A CFF INDEX: `count` items whose offsets, `offSize` bytes each, start at `offsets`; item i
// spans [dataStart + offset[i], dataStart + offset[i + 1]) of the table, all of it before
// dataEnd.
struct Index {
    uint32_t count = 0;
    uint32_t dataStart = 0;
    uint32_t dataEnd = 0;
    const uint8_t* offsets = nullptr;
    uint8_t offSize = 0;
};

// Reads the INDEX at `at` of the `size` bytes at `base`; `next` is the offset after it.
bool ReadIndex(const uint8_t* base, size_t size, size_t at, Index& index, size_t& next);
// Item `i` of `index`; a view with a null data pointer when its offsets are out of range.
std::string_view IndexItem(const uint8_t* base, const Index& index, uint32_t i);

// A DICT entry: the operator (12 x as 1200 + x), its operands, and the bytes the entry spans in
// the DICT, operands included, which a writer copies to keep a value exactly as it was.
struct DictEntry {
    uint16_t op = 0;
    uint8_t operandCount = 0;
    double operands[48] = {};
    uint32_t operandsBegin = 0;
    uint32_t end = 0;
};

// Parses a DICT into at most `max` entries; false when it is malformed or has more.
bool ParseDict(std::string_view dict, DictEntry* out, size_t max, size_t& count);

// A font DICT of a CID-keyed font, or the one font DICT a name-keyed font is read as.
struct FontDict {
    std::string_view privateDict;
    Index localSubrs;
    bool hasLocalSubrs = false;
    std::string_view fontMatrix;     // the FontMatrix entry's bytes, operator included, or empty
};

// A 'CFF ' table, checked when the font is loaded.
struct CffFont {
    const uint8_t* data = nullptr;
    size_t size = 0;
    Index globalSubrs;
    Index charStrings;
    std::string_view fontBBox;       // the FontBBox entry's bytes, operator included, or empty
    std::string_view fontMatrix;     // the Top DICT's FontMatrix entry, or empty
    bool cidKeyed = false;
    uint8_t fdSelectFormat = 0;      // CID-keyed: 0 or 3
    uint32_t fdSelect = 0;           // CID-keyed: offset of the FDSelect data after its format byte
    uint16_t fdRanges = 0;           // FDSelect format 3: its ranges, as ParseCff checked them
    uint16_t glyphCount = 0;
    std::vector<FontDict> fds;

    // The font DICT of `glyph`.
    uint8_t FdFor(uint16_t glyph) const;
};

// Reads the `size` bytes of a 'CFF ' table; false when it is not a CFF font of `glyphCount`
// glyphs with Type 2 charstrings or anything in it is out of range.
bool ParseCff(const uint8_t* data, size_t size, uint16_t glyphCount, CffFont& font);

// The charstring of `glyph` without subroutine calls: each called subroutine inline, the call
// and its operand removed, hint masks copied with the bytes they take. False when the glyph uses
// seac, runs past a limit or reads out of range.
bool Desubroutinize(const CffFont& font, uint16_t glyph, std::string& out);

// A glyph of a CID-keyed subset: the CID it is shown as and its glyph in the font.
struct CidGlyph {
    uint16_t cid;
    uint16_t glyph;
};

// A CID-keyed CFF font of .notdef and `glyphs`, ascending by CID, none of them CID 0: the
// Adobe-Identity ordering, each glyph's CID in the charset, the font DICTs and private DICTs of
// the glyphs it holds (without their subroutines), and FontBBox and FontMatrix as the font has
// them. A glyph that cannot be desubroutinized is written empty. False, with no font, when the
// glyphs' charstrings would pass 64 MiB (kMaxSubsetCharStringBytes in cff.cpp).
bool WriteCidKeyedSubset(const CffFont& font, const char* fontName, const std::vector<CidGlyph>& glyphs,
    std::string& out);

// A glyph of a simple font: its code, its name as a standard string (SID below 391) or, with
// `sid` 0xFFFF, `name`, and its Type 2 charstring, whose width operand counts from 0.
struct SimpleGlyph {
    uint8_t code;
    uint16_t sid;
    std::string_view name;
    std::string_view program;
};

// A name-keyed CFF font of .notdef and `glyphs`, ascending by code, with a built-in encoding of
// their codes; nominalWidthX and defaultWidthX are 0.
bool WriteSimpleFont(const char* fontName, const SimpleGlyph* glyphs, size_t count, const int16_t bbox[4],
    std::string& out);

} // namespace TinyPdf::Internal::Cff
