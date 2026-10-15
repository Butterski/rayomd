// SPDX-License-Identifier: Apache-2.0

#include "cff.h"

#include <cstring>
#include <memory>

namespace TinyPdf::Internal::Cff {
namespace {

constexpr size_t kMaxFds = 256;
constexpr size_t kMaxDictEntries = 64;
constexpr int kMaxSubrDepth = 10;
constexpr size_t kMaxGlyphBytes = 65535;
constexpr size_t kMaxInterpretedBytes = 1000000;

uint32_t ReadOffset(const uint8_t* at, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++) value = value << 8 | at[i];
    return value;
}

uint16_t ReadU16(const uint8_t* at) {
    return (uint16_t)(at[0] << 8 | at[1]);
}

// A DICT operand that is a whole number from 0 to 2^31 - 1, as offsets and sizes are.
bool Offset(const DictEntry& entry, int index, uint32_t& value) {
    if (index >= entry.operandCount) return false;
    const double operand = entry.operands[index];
    if (!(operand >= 0.0 && operand <= 2147483647.0)) return false;
    value = (uint32_t)operand;
    return (double)value == operand;
}

// The value of a real number operand, nibbles from `at` (after the 30): digits, '.', exponents
// and a minus sign, up to the nibble 0xf. Close enough for what RayoMD reads of them: it copies
// real-valued entries (FontMatrix, FontBBox) as they are written.
bool ReadReal(const uint8_t* data, size_t size, size_t& at, double& value) {
    double mantissa = 0.0;
    double scale = 1.0;
    int exponent = 0;
    bool negative = false;
    bool fraction = false;
    bool inExponent = false;
    bool negativeExponent = false;
    while (at < size) {
        const uint8_t byte = data[at++];
        for (int shift = 4; shift >= 0; shift -= 4) {
            const uint8_t nibble = (byte >> shift) & 15;
            if (nibble == 0xf) {
                if (inExponent) exponent = negativeExponent ? -exponent : exponent;
                double result = mantissa;
                for (int i = 0; i < exponent && i < 64; i++) result *= 10.0;
                for (int i = 0; i > exponent && i > -64; i--) result /= 10.0;
                value = negative ? -result : result;
                return true;
            }
            if (nibble <= 9) {
                if (inExponent) {
                    if (exponent < 1000) exponent = exponent * 10 + nibble;
                } else if (fraction) {
                    scale /= 10.0;
                    mantissa += nibble * scale;
                } else {
                    mantissa = mantissa * 10.0 + nibble;
                }
            } else if (nibble == 0xa) {
                fraction = true;
            } else if (nibble == 0xb || nibble == 0xc) {
                inExponent = true;
                negativeExponent = nibble == 0xc;
            } else if (nibble == 0xe) {
                negative = true;
            } else {
                return false;
            }
        }
    }
    return false;
}

// Integers of a DICT, as short as they go.
void PutDictInt(std::string& out, int32_t value) {
    if (value >= -107 && value <= 107) {
        out.push_back((char)(value + 139));
    } else if (value >= 108 && value <= 1131) {
        out.push_back((char)(247 + (value - 108) / 256));
        out.push_back((char)((value - 108) % 256));
    } else if (value >= -1131 && value <= -108) {
        out.push_back((char)(251 + (-value - 108) / 256));
        out.push_back((char)((-value - 108) % 256));
    } else if (value >= -32768 && value <= 32767) {
        out.push_back((char)28);
        out.push_back((char)((value >> 8) & 0xff));
        out.push_back((char)(value & 0xff));
    } else {
        out.push_back((char)29);
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back((char)(((uint32_t)value >> shift) & 0xff));
    }
}

// An offset or size in five bytes, so a DICT keeps its size whatever the value.
void PutDictInt5(std::string& out, uint32_t value) {
    out.push_back((char)29);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((char)((value >> shift) & 0xff));
}

void PutOp(std::string& out, uint16_t op) {
    if (op >= 1200) {
        out.push_back((char)12);
        out.push_back((char)(op - 1200));
    } else {
        out.push_back((char)op);
    }
}

void PutU16(std::string& out, uint16_t value) {
    out.push_back((char)(value >> 8));
    out.push_back((char)(value & 0xff));
}

// An INDEX of `data` cut at `ends`: item i spans [ends[i - 1], ends[i]), item 0 from 0.
void PutIndex(std::string& out, std::string_view data, const std::vector<uint32_t>& ends) {
    PutU16(out, (uint16_t)ends.size());
    if (ends.empty()) return;
    const uint32_t last = (uint32_t)data.size() + 1;
    const uint8_t offSize = last < 0x100 ? 1 : last < 0x10000 ? 2 : last < 0x1000000 ? 3 : 4;
    out.push_back((char)offSize);
    auto put = [&](uint32_t offset) {
        for (int byte = offSize - 1; byte >= 0; byte--) out.push_back((char)((offset >> (byte * 8)) & 0xff));
    };
    put(1);
    for (uint32_t end : ends) put(end + 1);
    out.append(data.data(), data.size());
}

void PutIndexOfOne(std::string& out, std::string_view item) {
    PutIndex(out, item, std::vector<uint32_t>{ (uint32_t)item.size() });
}

// The private DICT of `fd` as it is, its Subrs entry removed: a subset has no subroutines.
bool PrivateWithoutSubrs(const FontDict& fd, std::string& out) {
    std::vector<DictEntry> entries(kMaxDictEntries);
    size_t count = 0;
    if (!ParseDict(fd.privateDict, entries.data(), entries.size(), count)) return false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].op == 19) continue;
        out.append(fd.privateDict.data() + entries[i].operandsBegin, entries[i].end - entries[i].operandsBegin);
    }
    return true;
}

bool LoadPrivate(const uint8_t* data, size_t size, uint32_t privateSize, uint32_t privateOffset, FontDict& fd) {
    if (privateOffset > size || privateSize > size - privateOffset) return false;
    fd.privateDict = std::string_view((const char*)data + privateOffset, privateSize);
    std::vector<DictEntry> entries(kMaxDictEntries);
    size_t count = 0;
    if (!ParseDict(fd.privateDict, entries.data(), entries.size(), count)) return false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].op != 19) continue;
        uint32_t subrs = 0;
        size_t next = 0;
        if (!Offset(entries[i], 0, subrs) || (uint64_t)privateOffset + subrs > size ||
            !ReadIndex(data, size, (size_t)privateOffset + subrs, fd.localSubrs, next)) {
            return false;
        }
        fd.hasLocalSubrs = true;
    }
    return true;
}

// The Type 2 charstring interpreter of Desubroutinize: it follows the operand stack only as far
// as it needs to, for subroutine numbers and the hints a hint mask covers. It writes into `out`,
// which has room for kMaxGlyphBytes + 1 bytes: a token that does not fit makes the glyph longer
// than kMaxGlyphBytes, which fails after that token anyway, and endchar adds one byte to at most
// kMaxGlyphBytes.
struct Flattener {
    static constexpr size_t kCapacity = kMaxGlyphBytes + 1;

    const CffFont& font;
    const FontDict& fd;
    char* out;
    size_t written = 0;               // the bytes in `out`
    uint32_t operandAt[48] = {};      // where each operand on the stack starts in `out`
    int32_t operandValue[48] = {};
    bool operandInteger[48] = {};
    int operands = 0;
    uint32_t stems = 0;
    size_t budget = kMaxInterpretedBytes;
    bool ended = false;

    // Tokens are a few bytes, copied a byte at a time: -Os makes memcpy `rep movsb`, which takes
    // dozens of cycles to start.
    bool Put(const uint8_t* p, size_t n) {
        if (n > kCapacity - written) return false;
        char* const to = out + written;
        for (size_t i = 0; i < n; i++) to[i] = (char)p[i];
        written += n;
        return true;
    }
    bool PutByte(uint8_t b) {
        if (written == kCapacity) return false;
        out[written++] = (char)b;
        return true;
    }

    bool Run(std::string_view code, int depth) {
        const uint8_t* const bytes = (const uint8_t*)code.data();
        const size_t size = code.size();
        size_t at = 0;
        while (at < size) {
            const uint8_t b0 = bytes[at];
            if (b0 == 28 || b0 >= 32) {
                const size_t length = b0 == 28 ? 3 : b0 <= 246 ? 1 : b0 <= 254 ? 2 : 5;
                if (length > size - at || length > budget || operands >= 48) return false;
                int32_t value = 0;
                bool integer = true;
                if (b0 == 28) {
                    value = (int16_t)ReadU16(bytes + at + 1);
                } else if (b0 <= 246) {
                    value = b0 - 139;
                } else if (b0 <= 250) {
                    value = (b0 - 247) * 256 + bytes[at + 1] + 108;
                } else if (b0 <= 254) {
                    value = -(b0 - 251) * 256 - bytes[at + 1] - 108;
                } else {
                    const uint32_t fixed = ReadOffset(bytes + at + 1, 4);
                    value = (int16_t)(fixed >> 16);
                    integer = (fixed & 0xffff) == 0;
                }
                operandAt[operands] = (uint32_t)written;
                operandValue[operands] = value;
                operandInteger[operands] = integer;
                operands++;
                if (!Put(bytes + at, length)) return false;
                at += length;
                budget -= length;
            } else {
                if (budget == 0) return false;
                budget--;
                switch (b0) {
                case 10:     // callsubr
                case 29: {   // callgsubr
                    if (operands == 0 || !operandInteger[operands - 1] || depth >= kMaxSubrDepth) return false;
                    operands--;
                    written = operandAt[operands];
                    const Index& subrs = b0 == 10 ? fd.localSubrs : font.globalSubrs;
                    if (b0 == 10 && !fd.hasLocalSubrs) return false;
                    const int32_t bias = subrs.count < 1240 ? 107 : subrs.count < 33900 ? 1131 : 32768;
                    const int64_t number = (int64_t)operandValue[operands] + bias;
                    if (number < 0 || number >= (int64_t)subrs.count) return false;
                    const std::string_view subr = IndexItem(font.data, subrs, (uint32_t)number);
                    if (subr.data() == nullptr || !Run(subr, depth + 1)) return false;
                    if (ended) return true;
                    at++;
                    continue;
                }
                case 11:     // return
                    return depth > 0;
                case 14:     // endchar; with 4 or 5 operands seac, an accented glyph made of two
                    if (operands == 4 || operands == 5) return false;
                    if (!PutByte(14)) return false;
                    ended = true;
                    return true;
                case 1:      // hstem
                case 3:      // vstem
                case 18:     // hstemhm
                case 23:     // vstemhm
                    stems += (uint32_t)operands / 2;
                    if (!PutByte(b0)) return false;
                    at++;
                    break;
                case 19:     // hintmask
                case 20: {   // cntrmask: after implicit vstems, a bit per hint
                    stems += (uint32_t)operands / 2;
                    const size_t length = 1 + (stems + 7) / 8;
                    if (length > size - at || length > budget + 1) return false;
                    budget -= length - 1;
                    if (!Put(bytes + at, length)) return false;
                    at += length;
                    break;
                }
                case 12:
                    if (2 > size - at) return false;
                    if (!Put(bytes + at, 2)) return false;
                    at += 2;
                    break;
                case 0: case 2: case 9: case 13: case 15: case 16: case 17:
                    return false;    // reserved
                default:
                    if (!PutByte(b0)) return false;
                    at++;
                    break;
                }
                operands = 0;
            }
            if (written > kMaxGlyphBytes) return false;
        }
        // A subroutine may end without return; a glyph must end with endchar.
        return depth > 0;
    }
};

} // namespace

bool ReadIndex(const uint8_t* base, size_t size, size_t at, Index& index, size_t& next) {
    index = Index();
    if (at > size || size - at < 2) return false;
    const uint32_t count = ReadU16(base + at);
    if (count == 0) {
        index.dataStart = index.dataEnd = (uint32_t)(at + 2);
        next = at + 2;
        return true;
    }
    if (size - at < 3) return false;
    const uint8_t offSize = base[at + 2];
    if (offSize < 1 || offSize > 4) return false;
    const size_t offsetsAt = at + 3;
    const size_t offsetsBytes = (size_t)(count + 1) * offSize;
    if (offsetsBytes > size - offsetsAt) return false;
    const size_t dataStart = offsetsAt + offsetsBytes - 1;
    const uint32_t first = ReadOffset(base + offsetsAt, offSize);
    const uint32_t last = ReadOffset(base + offsetsAt + (size_t)count * offSize, offSize);
    if (first != 1 || last < 1 || last > size - dataStart || dataStart + last > 0xffffffffu) return false;
    index.count = count;
    index.offSize = offSize;
    index.offsets = base + offsetsAt;
    index.dataStart = (uint32_t)dataStart;
    index.dataEnd = (uint32_t)(dataStart + last);
    next = dataStart + last;
    return true;
}

std::string_view IndexItem(const uint8_t* base, const Index& index, uint32_t i) {
    if (i >= index.count) return {};
    const uint32_t start = ReadOffset(index.offsets + (size_t)i * index.offSize, index.offSize);
    const uint32_t end = ReadOffset(index.offsets + (size_t)(i + 1) * index.offSize, index.offSize);
    if (start < 1 || end < start || (uint64_t)index.dataStart + end > index.dataEnd) return {};
    return std::string_view((const char*)base + index.dataStart + start, end - start);
}

bool ParseDict(std::string_view dict, DictEntry* out, size_t max, size_t& count) {
    count = 0;
    const uint8_t* const data = (const uint8_t*)dict.data();
    const size_t size = dict.size();
    double values[48];
    uint8_t operands = 0;
    size_t begin = 0;
    size_t at = 0;
    while (at < size) {
        const uint8_t b0 = data[at];
        if (b0 <= 21) {
            uint16_t op = b0;
            size_t length = 1;
            if (b0 == 12) {
                if (at + 1 >= size) return false;
                op = (uint16_t)(1200 + data[at + 1]);
                length = 2;
            }
            if (count >= max) return false;
            DictEntry& entry = out[count++];
            entry.op = op;
            entry.operandCount = operands;
            for (uint8_t i = 0; i < operands; i++) entry.operands[i] = values[i];
            entry.operandsBegin = (uint32_t)begin;
            entry.end = (uint32_t)(at + length);
            at += length;
            operands = 0;
            begin = at;
            continue;
        }
        double value = 0.0;
        if (b0 == 28) {
            if (size - at < 3) return false;
            value = (int16_t)ReadU16(data + at + 1);
            at += 3;
        } else if (b0 == 29) {
            if (size - at < 5) return false;
            value = (int32_t)ReadOffset(data + at + 1, 4);
            at += 5;
        } else if (b0 == 30) {
            at++;
            if (!ReadReal(data, size, at, value)) return false;
        } else if (b0 >= 32 && b0 <= 246) {
            value = b0 - 139;
            at++;
        } else if (b0 >= 247 && b0 <= 254) {
            if (size - at < 2) return false;
            value = b0 <= 250 ? (b0 - 247) * 256 + data[at + 1] + 108 : -(b0 - 251) * 256 - data[at + 1] - 108;
            at += 2;
        } else {
            return false;
        }
        if (operands >= 48) return false;
        values[operands++] = value;
    }
    return operands == 0;
}

uint8_t CffFont::FdFor(uint16_t glyph) const {
    if (!cidKeyed) return 0;
    if (fdSelectFormat == 0) return glyph < glyphCount ? data[fdSelect + glyph] : 0;
    // Format 3: ranges of (first glyph, FD) with ascending first glyphs; the last range whose
    // first glyph is not after `glyph`. As many ranges as ParseCff checked, not as the data says
    // now: a font file changed in place since may say more.
    const uint8_t* const ranges = data + fdSelect + 2;
    size_t low = 0;
    size_t high = fdRanges;
    while (high - low > 1) {
        const size_t middle = (low + high) / 2;
        if (ReadU16(ranges + middle * 3) <= glyph) low = middle;
        else high = middle;
    }
    return ranges[low * 3 + 2];
}

bool ParseCff(const uint8_t* data, size_t size, uint16_t glyphCount, CffFont& font) {
    font = CffFont();
    if (size < 4 || data[0] != 1) return false;
    const size_t headerSize = data[2];
    if (headerSize < 4 || headerSize > size) return false;
    Index names, topDicts, strings;
    size_t next = 0;
    if (!ReadIndex(data, size, headerSize, names, next) || !ReadIndex(data, size, next, topDicts, next) ||
        !ReadIndex(data, size, next, strings, next) || !ReadIndex(data, size, next, font.globalSubrs, next) ||
        names.count == 0 || topDicts.count == 0) {
        return false;
    }
    const std::string_view top = IndexItem(data, topDicts, 0);
    if (top.data() == nullptr) return false;
    std::vector<DictEntry> entries(kMaxDictEntries);
    size_t count = 0;
    if (!ParseDict(top, entries.data(), entries.size(), count)) return false;
    uint32_t charStrings = 0, privateSize = 0, privateOffset = 0, fdArray = 0, fdSelect = 0;
    bool hasCharStrings = false, hasPrivate = false, hasFdArray = false, hasFdSelect = false;
    for (size_t i = 0; i < count; i++) {
        const DictEntry& entry = entries[i];
        const std::string_view bytes = top.substr(entry.operandsBegin, entry.end - entry.operandsBegin);
        switch (entry.op) {
        case 5:
            if (entry.operandCount != 4) return false;
            font.fontBBox = bytes;
            break;
        case 17:
            hasCharStrings = Offset(entry, 0, charStrings);
            break;
        case 18:
            hasPrivate = Offset(entry, 0, privateSize) && Offset(entry, 1, privateOffset);
            break;
        case 1206:   // CharstringType: Type 2 only
            if (entry.operandCount != 1 || entry.operands[0] != 2.0) return false;
            break;
        case 1207:
            if (entry.operandCount != 6) return false;
            font.fontMatrix = bytes;
            break;
        case 1230:   // ROS
            font.cidKeyed = true;
            break;
        case 1236:
            hasFdArray = Offset(entry, 0, fdArray);
            break;
        case 1237:
            hasFdSelect = Offset(entry, 0, fdSelect);
            break;
        default:
            break;
        }
    }
    if (!hasCharStrings || !ReadIndex(data, size, charStrings, font.charStrings, next) ||
        font.charStrings.count != glyphCount || glyphCount == 0) {
        return false;
    }
    font.data = data;
    font.size = size;
    font.glyphCount = glyphCount;
    if (!font.cidKeyed) {
        FontDict fd;
        if (!hasPrivate || !LoadPrivate(data, size, privateSize, privateOffset, fd)) return false;
        font.fds.push_back(fd);
        return true;
    }

    Index fdIndex;
    if (!hasFdArray || !hasFdSelect || !ReadIndex(data, size, fdArray, fdIndex, next) || fdIndex.count == 0 ||
        fdIndex.count > kMaxFds) {
        return false;
    }
    font.fds.reserve(fdIndex.count);
    for (uint32_t index = 0; index < fdIndex.count; index++) {
        const std::string_view dict = IndexItem(data, fdIndex, index);
        if (dict.data() == nullptr || !ParseDict(dict, entries.data(), entries.size(), count)) return false;
        FontDict fd;
        bool fdPrivate = false;
        uint32_t fdPrivateSize = 0, fdPrivateOffset = 0;
        for (size_t i = 0; i < count; i++) {
            const DictEntry& entry = entries[i];
            if (entry.op == 18) {
                fdPrivate = Offset(entry, 0, fdPrivateSize) && Offset(entry, 1, fdPrivateOffset);
            } else if (entry.op == 1207 && entry.operandCount == 6) {
                fd.fontMatrix = dict.substr(entry.operandsBegin, entry.end - entry.operandsBegin);
            }
        }
        if (!fdPrivate || !LoadPrivate(data, size, fdPrivateSize, fdPrivateOffset, fd)) return false;
        font.fds.push_back(fd);
    }

    if (fdSelect >= size) return false;
    font.fdSelectFormat = data[fdSelect];
    font.fdSelect = fdSelect + 1;
    const size_t at = font.fdSelect;
    if (font.fdSelectFormat == 0) {
        if (glyphCount > size - at) return false;
        for (size_t glyph = 0; glyph < glyphCount; glyph++) {
            if (data[at + glyph] >= font.fds.size()) return false;
        }
        return true;
    }
    if (font.fdSelectFormat != 3 || size - at < 2) return false;
    const size_t ranges = ReadU16(data + at);
    if (ranges == 0 || ranges * 3 + 4 > size - at) return false;
    font.fdRanges = (uint16_t)ranges;
    uint32_t previous = 0;
    for (size_t range = 0; range < ranges; range++) {
        const uint8_t* const entry = data + at + 2 + range * 3;
        const uint32_t first = ReadU16(entry);
        if ((range == 0 && first != 0) || (range > 0 && first <= previous) || entry[2] >= font.fds.size()) return false;
        previous = first;
    }
    return ReadU16(data + at + 2 + ranges * 3) > previous;
}

namespace {

// The glyph's charstring without subroutines in `buffer`, which has room for Flattener::kCapacity
// bytes; `length` gets its size.
bool FlattenGlyph(const CffFont& font, uint16_t glyph, char* buffer, size_t& length) {
    if (glyph >= font.charStrings.count || font.fds.empty()) return false;
    const std::string_view code = IndexItem(font.data, font.charStrings, glyph);
    if (code.data() == nullptr) return false;
    const uint8_t fd = font.FdFor(glyph);
    Flattener flattener{ font, font.fds[fd < font.fds.size() ? fd : 0], buffer };
    if (!flattener.Run(code, 0) || !flattener.ended) return false;
    length = flattener.written;
    return true;
}

} // namespace

bool Desubroutinize(const CffFont& font, uint16_t glyph, std::string& out) {
    out.clear();
    const std::unique_ptr<char[]> buffer(new char[Flattener::kCapacity]);
    size_t length = 0;
    if (!FlattenGlyph(font, glyph, buffer.get(), length)) return false;
    out.assign(buffer.get(), length);
    return true;
}

bool WriteCidKeyedSubset(const CffFont& font, const char* fontName, const std::vector<CidGlyph>& glyphs,
    std::string& out) {
    out.clear();
    const size_t glyphCount = glyphs.size() + 1;
    if (glyphCount > 65535 || font.fds.empty()) return false;

    // The charstrings, and the font DICTs in the order their glyphs first use them.
    std::vector<int> fdOrder(font.fds.size(), -1);
    std::vector<uint8_t> usedFds;
    std::string charSelect;          // FDSelect format 0: the new FD of each glyph
    std::string charData;
    std::vector<uint32_t> charEnds;
    charEnds.reserve(glyphCount);
    charSelect.reserve(glyphCount);
    const std::unique_ptr<char[]> program(new char[Flattener::kCapacity]);
    for (size_t index = 0; index < glyphCount; index++) {
        const uint16_t source = index == 0 ? 0 : glyphs[index - 1].glyph;
        uint8_t fd = source < font.glyphCount ? font.FdFor(source) : 0;
        if (fd >= font.fds.size()) fd = 0;
        if (fdOrder[fd] < 0) {
            fdOrder[fd] = (int)usedFds.size();
            usedFds.push_back(fd);
        }
        charSelect.push_back((char)fdOrder[fd]);
        size_t length = 0;
        if (FlattenGlyph(font, source, program.get(), length)) charData.append(program.get(), length);
        else charData.append("\x8B\x0E", 2);
        charEnds.push_back((uint32_t)charData.size());
    }
    std::vector<std::string> privates(usedFds.size());
    for (size_t index = 0; index < usedFds.size(); index++) {
        if (!PrivateWithoutSubrs(font.fds[usedFds[index]], privates[index])) return false;
    }

    std::string strings;
    PutIndex(strings, "AdobeIdentity", std::vector<uint32_t>{ 5, 13 });
    std::string charset(1, '\0');
    for (const CidGlyph& glyph : glyphs) PutU16(charset, glyph.cid);
    std::string fdSelect(1, '\0');
    fdSelect += charSelect;
    std::string charStrings;
    PutIndex(charStrings, charData, charEnds);

    // Twice: the offsets are five-byte integers, so the first pass gives every size.
    std::string header("\x01\x00\x04\x04", 4);
    PutIndexOfOne(header, fontName);
    std::string topIndex, fdArray;
    uint32_t charsetAt = 0, fdSelectAt = 0, charStringsAt = 0, fdArrayAt = 0, privatesAt = 0;
    for (int pass = 0; pass < 2; pass++) {
        std::string top;
        PutDictInt(top, 391);
        PutDictInt(top, 392);
        PutDictInt(top, 0);
        PutOp(top, 1230);
        PutDictInt(top, (int32_t)(glyphs.empty() ? 1 : glyphs.back().cid + 1));
        PutOp(top, 1234);
        top.append(font.fontBBox.data(), font.fontBBox.size());
        top.append(font.fontMatrix.data(), font.fontMatrix.size());
        PutDictInt5(top, charsetAt);
        PutOp(top, 15);
        PutDictInt5(top, charStringsAt);
        PutOp(top, 17);
        PutDictInt5(top, fdArrayAt);
        PutOp(top, 1236);
        PutDictInt5(top, fdSelectAt);
        PutOp(top, 1237);
        topIndex.clear();
        PutIndexOfOne(topIndex, top);

        std::string fdData;
        std::vector<uint32_t> fdEnds;
        uint32_t privateAt = privatesAt;
        for (size_t index = 0; index < usedFds.size(); index++) {
            PutDictInt5(fdData, (uint32_t)privates[index].size());
            PutDictInt5(fdData, privateAt);
            PutOp(fdData, 18);
            const std::string_view matrix = font.fds[usedFds[index]].fontMatrix;
            fdData.append(matrix.data(), matrix.size());
            fdEnds.push_back((uint32_t)fdData.size());
            privateAt += (uint32_t)privates[index].size();
        }
        fdArray.clear();
        PutIndex(fdArray, fdData, fdEnds);

        charsetAt = (uint32_t)(header.size() + topIndex.size() + strings.size() + 2);
        fdSelectAt = charsetAt + (uint32_t)charset.size();
        charStringsAt = fdSelectAt + (uint32_t)fdSelect.size();
        fdArrayAt = charStringsAt + (uint32_t)charStrings.size();
        privatesAt = fdArrayAt + (uint32_t)fdArray.size();
    }
    size_t total = privatesAt;
    for (const std::string& dict : privates) total += dict.size();
    out.reserve(total);
    out += header;
    out += topIndex;
    out += strings;
    out.append(2, '\0');             // no global subroutines
    out += charset;
    out += fdSelect;
    out += charStrings;
    out += fdArray;
    for (const std::string& dict : privates) out += dict;
    return out.size() == total;
}

bool WriteSimpleFont(const char* fontName, const SimpleGlyph* glyphs, size_t count, const int16_t bbox[4],
    std::string& out) {
    out.clear();
    if (count > 255) return false;
    std::string stringData;
    std::vector<uint32_t> stringEnds;
    std::string charset(1, '\0');
    std::string encoding(1, '\0');
    encoding.push_back((char)count);
    std::string charData("\x8B\x0E", 2);
    std::vector<uint32_t> charEnds{ 2 };
    for (size_t index = 0; index < count; index++) {
        const SimpleGlyph& glyph = glyphs[index];
        uint16_t sid = glyph.sid;
        if (sid >= 391) {
            sid = (uint16_t)(391 + stringEnds.size());
            stringData.append(glyph.name.data(), glyph.name.size());
            stringEnds.push_back((uint32_t)stringData.size());
        }
        PutU16(charset, sid);
        encoding.push_back((char)glyph.code);
        charData.append(glyph.program.data(), glyph.program.size());
        charEnds.push_back((uint32_t)charData.size());
    }
    std::string strings;
    PutIndex(strings, stringData, stringEnds);
    std::string charStrings;
    PutIndex(charStrings, charData, charEnds);
    const std::string privateDict("\x8B\x14\x8B\x15", 4);   // defaultWidthX 0, nominalWidthX 0

    std::string header("\x01\x00\x04\x04", 4);
    PutIndexOfOne(header, fontName);
    std::string topIndex;
    uint32_t charsetAt = 0, encodingAt = 0, charStringsAt = 0, privateAt = 0;
    for (int pass = 0; pass < 2; pass++) {
        std::string top;
        for (int side = 0; side < 4; side++) PutDictInt(top, bbox[side]);
        PutOp(top, 5);
        PutDictInt5(top, charsetAt);
        PutOp(top, 15);
        PutDictInt5(top, encodingAt);
        PutOp(top, 16);
        PutDictInt5(top, charStringsAt);
        PutOp(top, 17);
        PutDictInt5(top, (uint32_t)privateDict.size());
        PutDictInt5(top, privateAt);
        PutOp(top, 18);
        topIndex.clear();
        PutIndexOfOne(topIndex, top);
        charsetAt = (uint32_t)(header.size() + topIndex.size() + strings.size() + 2);
        encodingAt = charsetAt + (uint32_t)charset.size();
        charStringsAt = encodingAt + (uint32_t)encoding.size();
        privateAt = charStringsAt + (uint32_t)charStrings.size();
    }
    out.reserve(privateAt + privateDict.size());
    out += header;
    out += topIndex;
    out += strings;
    out.append(2, '\0');
    out += charset;
    out += encoding;
    out += charStrings;
    out += privateDict;
    return true;
}

} // namespace TinyPdf::Internal::Cff
