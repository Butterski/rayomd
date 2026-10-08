#include "math_parser.h"

#include <cstring>

// TeX-subset math parser. A recursive-descent parser over the source bytes with one cursor; it
// never fails on malformed input (every malformed construct has one defined recovery) and
// answers every hard limit with the source fallback. The functions keep the names of the
// reference parser this file was ported from; docs/development/native_math.md states the subset.

namespace TinyPdf::Internal {
namespace {

#include "math_symbols.inc"

constexpr int kScopeCapacity = 32;        // open {, \left, [ and environments: at most kMathMaxDepth + 2 at a time
constexpr size_t kInlineStack = 128;
constexpr size_t kInlineBraces = 512;
constexpr uint16_t kNoBrace = 0xFFFF;
constexpr size_t kMaxTextBytes = 0xFFFF;  // TextRun and Fallback offsets are 16 bit
constexpr uint8_t kTextFonts[4] = {1, 3, 2, 4};   // text bits (1 bold, 2 italic) -> /M1 /M3 /M2 /M4
constexpr uint8_t kFontRoman = 1;
constexpr uint8_t kFontBold = 3;
constexpr uint8_t kFontSymbol = 5;
constexpr uint8_t kPrimeCode = 0xA2;      // Symbol minute
constexpr uint8_t kEllipsisCode = 0xBC;   // Times ellipsis

constexpr int FindCommand(std::string_view name) {
    int low = 0;
    int high = kMathCommandCount - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        const int order = name.compare(kMathCommandNames + kMathCommands[middle].name);
        if (order == 0) return middle;
        if (order < 0) high = middle - 1;
        else low = middle + 1;
    }
    return -1;
}

constexpr int kCommandCdots = FindCommand("cdots");
constexpr int kCommandLdots = FindCommand("ldots");
constexpr int kCommandIn = FindCommand("in");
constexpr int kCommandSubset = FindCommand("subset");
constexpr int kCommandNeq = FindCommand("neq");
constexpr int kCommandNotIn = FindCommand("notin");
constexpr int kCommandNotSubset = FindCommand("nsubset");
constexpr int kCommandPrime = FindCommand("prime");
static_assert(kCommandCdots >= 0 && kCommandLdots >= 0 && kCommandIn >= 0 && kCommandSubset >= 0 &&
              kCommandNeq >= 0 && kCommandNotIn >= 0 && kCommandNotSubset >= 0 && kCommandPrime >= 0,
              "math_symbols.inc lacks a control word the parser refers to");
static_assert(kMathCommands[kCommandPrime].a == kPrimeCode && (kMathCommands[kCommandPrime].b & 7) == kFontSymbol &&
              kMathCommands[kCommandLdots].a == kEllipsisCode && (kMathCommands[kCommandLdots].b & 7) == kFontRoman,
              "the prime and ellipsis codes of the parser differ from math_symbols.inc");

bool IsSpace(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D); }
bool IsControl(unsigned char c) { return c == 0x7F || (c < 0x20 && !(c >= 0x09 && c <= 0x0D)); }
bool IsLetter(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }

std::string_view Trim(std::string_view value) {
    while (!value.empty() && IsSpace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && IsSpace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

// The character at s[i], i < end. A byte that does not begin a well-formed UTF-8 sequence inside
// [i, end) reads as U+FFFD with length 1, so decoding resumes at the next byte.
uint32_t Decode(const char* s, size_t i, size_t end, size_t& length) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    length = 1;
    if (lead < 0x80) return lead;
    size_t extra = 0;
    uint32_t code = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        code = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        code = lead & 0x0Fu;
        if (lead == 0xE0) low = 0xA0;
        if (lead == 0xED) high = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        code = lead & 0x07u;
        if (lead == 0xF0) low = 0x90;
        if (lead == 0xF4) high = 0x8F;
    } else {
        return 0xFFFD;
    }
    for (size_t k = 1; k <= extra; k++) {
        if (i + k >= end) return 0xFFFD;
        const unsigned char next = static_cast<unsigned char>(s[i + k]);
        if (next < low || next > high) return 0xFFFD;
        code = (code << 6) | (next & 0x3Fu);
        low = 0x80;
        high = 0xBF;
    }
    length = extra + 1;
    return code;
}

void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

const MathUnicodeEntry* FindUnicode(uint32_t code) {
    int low = 0;
    int high = static_cast<int>(sizeof(kMathUnicode) / sizeof(kMathUnicode[0])) - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        if (kMathUnicode[middle].cp == code) return &kMathUnicode[middle];
        if (kMathUnicode[middle].cp > code) high = middle - 1;
        else low = middle + 1;
    }
    return nullptr;
}

const MathEnvironment* FindEnvironment(std::string_view name) {
    int low = 0;
    int high = static_cast<int>(sizeof(kMathEnvironments) / sizeof(kMathEnvironments[0])) - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        const int order = name.compare(kMathEnvNames + kMathEnvironments[middle].name);
        if (order == 0) return &kMathEnvironments[middle];
        if (order < 0) high = middle - 1;
        else low = middle + 1;
    }
    return nullptr;
}

// MathDelimiter of a control word accepted after \left, \right, \middle and \big..; -1 otherwise.
int DelimiterOfCommand(int command) {
    for (const MathDelimWord& word : kMathDelimWords) {
        if (word.command == command) return word.delimiter;
    }
    return -1;
}

// Class a control word has as a symbol, for \dots; -1 when it is not a symbol.
int CommandClass(int command) {
    const MathCommand& entry = kMathCommands[command];
    if (entry.kind == kCmdGlyph || entry.kind == kCmdComposite) return (entry.b >> 3) & 7;
    return entry.kind == kCmdNegated ? static_cast<int>(MathClass::Rel) : -1;
}

struct DimenUnit { char name[3]; bool absolute; uint16_t numerator; uint16_t denominator; };
// Relative units in 1/1000 em, absolute ones in 1/100 pt.
constexpr DimenUnit kDimenUnits[] = {
    {"em", false, 1000, 1}, {"ex", false, 450, 1}, {"mu", false, 1000, 18}, {"pt", true, 100, 1},
    {"bp", true, 100, 1}, {"pc", true, 1200, 1}, {"mm", true, 28346, 100}, {"cm", true, 28346, 10},
    {"in", true, 7200, 1}};

// [sign] digits [. digits] unit, spaces ignored; at most 12 characters of number. The value is
// rounded half away from zero and clamped. No floating point is involved.
bool ParseDimen(std::string_view raw, bool& absolute, int& value) {
    size_t i = 0;
    const size_t size = raw.size();
    auto skipSpaces = [&]() { while (i < size && raw[i] == ' ') i++; };
    bool negative = false;
    for (skipSpaces(); i < size && (raw[i] == '+' || raw[i] == '-'); skipSpaces()) {
        if (raw[i++] == '-') negative = !negative;
    }
    uint64_t mantissa = 0;
    uint64_t scale = 1;
    int length = 0;
    bool seenDot = false;
    bool anyDigit = false;
    for (; i < size; skipSpaces()) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (IsDigit(c)) {
            anyDigit = true;
            if (length < 12) {
                mantissa = mantissa * 10 + (c - '0');
                if (seenDot) scale *= 10;
            }
        } else if (c == '.' && !seenDot) {
            seenDot = true;
        } else {
            break;
        }
        length++;
        i++;
    }
    char unit[2] = {0, 0};
    int unitLength = 0;
    for (; i < size; skipSpaces()) {
        if (unitLength == 2) return false;
        unit[unitLength++] = raw[i++];
    }
    if (!anyDigit || length > 12 || unitLength != 2) return false;
    for (const DimenUnit& entry : kDimenUnits) {
        if (entry.name[0] != unit[0] || entry.name[1] != unit[1]) continue;
        const uint64_t divisor = entry.denominator * scale;
        uint64_t rounded = (2 * mantissa * entry.numerator + divisor) / (2 * divisor);
        const uint64_t limit = entry.absolute ? kMathMaxSpaceCentiPt : kMathMaxSpaceMilliEm;
        if (rounded > limit) rounded = limit;
        absolute = entry.absolute;
        value = negative ? -static_cast<int>(rounded) : static_cast<int>(rounded);
        return true;
    }
    return false;
}

// Scratch stack of node indices: the items of every list under construction, innermost on top.
struct IndexStack {
    uint16_t local[kInlineStack];
    std::vector<uint16_t> heap;
    uint16_t* data = local;
    size_t size = 0;
    size_t capacity = kInlineStack;

    void Push(uint16_t value) {
        if (size == capacity) {
            std::vector<uint16_t> larger(capacity * 4);
            std::memcpy(larger.data(), data, size * sizeof(uint16_t));
            heap.swap(larger);
            data = heap.data();
            capacity *= 4;
        }
        data[size++] = value;
    }
};

enum : uint8_t { kTokEof, kTokChar, kTokSymbol, kTokWord };
enum : uint8_t { kRowsCentre, kRowsPairs, kRowsDetect };

struct Token {
    uint32_t start = 0xFFFFFFFFu;
    uint32_t end = 0;
    uint32_t limit = 0;       // end of the range the token was read in
    uint32_t value = 0;       // character or control symbol: its code point (0 = nothing to typeset)
    int16_t command = -1;     // control word: index into kMathCommands, -1 = unknown
    uint8_t kind = kTokEof;
};

struct RowTag {
    uint16_t head = 0;
    bool claimed = false;     // the row has a \tag (the first one wins)
    bool star = false;
};

struct Rows {
    uint16_t head = 0;        // first Row, or the list itself when not an array
    RowTag tag;               // tag of the single row when not an array
    bool array = false;
    uint8_t maxCells = 0;
    uint8_t rulesBelow = 0;
};

struct ColumnSpec {
    int count = 0;            // columns; 0 = no specification
    int ruleCount = 0;
    uint8_t align[kMathMaxColumns];
    uint16_t rules[kMathMaxColumns + 1];   // vertical rules before column i; [count] = after the last one
};

struct Scope {
    uint8_t groupFloor, leftFloor, optionalFloor;
    bool closeOnBrace;
};

struct Checkpoint {
    size_t nodes, text;
    int orphans;
};

class Parser {
public:
    Parser(std::string_view tex, bool display, bool hasFallback, MathParseResult& out)
        : s_(tex.data()), size_(static_cast<uint32_t>(tex.size())), end_(size_), display_(display),
          hasFallback_(hasFallback), out_(out) {}

    bool ParseFormula();
    void SourceFallback(std::string_view tex);

private:
    // ---- arena ---------------------------------------------------------------------------
    MathNode& N(uint16_t index) { return out_.nodes[index]; }
    void Fail() { failed_ = true; }
    void Push(uint16_t node) { stack_.Push(node); }

    uint16_t NewNode(MathNodeKind kind, MathClass cls = MathClass::None) {
        if (failed_ || out_.nodes.size() > static_cast<size_t>(kMathMaxNodes)) {
            Fail();
            return 0;
        }
        out_.nodes.emplace_back();
        MathNode& node = out_.nodes.back();
        node.kind = kind;
        node.cls = cls;
        return static_cast<uint16_t>(out_.nodes.size() - 1);
    }

    uint16_t NewGlyph(MathClass cls, uint8_t font, uint32_t code, uint8_t flags) {
        const uint16_t node = NewNode(MathNodeKind::Glyph, cls);
        N(node).aux = font;
        N(node).a = static_cast<uint16_t>(code);
        N(node).flags = flags;
        return node;
    }

    uint16_t NewSpace(int width, int displayWidth = 0) {
        const uint16_t node = NewNode(MathNodeKind::Space);
        N(node).a = static_cast<uint16_t>(static_cast<int16_t>(width));
        N(node).b = static_cast<uint16_t>(displayWidth);
        return node;
    }

    // An atom with one child list (Group, Accent, Enclose, Phantom, Text ...).
    uint16_t NewUnary(MathNodeKind kind, MathClass cls, uint16_t body, uint8_t aux = 0, uint8_t flags = 0) {
        const uint16_t node = NewNode(kind, cls);
        N(node).child[0] = body;
        N(node).aux = aux;
        N(node).flags = flags;
        return node;
    }

    // Links the stack entries from `mark` up into one list and removes them from the stack.
    uint16_t LinkFrom(size_t mark) {
        uint16_t head = 0;
        while (stack_.size > mark) {
            const uint16_t node = stack_.data[--stack_.size];
            N(node).next = head;
            head = node;
        }
        return head;
    }

    void PushList(uint16_t head) {
        while (head) {
            const uint16_t next = N(head).next;
            Push(head);
            head = next;
        }
    }

    uint16_t AtomOf(uint16_t item) { return N(item).kind == MathNodeKind::Scripts ? N(item).child[0] : item; }

    void SetClass(uint16_t item, MathClass cls) {
        N(item).cls = cls;
        if (N(item).kind == MathNodeKind::Scripts) N(N(item).child[0]).cls = cls;
    }

    uint16_t Wrap(uint16_t atom, uint8_t limitsMode = kLimitsDefault) {
        const uint16_t scripts = NewNode(MathNodeKind::Scripts, N(atom).cls);
        N(scripts).flags = N(atom).flags & kOpDisplayLimits;
        N(scripts).a = limitsMode;
        N(scripts).child[0] = atom;
        return scripts;
    }

    // Takes a node out of the tree for good; Compact removes it at the end.
    void Orphan(uint16_t node) {
        N(node).kind = MathNodeKind::Null;
        orphans_++;
    }

    Checkpoint Mark() const { return {out_.nodes.size(), out_.text.size(), orphans_}; }
    // Drops everything parsed since `mark`. Valid only when nothing older refers to it.
    void Rollback(const Checkpoint& mark) {
        out_.nodes.resize(mark.nodes);
        out_.text.resize(mark.text);
        orphans_ = mark.orphans;
    }

    // ---- scopes --------------------------------------------------------------------------
    int BraceDepth() const { return groupTop_ - groupFloor_; }
    int LeftDepth() const { return leftTop_ - leftFloor_; }

    Scope EnterScope(bool closeOnBrace) {
        const Scope saved = {groupFloor_, leftFloor_, optionalFloor_, closeOnBrace_};
        groupFloor_ = groupTop_;
        leftFloor_ = leftTop_;
        optionalFloor_ = optionalTop_;
        closeOnBrace_ = closeOnBrace;
        return saved;
    }

    void LeaveScope(const Scope& saved) {
        groupTop_ = groupFloor_;
        leftTop_ = leftFloor_;
        optionalTop_ = optionalFloor_;
        groupFloor_ = saved.groupFloor;
        leftFloor_ = saved.leftFloor;
        optionalFloor_ = saved.optionalFloor;
        closeOnBrace_ = saved.closeOnBrace;
    }

    void Enter() { if (++depth_ > kMathMaxDepth) Fail(); }
    void Leave() { depth_--; }

    int MatchingBrace(uint32_t open) const {
        if (!closeOf_ || closeOf_[open] == kNoBrace || closeOf_[open] >= end_) return -1;
        return closeOf_[open];
    }

    bool InBalancedGroup() const {
        return groupTop_ > groupFloor_ && MatchingBrace(groupOpen_[groupTop_ - 1] - 1u) >= 0;
    }

    int Find(char c, uint32_t from, uint32_t window) const {
        const uint32_t to = end_ - from < window ? end_ : from + window;
        const void* found = std::memchr(s_ + from, c, to - from);
        return found ? static_cast<int>(static_cast<const char*>(found) - s_) : -1;
    }

    // ---- lexer ---------------------------------------------------------------------------
    void MatchBraces();
    void SkipWs();
    const Token& Peek();
    Token Take();
    bool TakeStar();
    bool PeekEnvName(uint32_t from, std::string_view& name, uint32_t& after) const;
    bool IsRowSep(const Token& token) const;
    bool IsStop(const Token& token) const;
    static bool IsChar(const Token& token, uint32_t c) { return token.kind == kTokChar && token.value == c; }
    static uint8_t KindOf(const Token& token) {
        return token.kind == kTokWord && token.command >= 0 ? kMathCommands[token.command].kind
                                                            : static_cast<uint8_t>(kCmdKindCount);
    }

    // ---- math mode -----------------------------------------------------------------------
    void ParseItems(bool leadEmpty);
    uint16_t ParseList();
    uint16_t ScriptBase(size_t mark);
    void AttachScript(size_t mark, int slot, uint16_t arg);
    void AddPrimes(size_t mark, int count);
    void ApplyLimits(size_t mark, uint8_t mode);
    void ResolveBins(size_t mark);
    uint16_t ParseGroupBody();
    uint16_t ParseArg();
    bool ParseOptional(uint16_t& body);
    std::string_view RawBraced();
    std::string_view RawDimen();
    int ParseDelim();
    uint16_t DelimiterAtom(int delimiter, MathClass cls);
    uint16_t ParsePrimary();
    uint16_t AsciiNode(unsigned char c);
    uint16_t CharNode(uint32_t code);
    uint16_t ControlSymbol(uint32_t code);
    uint16_t Command(int index);
    uint16_t OpName(std::string_view name, MathClass cls, uint8_t flags, size_t split);
    uint16_t ArgWithVariant(uint8_t variant);
    int PeekClass(const Token& token) const;
    uint16_t ParseNot();
    uint16_t ParseMod(int kind);
    uint16_t SpaceFromDimen(std::string_view raw);
    uint16_t ParseLeftRight();
    void ParseRows(uint8_t mode, bool closeOnBrace, bool always, Rows& result);
    uint16_t NewRow(uint16_t cells, const RowTag& tag, int rules);
    void SkipRowSepDimen();
    uint16_t ParseSubstack();
    void ParseColSpec(std::string_view spec, ColumnSpec& columns);
    uint16_t ParseEnvironment();
    uint16_t MakeArray(MathClass cls, int left, int right, uint8_t aux, uint16_t gap, const Rows& rows,
                       uint8_t pattern, uint8_t align, const ColumnSpec& spec);

    // ---- text mode -----------------------------------------------------------------------
    uint32_t FindTextEnd(uint32_t start) const;
    void TextByte(size_t mark, uint8_t bits, uint32_t byte);
    void TextWord(size_t mark, uint8_t bits, const char* word, size_t size);
    void PlainText(size_t mark, uint8_t bits, std::string_view text);
    void ParseTextArg(uint8_t bits, size_t mark);
    void ParseText(uint8_t bits, size_t mark);
    uint16_t MathInText(uint32_t start, uint32_t close);
    void TextControl(uint8_t& bits, size_t mark);

    void Compact();

    const char* s_;
    uint32_t size_;
    uint32_t pos_ = 0;
    uint32_t end_;
    bool display_;
    bool hasFallback_;
    bool percentComment_ = false;
    bool failed_ = false;
    bool closeOnBrace_ = false;   // \substack body: '}' at brace depth 0 closes the scope
    bool inOpName_ = false;
    uint8_t variant_ = kVariantNormal;
    int depth_ = 0;
    int orphans_ = 0;             // nodes taken out of the tree; removed by Compact
    int rowRules_ = 0;            // \hline count of the row being parsed
    RowTag rowTag_;
    MathParseResult& out_;
    Token token_;
    const uint16_t* closeOf_ = nullptr;   // position of the '}' matching the '{' at each byte
    // Per cell scope (formula, environment body, \substack, math inside text): only the entries
    // above the floors belong to the current scope.
    uint8_t groupTop_ = 0, groupFloor_ = 0;
    uint8_t leftTop_ = 0, leftFloor_ = 0;
    uint8_t optionalTop_ = 0, optionalFloor_ = 0;
    uint8_t envTop_ = 0, envFloor_ = 0;
    uint16_t groupOpen_[kScopeCapacity];      // position after each open '{'
    uint8_t leftBraceBase_[kScopeCapacity];   // brace depth at which each open \left started
    uint8_t optionalBrace_[kScopeCapacity];   // brace and \left depth of each open [..] argument
    uint8_t optionalLeft_[kScopeCapacity];
    std::string_view envNames_[kScopeCapacity];
    IndexStack stack_;
    uint16_t bracesLocal_[kInlineBraces];
    std::vector<uint16_t> bracesHeap_;
};

// ------------------------------------------------------------------------------------------
// Lexer
// ------------------------------------------------------------------------------------------

// One pass: closeOf_[p] = position of the '}' matching the '{' at p. A backslash escapes the next
// byte. Every "is this group balanced" question is answered from this table in O(1).
void Parser::MatchBraces() {
    if (!size_ || !std::memchr(s_, '{', size_)) return;
    uint16_t* table = bracesLocal_;
    if (size_ > kInlineBraces) {
        bracesHeap_.resize(size_);
        table = bracesHeap_.data();
    }
    std::memset(table, 0xFF, size_ * sizeof(uint16_t));
    for (uint32_t i = 0; i < size_; i++) {
        const char c = s_[i];
        if (c == '\\') i++;
        else if (c == '{') stack_.Push(static_cast<uint16_t>(i));
        else if (c == '}' && stack_.size) table[stack_.data[--stack_.size]] = static_cast<uint16_t>(i);
    }
    stack_.size = 0;
    closeOf_ = table;
}

void Parser::SkipWs() {
    while (pos_ < end_) {
        const unsigned char c = static_cast<unsigned char>(s_[pos_]);
        if (IsSpace(c) || IsControl(c)) {
            pos_++;
        } else if (c == '%' && percentComment_) {
            while (pos_ < end_ && s_[pos_] != '\n') pos_++;
        } else {
            break;
        }
    }
}

const Token& Parser::Peek() {
    SkipWs();
    if (token_.start == pos_ && token_.limit == end_) return token_;
    token_.start = pos_;
    token_.end = pos_;
    token_.limit = end_;
    token_.value = 0;
    token_.command = -1;
    token_.kind = kTokEof;
    if (pos_ >= end_) return token_;
    size_t length = 1;
    if (s_[pos_] != '\\') {
        token_.kind = kTokChar;
        token_.value = Decode(s_, pos_, end_, length);
        token_.end = pos_ + static_cast<uint32_t>(length);
        return token_;
    }
    token_.kind = kTokSymbol;
    token_.end = pos_ + 1;
    if (pos_ + 1 >= end_) return token_;                    // a backslash at the end: ignored
    const unsigned char next = static_cast<unsigned char>(s_[pos_ + 1]);
    if (IsLetter(next)) {
        uint32_t stop = pos_ + 1;
        while (stop < end_ && IsLetter(static_cast<unsigned char>(s_[stop]))) stop++;
        token_.kind = kTokWord;
        token_.end = stop;
        token_.command = static_cast<int16_t>(FindCommand(std::string_view(s_ + pos_ + 1, stop - pos_ - 1)));
    } else if (IsControl(next)) {
        token_.end = pos_ + 2;                              // the backslash goes with a control byte
    } else {
        token_.value = Decode(s_, pos_ + 1, end_, length);
        token_.end = pos_ + 1 + static_cast<uint32_t>(length);
    }
    return token_;
}

Token Parser::Take() {
    const Token token = Peek();
    pos_ = token.end;
    return token;
}

// A star belongs to a command only when it follows it immediately.
bool Parser::TakeStar() {
    if (pos_ >= end_ || s_[pos_] != '*') return false;
    pos_++;
    return true;
}

// The name in \begin{..} or \end{..}, looked for at `from`, without consuming anything.
bool Parser::PeekEnvName(uint32_t from, std::string_view& name, uint32_t& after) const {
    uint32_t open = from;
    while (open < end_ && (IsSpace(static_cast<unsigned char>(s_[open])) || IsControl(static_cast<unsigned char>(s_[open])))) {
        open++;
    }
    if (open >= end_ || s_[open] != '{') return false;
    uint32_t close = open + 1;
    while (close < end_ && s_[close] != '}' && close - open <= static_cast<uint32_t>(kMathMaxEnvName) + 1) close++;
    if (close >= end_ || s_[close] != '}') return false;
    name = Trim(std::string_view(s_ + open + 1, close - open - 1));
    after = close + 1;
    return true;
}

bool Parser::IsRowSep(const Token& token) const {
    return (token.kind == kTokSymbol && token.value == '\\') || KindOf(token) == kCmdRowSep;
}

// True when the token ends the current list without being consumed by it: every enclosing
// construct sees it again and closes itself until its owner takes it.
bool Parser::IsStop(const Token& token) const {
    if (token.kind == kTokEof) return true;
    if (token.kind == kTokChar) {
        if (token.value == '&') return !InBalancedGroup();
        if (token.value == '}') return BraceDepth() > 0 || closeOnBrace_;
        if (token.value == ']') {
            return optionalTop_ > optionalFloor_ && optionalBrace_[optionalTop_ - 1] == BraceDepth() &&
                optionalLeft_[optionalTop_ - 1] == LeftDepth();
        }
        return false;
    }
    if (IsRowSep(token)) return !InBalancedGroup();
    switch (KindOf(token)) {
    case kCmdRight:
        return LeftDepth() > 0;
    case kCmdMiddle:
        return LeftDepth() > 0 && leftBraceBase_[leftTop_ - 1] == BraceDepth();
    case kCmdEnd: {
        std::string_view name;
        uint32_t after = 0;
        if (!PeekEnvName(token.end, name, after)) return false;
        for (int i = envFloor_; i < envTop_; i++) {
            if (envNames_[i] == name) return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// ------------------------------------------------------------------------------------------
// Lists
// ------------------------------------------------------------------------------------------

// Items up to (not including) a stop token, left on the stack. Applies an infix fraction and the
// Bin -> Ord rule. leadEmpty: start with an empty Ord atom (the {} amsmath puts into
// left-aligned alignment cells, so that a leading + or = keeps its spacing).
void Parser::ParseItems(bool leadEmpty) {
    Enter();
    const uint8_t savedVariant = variant_;
    const size_t mark = stack_.size;
    int infix = -1;
    uint16_t numerator = 0;
    if (leadEmpty) Push(NewNode(MathNodeKind::Group, MathClass::Ord));
    while (!failed_) {
        const Token token = Peek();
        if (IsStop(token)) break;
        if (token.kind == kTokChar) {
            if (token.value == '}' || token.value == '&') {     // stray brace; separator in a balanced group
                Take();
                continue;
            }
            if (token.value == '^' || token.value == '_') {
                Take();
                const uint16_t arg = ParseArg();
                AttachScript(mark, token.value == '^' ? 2 : 1, arg);
                continue;
            }
            if (token.value == '\'') {
                int count = 0;
                while (IsChar(Peek(), '\'')) {
                    Take();
                    count++;
                }
                AddPrimes(mark, count);
                continue;
            }
            const MathUnicodeEntry* special = token.value > 0x7F ? FindUnicode(token.value) : nullptr;
            if (special && special->kind == kUniPrime) {
                Take();
                AddPrimes(mark, special->arg);
                continue;
            }
            if (special && (special->kind == kUniSup || special->kind == kUniSub)) {
                // a run of Unicode superscript (subscript) characters is one script
                const uint8_t which = special->kind;
                const size_t scriptMark = stack_.size;
                for (;;) {
                    const Token next = Peek();
                    const MathUnicodeEntry* entry =
                        next.kind == kTokChar && next.value > 0x7F ? FindUnicode(next.value) : nullptr;
                    if (!entry || entry->kind != which) break;
                    Take();
                    const uint16_t node = AsciiNode(entry->arg);
                    if (node) Push(node);
                }
                ResolveBins(scriptMark);
                const uint16_t arg = LinkFrom(scriptMark);
                AttachScript(mark, which == kUniSup ? 2 : 1, arg);
                continue;
            }
        } else if (IsRowSep(token)) {                           // inside a balanced brace group: dropped
            Take();
            continue;
        } else if (token.kind == kTokWord && token.command >= 0) {
            const MathCommand& command = kMathCommands[token.command];
            switch (command.kind) {
            case kCmdRight:                                     // without \left: its delimiter at normal size
            case kCmdMiddle: {
                Take();
                const uint16_t node =
                    DelimiterAtom(ParseDelim(), command.kind == kCmdRight ? MathClass::Close : MathClass::Ord);
                if (node) Push(node);
                continue;
            }
            case kCmdEnd: {                                     // matches no open environment: dropped with its name
                std::string_view name;
                uint32_t after = 0;
                const bool named = PeekEnvName(token.end, name, after);
                Take();
                if (named) pos_ = after;
                continue;
            }
            case kCmdLimits:
                Take();
                ApplyLimits(mark, command.a);
                continue;
            case kCmdInfix:
                Take();
                if (infix < 0) {                                // a second one in the same list is ignored
                    infix = token.command;
                    ResolveBins(mark);
                    numerator = LinkFrom(mark);
                }
                continue;
            case kCmdFontDecl:
                Take();
                variant_ = command.a;
                continue;
            case kCmdHline:
                Take();
                rowRules_++;
                continue;
            case kCmdTag: {
                Take();
                const bool star = TakeStar();
                const size_t tagMark = stack_.size;
                if (!rowTag_.claimed) {
                    rowTag_.claimed = true;
                    rowTag_.star = star;
                    ParseTextArg(0, tagMark);
                    rowTag_.head = LinkFrom(tagMark);
                } else {                                        // a later tag of the row: parsed and dropped
                    const Checkpoint checkpoint = Mark();
                    ParseTextArg(0, tagMark);
                    stack_.size = tagMark;
                    Rollback(checkpoint);
                }
                continue;
            }
            default:
                break;
            }
        }
        const uint16_t node = ParsePrimary();
        if (node) Push(node);
    }
    ResolveBins(mark);
    if (infix >= 0) {
        const uint16_t denominator = LinkFrom(mark);
        const uint16_t fraction = NewNode(MathNodeKind::Fraction, MathClass::Ord);
        N(fraction).flags = kMathCommands[infix].a;
        if (kMathCommands[infix].b) {
            N(fraction).a = kDelimLParen;
            N(fraction).b = kDelimRParen;
        }
        N(fraction).child[0] = numerator;
        N(fraction).child[1] = denominator;
        Push(fraction);
    }
    variant_ = savedVariant;
    Leave();
}

uint16_t Parser::ParseList() {
    const size_t mark = stack_.size;
    ParseItems(false);
    return LinkFrom(mark);
}

// The Scripts node of the last item of the list; scripts with nothing before them get an empty
// Ord nucleus, as in TeX.
uint16_t Parser::ScriptBase(size_t mark) {
    if (stack_.size == mark || N(stack_.data[stack_.size - 1]).cls == MathClass::None) {
        Push(NewNode(MathNodeKind::Group, MathClass::Ord));
    }
    uint16_t& top = stack_.data[stack_.size - 1];
    if (N(top).kind != MathNodeKind::Scripts) top = Wrap(top);
    return top;
}

// slot: 1 subscript, 2 superscript (the child index of the Scripts node).
void Parser::AttachScript(size_t mark, int slot, uint16_t arg) {
    if (!arg) return;                                           // empty or missing argument: nothing attached
    uint16_t base = ScriptBase(mark);
    if (N(base).child[slot]) {                                  // double script: TeX's recovery x^a{}^b
        Push(NewNode(MathNodeKind::Group, MathClass::Ord));
        base = ScriptBase(mark);
    }
    if (slot == 2) {
        // Leading bare \prime atoms become primes: the Symbol prime glyph is already raised.
        while (arg && N(arg).kind == MathNodeKind::Glyph && N(arg).aux == kFontSymbol && N(arg).a == kPrimeCode) {
            if (N(base).aux < kMathMaxPrimes) N(base).aux++;
            const uint16_t prime = arg;
            arg = N(arg).next;
            Orphan(prime);
        }
        if (!arg) return;
    }
    N(base).child[slot] = arg;
}

void Parser::AddPrimes(size_t mark, int count) {
    const uint16_t base = ScriptBase(mark);
    const int total = N(base).aux + count;
    N(base).aux = static_cast<uint8_t>(total < kMathMaxPrimes ? total : kMathMaxPrimes);
}

// \limits, \nolimits, \displaylimits directly after an Op atom; after anything else: ignored.
void Parser::ApplyLimits(size_t mark, uint8_t mode) {
    if (stack_.size == mark) return;
    uint16_t& top = stack_.data[stack_.size - 1];
    if (N(top).cls != MathClass::Op) return;
    if (N(top).kind != MathNodeKind::Scripts) {
        if (mode != kLimitsDefault) top = Wrap(top, mode);
        return;
    }
    N(top).a = mode;
    if (mode == kLimitsDefault && !N(top).child[1] && !N(top).child[2] && !N(top).aux) {
        const uint16_t carrier = top;                           // the node carried only the mode
        top = N(carrier).child[0];
        Orphan(carrier);
    }
}

// TeX Appendix G rules 5 and 6, on the finished list. Non-atoms are skipped; a Middle delimiter
// is a list boundary.
void Parser::ResolveBins(size_t mark) {
    uint16_t previous = 0;
    for (size_t i = mark; i < stack_.size; i++) {
        const uint16_t item = stack_.data[i];
        const MathClass cls = N(item).cls;
        if (N(item).kind == MathNodeKind::Middle) {
            if (previous && N(previous).cls == MathClass::Bin) SetClass(previous, MathClass::Ord);
            previous = 0;
            continue;
        }
        if (cls == MathClass::None) continue;
        const MathClass before = previous ? N(previous).cls : MathClass::None;
        if (cls == MathClass::Bin) {
            if (!previous || before == MathClass::Bin || before == MathClass::Op || before == MathClass::Rel ||
                before == MathClass::Open || before == MathClass::Punct) {
                SetClass(item, MathClass::Ord);
            }
        } else if (before == MathClass::Bin &&
                   (cls == MathClass::Rel || cls == MathClass::Close || cls == MathClass::Punct)) {
            SetClass(previous, MathClass::Ord);
        }
        previous = item;
    }
    if (previous && N(previous).cls == MathClass::Bin) SetClass(previous, MathClass::Ord);
}

// ------------------------------------------------------------------------------------------
// Arguments
// ------------------------------------------------------------------------------------------

// After '{' has been consumed: the body, then the matching '}' when it is there.
uint16_t Parser::ParseGroupBody() {
    if (groupTop_ < kScopeCapacity) groupOpen_[groupTop_++] = static_cast<uint16_t>(pos_);
    else Fail();
    const uint16_t body = ParseList();
    if (groupTop_ > groupFloor_) groupTop_--;
    if (IsChar(Peek(), '}')) Take();
    return body;
}

// One argument unit: a brace group or a single primary, never with scripts. Missing: empty.
uint16_t Parser::ParseArg() {
    const Token token = Peek();
    if (IsStop(token)) return 0;
    if (token.kind == kTokChar) {
        if (token.value == '^' || token.value == '_' || token.value == '\'' || token.value == '}') return 0;
        if (token.value == '{') {
            Take();
            return ParseGroupBody();
        }
    }
    switch (KindOf(token)) {
    case kCmdRight: case kCmdMiddle: case kCmdEnd: case kCmdLimits: case kCmdInfix: case kCmdHline: case kCmdTag:
        return 0;
    default:
        break;
    }
    Enter();
    const uint16_t node = ParsePrimary();
    Leave();
    if (node && N(node).cls == MathClass::Bin) SetClass(node, MathClass::Ord);   // a one-atom list: e^-, x^*
    return node;
}

// [ list ] when the next token is '['. It ends at the first ']' at the depth of its '['.
bool Parser::ParseOptional(uint16_t& body) {
    if (!IsChar(Peek(), '[')) return false;
    Take();
    if (optionalTop_ < kScopeCapacity) {
        optionalBrace_[optionalTop_] = static_cast<uint8_t>(BraceDepth());
        optionalLeft_[optionalTop_++] = static_cast<uint8_t>(LeftDepth());
    } else {
        Fail();
    }
    body = ParseList();
    if (optionalTop_ > optionalFloor_) optionalTop_--;
    if (IsChar(Peek(), ']')) Take();
    return true;
}

// {...} taken as raw bytes (brace-balanced, at most kMathMaxRawArgBytes of it); empty when absent.
std::string_view Parser::RawBraced() {
    if (!IsChar(Peek(), '{')) return std::string_view();
    const uint32_t start = pos_ + 1;
    const uint32_t close = FindTextEnd(start);
    pos_ = close < end_ && s_[close] == '}' ? close + 1 : close;
    const uint32_t length = close - start;
    return std::string_view(s_ + start, length < static_cast<uint32_t>(kMathMaxRawArgBytes) ? length : kMathMaxRawArgBytes);
}

// An unbraced dimension after \kern and friends: sign, digits, a point, spaces, two letters.
std::string_view Parser::RawDimen() {
    SkipWs();
    uint32_t stop = pos_;
    while (stop < end_ && (IsDigit(static_cast<unsigned char>(s_[stop])) || s_[stop] == '+' || s_[stop] == '-' ||
                           s_[stop] == '.' || s_[stop] == ' ')) {
        stop++;
    }
    const uint32_t unit = stop;
    while (stop < end_ && stop - unit < 2 && IsLetter(static_cast<unsigned char>(s_[stop]))) stop++;
    const std::string_view raw(s_ + pos_, stop - pos_);
    pos_ = stop;
    return raw;
}

// The delimiter after \left, \right, \middle and \big..; anything else is not consumed and
// gives the null delimiter.
int Parser::ParseDelim() {
    const Token token = Peek();
    int delimiter = -1;
    if (token.kind == kTokChar) {
        switch (token.value) {
        case '(': delimiter = kDelimLParen; break;
        case ')': delimiter = kDelimRParen; break;
        case '[': delimiter = kDelimLBrack; break;
        case ']': delimiter = kDelimRBrack; break;
        case '|': delimiter = kDelimVert; break;
        case '/': delimiter = kDelimSlash; break;
        case '.': delimiter = kDelimNone; break;
        case '<': delimiter = kDelimLAngle; break;
        case '>': delimiter = kDelimRAngle; break;
        default: {
            const MathUnicodeEntry* entry = token.value > 0x7F ? FindUnicode(token.value) : nullptr;
            if (entry && entry->kind == kUniCommand) delimiter = DelimiterOfCommand(entry->value);
            break;
        }
        }
    } else if (token.kind == kTokSymbol) {
        if (token.value == '{') delimiter = kDelimLBrace;
        else if (token.value == '}') delimiter = kDelimRBrace;
        else if (token.value == '|') delimiter = kDelimDblVert;
    } else if (token.kind == kTokWord && token.command >= 0) {
        delimiter = DelimiterOfCommand(token.command);
    }
    if (delimiter < 0) return kDelimNone;
    Take();
    return delimiter;
}

// A delimiter used bare, at its natural size: a glyph, a composite, or the vector shape.
uint16_t Parser::DelimiterAtom(int delimiter, MathClass cls) {
    const MathDelimBaseEntry& base = kMathDelimBase[delimiter];
    if (base.base == kDelimBaseNone) return 0;
    if (base.base == kDelimBaseGlyph) return NewGlyph(cls, base.font, base.code, 0);
    const uint16_t node = NewNode(base.base == kDelimBaseVector ? MathNodeKind::SizedDelim : MathNodeKind::Composite, cls);
    N(node).a = base.base == kDelimBaseVector ? static_cast<uint16_t>(delimiter) : base.code;
    return node;
}

// ------------------------------------------------------------------------------------------
// Primaries
// ------------------------------------------------------------------------------------------

uint16_t Parser::ParsePrimary() {
    if (failed_) return 0;                                      // a hard limit was hit: unwind without recursing
    const Token token = Take();
    switch (token.kind) {
    case kTokChar:
        if (token.value == '{') return NewUnary(MathNodeKind::Group, MathClass::Ord, ParseGroupBody());
        if (token.value == '~') return NewSpace(250);
        return CharNode(token.value);
    case kTokSymbol:
        return ControlSymbol(token.value);
    case kTokWord:
        if (token.command >= 0) return Command(token.command);
        return OpName(std::string_view(s_ + token.start + 1, token.end - token.start - 1), MathClass::Op,
                      kOpNameUnknown, 0);
    default:
        return 0;
    }
}

uint16_t Parser::AsciiNode(unsigned char c) {
    const bool letter = IsLetter(c);
    if (letter || IsDigit(c)) {
        const MathVariant& variant = kMathVariants[variant_];
        return NewGlyph(MathClass::Ord, letter ? variant.letterFont : variant.digitFont, c,
                        letter || !variant.lettersOnly ? variant.latinFlags : 0);
    }
    if ((inOpName_ && (c == '-' || c == '*')) || c == '%') return NewGlyph(MathClass::Ord, kFontRoman, c, 0);
    if (c < 0x20 || c > 0x7E) return 0;
    const MathAsciiEntry& entry = kMathAscii[c - 0x20];
    if (!entry.fontClass) return 0;
    const uint8_t font = entry.fontClass & 7;
    return NewGlyph(static_cast<MathClass>(entry.fontClass >> 3), font, entry.code,
                    variant_ == kVariantBoldItalic && font == kFontSymbol ? kGlyphHeavy : 0);
}

uint16_t Parser::CharNode(uint32_t code) {
    if (code < 0x80) return code == '&' ? 0 : AsciiNode(static_cast<unsigned char>(code));
    if (const MathUnicodeEntry* entry = FindUnicode(code)) {
        switch (entry->kind) {
        case kUniCommand: return Command(entry->value);
        case kUniGlyph: return NewGlyph(static_cast<MathClass>(entry->arg), entry->value >> 8, entry->value & 0xFF, 0);
        case kUniMinus: return AsciiNode('-');
        case kUniBlackboard: return NewGlyph(MathClass::Ord, kFontBold, entry->arg, kGlyphOutline);
        case kUniSpace: return NewSpace(entry->value);
        case kUniIgnore: return 0;
        case kUniPrime: return NewGlyph(MathClass::Ord, kFontSymbol, kPrimeCode, 0);
        default: return AsciiNode(entry->arg);                  // kUniSup, kUniSub outside a run
        }
    }
    if (code == 0x1D7D9 || code == 0x1D55C) {                   // blackboard 1 and k
        return NewGlyph(MathClass::Ord, kFontBold, code == 0x1D7D9 ? '1' : 'k', kGlyphOutline);
    }
    if (!hasFallback_) return NewGlyph(MathClass::Ord, kFontRoman, '?', 0);
    const size_t offset = out_.text.size();
    if (offset + 4 > kMaxTextBytes) {
        Fail();
        return 0;
    }
    AppendUtf8(out_.text, code);
    const uint16_t node = NewNode(MathNodeKind::Fallback, MathClass::Ord);
    N(node).a = static_cast<uint16_t>(offset);
    N(node).b = static_cast<uint16_t>(out_.text.size() - offset);
    return node;
}

// `code` is the character after the backslash: 0 for nothing, else 0x20-0x7E or non-ASCII.
uint16_t Parser::ControlSymbol(uint32_t code) {
    switch (code) {
    case 0:
        return 0;
    case ' ': case '\t': case '\n': case '\v': case '\f': case '\r':
        return NewSpace(250);
    case ',':
        return NewSpace(167);
    case ':': case '>':
        return NewSpace(222);
    case ';':
        return NewSpace(278);
    case '!':
        return NewSpace(-167);
    case '{':
        return DelimiterAtom(kDelimLBrace, MathClass::Open);
    case '}':
        return DelimiterAtom(kDelimRBrace, MathClass::Close);
    case '|':
        return DelimiterAtom(kDelimDblVert, MathClass::Ord);
    case '%': case '$': case '&': case '#': case '_':
        return NewGlyph(MathClass::Ord, variant_ == kVariantBoldItalic ? kFontBold : kFontRoman, code, 0);
    case '(': case ')': case '[': case ']':                     // math delimiters inside math
    case '-': case '/': case '*': case '@': case '"': case '\'': case '^': case '~': case '`': case '=': case '.':
        return 0;                                               // discretionary, italic correction, text accents
    default:
        return CharNode(code);                                  // unknown: the character itself
    }
}

// name[0, split) + thin space + the rest when split is not 0.
uint16_t Parser::OpName(std::string_view name, MathClass cls, uint8_t flags, size_t split) {
    if (out_.text.size() + name.size() > kMaxTextBytes) {
        Fail();
        return 0;
    }
    const size_t mark = stack_.size;
    for (int part = split ? 0 : 1; part < 2; part++) {
        const std::string_view piece = part ? std::string_view(name.data() + split, name.size() - split)
                                            : std::string_view(name.data(), split);
        const uint16_t run = NewNode(MathNodeKind::TextRun);
        N(run).aux = kFontRoman;
        N(run).a = static_cast<uint16_t>(out_.text.size());
        N(run).b = static_cast<uint16_t>(piece.size());
        out_.text.append(piece.data(), piece.size());
        Push(run);
        if (!part) Push(NewSpace(167));
    }
    return NewUnary(MathNodeKind::OpName, cls, LinkFrom(mark), 0, flags);
}

uint16_t Parser::ArgWithVariant(uint8_t variant) {
    const uint8_t saved = variant_;
    variant_ = variant;
    const uint16_t body = ParseArg();
    variant_ = saved;
    return body;
}

uint16_t Parser::Command(int index) {
    const MathCommand& command = kMathCommands[index];
    const uint8_t bigOp = command.c & 7;
    uint8_t flags = command.c & 0x08 ? kOpDisplayLimits : 0;
    switch (command.kind) {
    case kCmdGlyph: {
        uint8_t font = command.b & 7;
        const int greek = command.b >> 6;
        const MathVariant& variant = kMathVariants[variant_];
        if (greek == 1) flags |= variant.lowerGreekFlags;
        else if (greek) flags |= variant.upperGreekFlags | (greek == 3 ? kGlyphSlant : 0);
        else if (variant_ == kVariantBoldItalic && font == kFontSymbol) flags |= kGlyphHeavy;
        else if (variant_ == kVariantBoldItalic && font <= 2) font += 2;    // Times commands move to the bold face
        if (command.c & 0x10) flags |= kAtomPad;
        const uint16_t node = NewGlyph(static_cast<MathClass>((command.b >> 3) & 7), font, command.a, flags);
        N(node).b = bigOp;
        return node;
    }
    case kCmdComposite: {
        const uint16_t node = NewNode(MathNodeKind::Composite, static_cast<MathClass>((command.b >> 3) & 7));
        N(node).flags = flags | (variant_ == kVariantBoldItalic ? kGlyphHeavy : 0);
        N(node).a = command.a;
        N(node).b = bigOp;
        return node;
    }
    case kCmdNegated: {
        const uint16_t node = Command(command.a | command.b << 8);
        N(node).flags |= kAtomNegated;
        return node;
    }
    case kCmdDelimiter:
        return DelimiterAtom(command.a, static_cast<MathClass>(command.b));
    case kCmdOpName:
        return OpName(kMathCommandNames + command.name, MathClass::Op, command.a ? kOpDisplayLimits : 0, command.b);
    case kCmdSpace:
        return NewSpace(static_cast<int16_t>(command.a | command.b << 8));
    case kCmdBlackboard:
        return NewGlyph(MathClass::Ord, kFontBold, command.a, kGlyphOutline);
    case kCmdFraction: {
        const uint16_t numerator = ParseArg();
        const uint16_t denominator = ParseArg();
        const uint16_t node = NewUnary(MathNodeKind::Fraction, MathClass::Ord, numerator, 0, command.a);
        N(node).child[1] = denominator;
        if (command.b) {
            N(node).a = kDelimLParen;
            N(node).b = kDelimRParen;
        }
        return node;
    }
    case kCmdSqrt: {
        uint16_t index2 = 0;
        ParseOptional(index2);
        const uint16_t node = NewUnary(MathNodeKind::Radical, MathClass::Ord, ParseArg());
        N(node).child[1] = index2;
        return node;
    }
    case kCmdAccent:
        return NewUnary(MathNodeKind::Accent, MathClass::Ord, ParseArg(), command.a);
    case kCmdOverUnder: {
        const bool brace = command.a >= kOverBrace;
        const uint16_t node =
            NewUnary(MathNodeKind::OverUnder, brace ? MathClass::Op : MathClass::Ord, ParseArg(), command.a);
        return brace ? Wrap(node, kLimitsOn) : node;            // the braces take their scripts as limits
    }
    case kCmdMathFont:
        return NewUnary(MathNodeKind::Group, MathClass::Ord, ArgWithVariant(command.a));
    case kCmdText: {
        const size_t mark = stack_.size;
        ParseTextArg(command.a & ~command.b, mark);
        const uint16_t text = NewUnary(MathNodeKind::Text, MathClass::Ord, LinkFrom(mark));
        return command.c ? NewUnary(MathNodeKind::Enclose, MathClass::Ord, text, kEncloseBox) : text;
    }
    case kCmdStack: {                                           // a: 0 \overset, 1 \underset, 2 \stackrel
        const uint16_t script = ParseArg();
        const uint16_t base = ParseArg();
        MathClass cls = MathClass::Rel;
        if (command.a != 2) cls = base && !N(base).next && N(base).cls != MathClass::None ? N(base).cls : MathClass::Ord;
        const uint16_t node = NewUnary(MathNodeKind::Stack, cls, base);
        N(node).child[command.a == 1 ? 2 : 1] = script;
        return node;
    }
    case kCmdXArrow: {
        uint16_t under = 0;
        ParseOptional(under);
        const uint16_t over = ParseArg();
        const uint16_t node = NewUnary(MathNodeKind::Stack, MathClass::Rel, 0, command.a);
        N(node).child[1] = over;
        N(node).child[2] = under;
        return node;
    }
    case kCmdEnclose:
        return NewUnary(MathNodeKind::Enclose, MathClass::Ord, ParseArg(), command.a);
    case kCmdOperatorName: {
        const bool star = TakeStar();
        const bool saved = inOpName_;
        inOpName_ = true;                                       // - and * are the Times hyphen and asterisk
        const uint16_t body = ArgWithVariant(kVariantRoman);
        inOpName_ = saved;
        return NewUnary(MathNodeKind::OpName, MathClass::Op, body, 0, star ? kOpDisplayLimits : 0);
    }
    case kCmdSubstack:
        return ParseSubstack();
    case kCmdNot:
        return ParseNot();
    case kCmdLeft:
        return ParseLeftRight();
    case kCmdPaired: {
        const uint16_t node = NewUnary(MathNodeKind::LeftRight, MathClass::Inner, ParseArg());
        N(node).a = command.a;
        N(node).b = command.b;
        return node;
    }
    case kCmdSized: {
        const int delimiter = ParseDelim();
        if (!delimiter) return 0;
        const uint16_t node = NewNode(MathNodeKind::SizedDelim, static_cast<MathClass>(command.b));
        N(node).aux = command.a;
        N(node).a = static_cast<uint16_t>(delimiter);
        return node;
    }
    case kCmdBegin:
        return ParseEnvironment();
    case kCmdStyle: {
        const uint16_t node = NewNode(MathNodeKind::Style);
        N(node).aux = command.a;
        return node;
    }
    case kCmdClass:
        return NewUnary(MathNodeKind::Group, static_cast<MathClass>(command.a), ParseArg(), 0,
                        command.a == static_cast<uint8_t>(MathClass::Op) ? kOpDisplayLimits : 0);
    case kCmdPhantom:
        return NewUnary(MathNodeKind::Phantom, MathClass::Ord, ParseArg(), command.a);
    case kCmdMod:
        return ParseMod(command.a);
    case kCmdHSpace:
        TakeStar();
        return SpaceFromDimen(RawBraced());
    case kCmdKern:
        return SpaceFromDimen(RawDimen());
    case kCmdDots: {                                            // \cdots before a Bin or Rel, else \ldots
        const int cls = PeekClass(Peek());
        return Command(cls == static_cast<int>(MathClass::Bin) || cls == static_cast<int>(MathClass::Rel)
                           ? kCommandCdots : kCommandLdots);
    }
    case kCmdColorArg:
        RawBraced();
        return NewUnary(MathNodeKind::Group, MathClass::Ord, ParseArg());
    case kCmdTransparent:
        return NewUnary(MathNodeKind::Group, MathClass::Ord, ParseArg());
    case kCmdRef: {                                             // the label as upright text
        const std::string_view label = RawBraced();
        const size_t mark = stack_.size;
        if (command.a) TextByte(mark, 0, '(');
        PlainText(mark, 0, label);
        if (command.a) TextByte(mark, 0, ')');
        return NewUnary(MathNodeKind::Text, MathClass::Ord, LinkFrom(mark));
    }
    case kCmdIgnore: {
        if (command.b) TakeStar();
        if (command.c) {                                        // [..]: parsed and dropped, a \tag in it too
            const Checkpoint checkpoint = Mark();
            const RowTag savedTag = rowTag_;
            uint16_t dropped = 0;
            ParseOptional(dropped);
            rowTag_ = savedTag;
            Rollback(checkpoint);
        }
        for (int i = 0; i < command.a; i++) RawBraced();
        return 0;
    }
    case kCmdFontDecl:      // a declaration where one primary is expected
    case kCmdTextDecl:      // a text declaration in math
    case kCmdRowSep:        // \cr where no row can end
        return 0;
    default:
        // \right \middle \end \limits \over \hline \tag outside a list, and the text-only words:
        // typeset like any unknown control word.
        return OpName(kMathCommandNames + command.name, MathClass::Op, kOpNameUnknown, 0);
    }
}

// Class the token would have as an atom, for \dots; -1 when it has none.
int Parser::PeekClass(const Token& token) const {
    if (token.kind == kTokWord) return token.command >= 0 ? CommandClass(token.command) : -1;
    if (token.kind != kTokChar) return -1;
    if (token.value < 0x80) {
        if (token.value < 0x20 || token.value > 0x7E) return -1;
        const uint8_t fontClass = kMathAscii[token.value - 0x20].fontClass;
        return fontClass ? fontClass >> 3 : -1;
    }
    const MathUnicodeEntry* entry = FindUnicode(token.value);
    if (!entry) return -1;
    if (entry->kind == kUniCommand) return CommandClass(entry->value);
    if (entry->kind == kUniGlyph) return entry->arg;
    return entry->kind == kUniMinus ? static_cast<int>(MathClass::Bin) : -1;
}

// \not= \not\in \not\subset are real glyphs; otherwise the argument's atom (or a Group around a
// longer argument) gets the negation slash.
uint16_t Parser::ParseNot() {
    const Token token = Peek();
    int glyph = -1;
    if (IsChar(token, '=')) glyph = kCommandNeq;
    else if (token.kind == kTokWord && token.command == kCommandIn) glyph = kCommandNotIn;
    else if (token.kind == kTokWord && token.command == kCommandSubset) glyph = kCommandNotSubset;
    if (glyph >= 0) {
        Take();
        return Command(glyph);
    }
    uint16_t arg = ParseArg();
    if (!arg) return 0;
    if (N(arg).next || N(arg).cls == MathClass::None) arg = NewUnary(MathNodeKind::Group, MathClass::Ord, arg);
    N(AtomOf(arg)).flags |= kAtomNegated;
    return arg;
}

// kind: 0 \bmod, 1 \pmod, 2 \mod, 3 \pod (the amsmath definitions).
uint16_t Parser::ParseMod(int kind) {
    if (kind == 0) return OpName("mod", MathClass::Bin, 0, 0);
    const uint16_t arg = ParseArg();
    const size_t mark = stack_.size;
    Push(NewSpace(kind == 2 ? 667 : 444, 1000));
    if (kind != 2) Push(NewGlyph(MathClass::Open, kFontRoman, '(', 0));
    if (kind != 3) {
        for (const char* c = "mod"; *c; c++) Push(NewGlyph(MathClass::Ord, kFontRoman, static_cast<unsigned char>(*c), 0));
        Push(NewSpace(333));
    }
    PushList(arg);
    if (kind != 2) Push(NewGlyph(MathClass::Close, kFontRoman, ')', 0));
    return NewUnary(MathNodeKind::Group, MathClass::Ord, LinkFrom(mark));
}

uint16_t Parser::SpaceFromDimen(std::string_view raw) {
    bool absolute = false;
    int value = 0;
    if (!ParseDimen(raw, absolute, value)) return 0;            // unparsable: no space
    const uint16_t node = NewSpace(value);
    if (absolute) N(node).flags = kSpaceAbsolute;
    return node;
}

// \left delim list { \middle delim list } \right delim. A \left whose \right is missing, or lies
// beyond a separator that closes the construct, ends there with a null right delimiter.
uint16_t Parser::ParseLeftRight() {
    const int left = ParseDelim();
    if (leftTop_ < kScopeCapacity) leftBraceBase_[leftTop_++] = static_cast<uint8_t>(BraceDepth());
    else Fail();
    const size_t mark = stack_.size;
    for (;;) {
        ParseItems(false);
        if (KindOf(Peek()) != kCmdMiddle || leftTop_ == leftFloor_ || leftBraceBase_[leftTop_ - 1] != BraceDepth()) break;
        Take();
        const int delimiter = ParseDelim();
        const uint16_t middle = NewNode(MathNodeKind::Middle);
        N(middle).a = static_cast<uint16_t>(delimiter);
        Push(middle);
    }
    const uint16_t body = LinkFrom(mark);
    if (leftTop_ > leftFloor_) leftTop_--;
    int right = kDelimNone;
    if (KindOf(Peek()) == kCmdRight) {
        Take();
        right = ParseDelim();
    }
    const uint16_t node = NewUnary(MathNodeKind::LeftRight, MathClass::Inner, body);
    N(node).a = static_cast<uint16_t>(left);
    N(node).b = static_cast<uint16_t>(right);
    return node;
}

// ------------------------------------------------------------------------------------------
// Rows and environments
// ------------------------------------------------------------------------------------------

uint16_t Parser::NewRow(uint16_t cells, const RowTag& tag, int rules) {
    const uint16_t row = NewUnary(MathNodeKind::Row, MathClass::None, cells, static_cast<uint8_t>(rules < 3 ? rules : 3),
                                  tag.star && tag.head ? kRowTagNoParens : 0);
    N(row).child[1] = tag.head;
    return row;
}

// cell { & cell } { rowsep ... } in a cell scope of its own. With `always` the result is an
// array even for a single cell; otherwise one row with one cell is returned as a plain list.
void Parser::ParseRows(uint8_t mode, bool closeOnBrace, bool always, Rows& result) {
    const Scope scope = EnterScope(closeOnBrace);
    const RowTag savedTag = rowTag_;
    const int savedRules = rowRules_;
    rowTag_ = RowTag();
    rowRules_ = 0;
    const size_t rowMark = stack_.size;   // finished Row nodes, then the Cell nodes of the open row
    size_t cellMark = rowMark;
    int rows = 0;
    int cells = 0;
    int maxCells = 0;
    int rulesBelow = 0;
    // The first row is not built while it may still be the whole formula ("a \\" at top level).
    bool pending = false;
    uint16_t pendingList = 0;
    RowTag pendingTag;
    int pendingRules = 0;
    while (!failed_) {
        const size_t listMark = stack_.size;
        ParseItems(mode != kRowsCentre && (cells & 1));
        const uint16_t list = LinkFrom(listMark);
        const Token token = Peek();
        const bool cellSep = IsChar(token, '&');
        const bool rowSep = !cellSep && IsRowSep(token);
        const bool firstCell = !always && rows == 0 && cells == 0;
        if (!cellSep && !rowSep) {
            if (rows > 0 && cells == 0 && !list && !rowTag_.claimed) {      // a trailing row separator
                rulesBelow = rowRules_;
                break;
            }
            if (firstCell) {
                pending = true;
                pendingList = list;
                pendingTag = rowTag_;
                break;
            }
        } else if (rowSep && firstCell) {
            Take();
            TakeStar();
            SkipRowSepDimen();
            pending = true;
            pendingList = list;
            pendingTag = rowTag_;
            pendingRules = rowRules_;
            rowTag_ = RowTag();
            rowRules_ = 0;
            rows = 1;
            continue;
        }
        if (pending) {
            pending = false;
            Push(NewRow(NewUnary(MathNodeKind::Cell, MathClass::None, pendingList), pendingTag, pendingRules));
            cellMark = stack_.size;
            maxCells = 1;
        }
        Push(NewUnary(MathNodeKind::Cell, MathClass::None, list));
        cells++;
        if (cellSep) {
            Take();
            if (cells >= kMathMaxColumns) Fail();
            continue;
        }
        if (rowSep) {
            Take();
            TakeStar();
            SkipRowSepDimen();
        }
        const uint16_t rowCells = LinkFrom(cellMark);
        Push(NewRow(rowCells, rowTag_, rowRules_));
        cellMark = stack_.size;
        if (cells > maxCells) maxCells = cells;
        cells = 0;
        rowTag_ = RowTag();
        rowRules_ = 0;
        if (++rows > kMathMaxRows) Fail();
        if (!rowSep) break;
    }
    result.array = !pending;
    if (pending) {
        stack_.size = rowMark;
        result.head = pendingList;
        result.tag = pendingTag;
    } else {
        result.head = LinkFrom(rowMark);
        result.maxCells = static_cast<uint8_t>(maxCells);
        result.rulesBelow = static_cast<uint8_t>(rulesBelow < 3 ? rulesBelow : 3);
    }
    rowTag_ = savedTag;
    rowRules_ = savedRules;
    LeaveScope(scope);
}

// \\[2pt]: the bracket is consumed only when it follows immediately and holds a dimension.
void Parser::SkipRowSepDimen() {
    if (pos_ >= end_ || s_[pos_] != '[') return;
    const int close = Find(']', pos_, 24);
    bool absolute = false;
    int value = 0;
    if (close > 0 && ParseDimen(std::string_view(s_ + pos_ + 1, static_cast<uint32_t>(close) - pos_ - 1), absolute, value)) {
        pos_ = static_cast<uint32_t>(close) + 1;
    }
}

uint16_t Parser::ParseSubstack() {
    if (!IsChar(Peek(), '{')) return 0;
    Take();
    Rows rows;
    ParseRows(kRowsCentre, true, true, rows);
    if (IsChar(Peek(), '}')) Take();
    return MakeArray(MathClass::Ord, kDelimNone, kDelimNone, 2, 0, rows, kEnvColsCentre, kColumnCentre, ColumnSpec());
}

// Column specification of array: l c r add a column, p{..} m{..} b{..} an l column, | a
// vertical rule; @{..} !{..} >{..} <{..} and the count of *{n}{..} are dropped.
void Parser::ParseColSpec(std::string_view spec, ColumnSpec& columns) {
    int parsed = 0;
    uint16_t rules = 0;
    for (size_t i = 0; i < spec.size(); i++) {
        const char c = spec[i];
        const bool braceNext = i + 1 < spec.size() && spec[i + 1] == '{';
        const bool paragraph = (c == 'p' || c == 'm' || c == 'b') && braceNext;
        if (c == 'l' || c == 'c' || c == 'r' || paragraph) {
            if (parsed == kMathMaxColumns) {                    // a 25th column: the same limit as a 25th cell
                Fail();
                return;
            }
            columns.align[parsed] = c == 'c' ? kColumnCentre : c == 'r' ? kColumnRight : kColumnLeft;
            columns.rules[parsed++] = rules;
            rules = 0;
        } else if (c == '|') {
            rules++;
        }
        if (braceNext && (paragraph || c == '@' || c == '!' || c == '>' || c == '<' || c == '*')) {
            int depth = 0;
            for (i++; i < spec.size(); i++) {
                if (spec[i] == '{') depth++;
                else if (spec[i] == '}') depth--;
                if (depth == 0) break;
            }
        }
    }
    columns.rules[parsed] = rules;
    columns.ruleCount = parsed + 1;
    if (!parsed) columns.align[0] = kColumnCentre;              // an empty specification is one c column
    columns.count = parsed ? parsed : 1;
}

// pattern: how columns without a specification are aligned (MathEnvColumns); align: the fixed
// alignment of kEnvColsCentre and kEnvColsLeft.
uint16_t Parser::MakeArray(MathClass cls, int left, int right, uint8_t aux, uint16_t gap, const Rows& rows,
                           uint8_t pattern, uint8_t align, const ColumnSpec& spec) {
    int count = rows.maxCells > spec.count ? rows.maxCells : spec.count;
    if (count < 1) count = 1;
    const size_t mark = stack_.size;
    for (int c = 0; c < count; c++) {
        uint8_t columnAlign = align;
        if (spec.count) columnAlign = spec.align[c < spec.count ? c : spec.count - 1];
        else if (pattern == kEnvColsPairs) columnAlign = c & 1 ? kColumnLeft : kColumnRight;
        else if (pattern == kEnvColsRcl) columnAlign = c == 0 ? kColumnRight : c == 1 ? kColumnCentre : kColumnLeft;
        const uint16_t column = NewNode(MathNodeKind::Col);
        N(column).aux = columnAlign;
        if (c < spec.ruleCount) N(column).a = spec.rules[c];
        if (c == count - 1 && spec.ruleCount > count) N(column).b = spec.rules[count];
        Push(column);
    }
    const uint16_t columns = LinkFrom(mark);
    const uint16_t node = NewUnary(MathNodeKind::Array, cls, rows.head, aux, rows.rulesBelow);
    N(node).child[1] = columns;
    N(node).a = static_cast<uint16_t>(left | right << 8);
    N(node).b = gap;
    return node;
}

uint16_t Parser::ParseEnvironment() {
    std::string_view name;
    uint32_t after = 0;
    if (!PeekEnvName(pos_, name, after)) return OpName("begin", MathClass::Op, kOpNameUnknown, 0);
    pos_ = after;
    // An unknown environment is a centred matrix; its name is not shown.
    MathEnvironment env = {0, 1000, kDelimNone, kDelimNone, kEnvColsCentre, 1, static_cast<uint8_t>(MathClass::Ord),
                           kEnvArgsNone};
    if (const MathEnvironment* known = FindEnvironment(name)) {
        env = *known;
    } else {
        // arguments glued to \begin{name} are dropped: [..], then one {..}
        if (pos_ < end_ && s_[pos_] == '[') {
            const int close = Find(']', pos_, 64);
            if (close > 0) pos_ = static_cast<uint32_t>(close) + 1;
        }
        if (pos_ < end_ && s_[pos_] == '{' && MatchingBrace(pos_) >= 0) pos_ = static_cast<uint32_t>(MatchingBrace(pos_)) + 1;
    }
    uint8_t align = env.columns == kEnvColsLeft ? kColumnLeft : kColumnCentre;
    ColumnSpec spec;
    if (env.args == kEnvArgsDropBrace) {
        RawBraced();
    } else if (env.args != kEnvArgsNone) {
        if (env.args != kEnvArgsSubarraySpec && IsChar(Peek(), '[')) {       // [r] of matrix*, [t] of array
            const int close = Find(']', pos_, 8);
            if (close > 0) {
                const std::string_view option = Trim(std::string_view(s_ + pos_ + 1, static_cast<uint32_t>(close) - pos_ - 1));
                if (env.args == kEnvArgsOptAlign && option.size() == 1) {
                    if (option[0] == 'l') align = kColumnLeft;
                    else if (option[0] == 'r') align = kColumnRight;
                }
                pos_ = static_cast<uint32_t>(close) + 1;
            }
        }
        if (env.args != kEnvArgsOptAlign) ParseColSpec(RawBraced(), spec);
    }
    if (envTop_ < kScopeCapacity) envNames_[envTop_++] = name;
    else Fail();
    Rows rows;
    ParseRows(env.columns == kEnvColsPairs ? kRowsPairs : kRowsCentre, false, true, rows);
    if (envTop_ > envFloor_) envTop_--;
    // \end of this environment is consumed; \end of an outer one closes this one implicitly.
    const Token token = Peek();
    std::string_view endName;
    if (KindOf(token) == kCmdEnd && PeekEnvName(token.end, endName, after) && endName == name) pos_ = after;
    return MakeArray(static_cast<MathClass>(env.cls), env.left, env.right, env.aux, env.colGap, rows, env.columns,
                     align, spec);
}

// ------------------------------------------------------------------------------------------
// Text mode
// ------------------------------------------------------------------------------------------

// Position of the '}' matching the '{' before `start`. When that brace is unbalanced the text
// ends at the first row separator, cell separator or \end, so that a forgotten '}' costs one
// cell and not the whole environment.
uint32_t Parser::FindTextEnd(uint32_t start) const {
    const int close = MatchingBrace(start - 1);
    if (close >= 0) return static_cast<uint32_t>(close);
    for (uint32_t i = start; i < end_; i++) {
        if (s_[i] == '&') return i;
        if (s_[i] != '\\') continue;
        if (i + 1 < end_ && s_[i + 1] == '\\') return i;
        if (end_ - i >= 4 && std::memcmp(s_ + i, "\\end", 4) == 0) return i;
        i++;
    }
    return end_;
}

// One character of a standard font (0x20-0x7E, or the ellipsis) in the text font `bits`:
// appended to the last item of the text list when that is a run of the same font.
void Parser::TextByte(size_t mark, uint8_t bits, uint32_t byte) {
    const uint8_t font = kTextFonts[bits & 3];
    std::string& text = out_.text;
    if (text.size() >= kMaxTextBytes) {
        Fail();
        return;
    }
    if (stack_.size > mark) {
        const uint16_t last = stack_.data[stack_.size - 1];
        if (N(last).kind == MathNodeKind::TextRun && N(last).aux == font) {
            if (static_cast<size_t>(N(last).a) + N(last).b != text.size()) {
                // Other text was stored behind the run (a \tag inside it): move the run to the end.
                const size_t offset = N(last).a;
                const size_t length = N(last).b;
                if (text.size() + length >= kMaxTextBytes) {
                    Fail();
                    return;
                }
                N(last).a = static_cast<uint16_t>(text.size());
                text.resize(text.size() + length);
                std::memmove(&text[N(last).a], &text[offset], length);
            }
            text.push_back(static_cast<char>(byte));
            N(last).b++;
            return;
        }
    }
    const uint16_t run = NewNode(MathNodeKind::TextRun);
    N(run).aux = font;
    N(run).a = static_cast<uint16_t>(text.size());
    N(run).b = 1;
    text.push_back(static_cast<char>(byte));
    Push(run);
}

// A run of bytes without white space; control bytes in it are dropped. A word with a byte >=
// 0x80 goes whole to the fallback font, or, without one, shows '?' for each such character.
void Parser::TextWord(size_t mark, uint8_t bits, const char* word, size_t size) {
    bool ascii = true;
    for (size_t i = 0; i < size && ascii; i++) ascii = static_cast<unsigned char>(word[i]) < 0x80;
    const bool fallback = !ascii && hasFallback_;
    const size_t offset = out_.text.size();
    if (fallback && offset + size * 3 > kMaxTextBytes) {
        Fail();
        return;
    }
    size_t length = 1;
    for (size_t i = 0; i < size; i += length) {
        length = 1;
        if (IsControl(static_cast<unsigned char>(word[i]))) continue;
        const uint32_t code = Decode(word, i, size, length);
        if (fallback) AppendUtf8(out_.text, code);
        else TextByte(mark, bits, code < 0x80 ? code : '?');
    }
    if (!fallback || out_.text.size() == offset) return;
    const uint16_t node = NewNode(MathNodeKind::Fallback);
    N(node).a = static_cast<uint16_t>(offset);
    N(node).b = static_cast<uint16_t>(out_.text.size() - offset);
    Push(node);
}

// Raw bytes as text: words separated by white space, one space per run; nothing is special.
void Parser::PlainText(size_t mark, uint8_t bits, std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        size_t stop = i;
        bool space = false;
        while (stop < text.size() && (IsSpace(static_cast<unsigned char>(text[stop])) || IsControl(static_cast<unsigned char>(text[stop])))) {
            space = space || IsSpace(static_cast<unsigned char>(text[stop]));
            stop++;
        }
        if (stop > i) {
            if (space) TextByte(mark, bits, ' ');
        } else {
            while (stop < text.size() && !IsSpace(static_cast<unsigned char>(text[stop]))) stop++;
            TextWord(mark, bits, text.data() + i, stop - i);
        }
        i = stop;
    }
}

// The argument of \text and friends, appended to the text list that starts at `mark`.
void Parser::ParseTextArg(uint8_t bits, size_t mark) {
    if (failed_) return;
    SkipWs();
    if (pos_ >= end_) return;
    if (s_[pos_] == '{') {
        const uint32_t start = pos_ + 1;
        const uint32_t close = FindTextEnd(start);
        const uint32_t savedEnd = end_;
        pos_ = start;
        end_ = close;
        Enter();
        ParseText(bits, mark);
        Leave();
        end_ = savedEnd;
        pos_ = close < savedEnd && s_[close] == '}' ? close + 1 : close;
        return;
    }
    const Token token = Peek();
    if (IsStop(token)) return;
    if (token.kind == kTokChar) {                               // a single character, as text
        Take();
        TextWord(mark, bits, s_ + token.start, token.end - token.start);
        return;
    }
    Enter();
    const uint16_t node = ParsePrimary();                       // or a single math primary
    Leave();
    if (node) Push(node);
}

void Parser::ParseText(uint8_t bits, size_t mark) {
    while (pos_ < end_ && !failed_) {
        const unsigned char c = static_cast<unsigned char>(s_[pos_]);
        if (IsSpace(c) || IsControl(c)) {                       // a run of white space is one space
            bool space = false;
            while (pos_ < end_ && (IsSpace(static_cast<unsigned char>(s_[pos_])) || IsControl(static_cast<unsigned char>(s_[pos_])))) {
                space = space || IsSpace(static_cast<unsigned char>(s_[pos_]));
                pos_++;
            }
            if (space) TextByte(mark, bits, ' ');
        } else if (c == '~') {
            pos_++;
            TextByte(mark, bits, ' ');
        } else if (c == '%' && percentComment_) {
            while (pos_ < end_ && s_[pos_] != '\n') pos_++;
        } else if (c == '{') {                                  // a group limits the effect of a declaration
            const uint32_t start = pos_ + 1;
            const uint32_t close = FindTextEnd(start);
            const uint32_t savedEnd = end_;
            pos_ = start;
            end_ = close < savedEnd ? close : savedEnd;
            Enter();
            ParseText(bits, mark);
            Leave();
            end_ = savedEnd;
            pos_ = close + 1 < savedEnd ? close + 1 : savedEnd;
        } else if (c == '}') {
            pos_++;
        } else if (c == '$') {                                  // math up to the next unescaped $
            const uint32_t start = pos_ + 1;
            uint32_t close = start;
            while (close < end_ && s_[close] != '$') close += s_[close] == '\\' ? 2 : 1;
            if (close > end_) close = end_;
            Push(MathInText(start, close));
            pos_ = close + 1 < end_ ? close + 1 : end_;
        } else if (c == '\\') {
            TextControl(bits, mark);
        } else {
            uint32_t stop = pos_;
            while (stop < end_) {
                const unsigned char w = static_cast<unsigned char>(s_[stop]);
                if (IsSpace(w) || w == '{' || w == '}' || w == '$' || w == '\\' || w == '~' || (w == '%' && percentComment_)) break;
                stop++;
            }
            TextWord(mark, bits, s_ + pos_, stop - pos_);
            pos_ = stop;
        }
    }
}

// $...$ or \(...\) inside text: a cell scope of its own in which & and \\ are dropped.
uint16_t Parser::MathInText(uint32_t start, uint32_t close) {
    const uint32_t savedPos = pos_;
    const uint32_t savedEnd = end_;
    const uint8_t savedVariant = variant_;
    const uint8_t savedEnvFloor = envFloor_;
    const Scope scope = EnterScope(false);
    pos_ = start;
    end_ = close;
    variant_ = kVariantNormal;
    envFloor_ = envTop_;
    const size_t mark = stack_.size;
    while (pos_ < end_ && !failed_) {
        ParseItems(false);
        if (Peek().kind == kTokEof) break;
        Take();
    }
    const uint16_t body = LinkFrom(mark);
    LeaveScope(scope);
    envFloor_ = savedEnvFloor;
    variant_ = savedVariant;
    pos_ = savedPos;
    end_ = savedEnd;
    return NewUnary(MathNodeKind::Group, MathClass::Ord, body, 0, kGroupInText);
}

// A backslash in text. A declaration changes `bits` for the rest of the enclosing group.
void Parser::TextControl(uint8_t& bits, size_t mark) {
    if (pos_ + 1 >= end_) {
        pos_++;
        return;
    }
    const unsigned char next = static_cast<unsigned char>(s_[pos_ + 1]);
    if (!IsLetter(next)) {
        if (IsControl(next)) {                                  // the backslash goes with a control byte
            pos_ += 2;
            return;
        }
        size_t length = 1;
        const uint32_t start = pos_ + 1;
        const uint32_t code = Decode(s_, start, end_, length);
        pos_ = start + static_cast<uint32_t>(length);
        switch (code) {
        case '%': case '$': case '&': case '#': case '_': case '{': case '}':
            TextByte(mark, bits, code);
            return;
        case ' ': case '\t': case '\n': case '\v': case '\f': case '\r': case '\\':
            TextByte(mark, bits, ' ');
            return;
        case ',':
            Push(NewSpace(167));
            return;
        case ':': case '>':
            Push(NewSpace(222));
            return;
        case ';':
            Push(NewSpace(278));
            return;
        case '!':
            Push(NewSpace(-167));
            return;
        case '(': {                                             // \( ... \)
            uint32_t close = pos_;
            while (close + 1 < end_ && (s_[close] != '\\' || s_[close + 1] != ')')) close++;
            if (close + 1 >= end_) close = end_;
            const uint16_t node = MathInText(pos_, close);
            pos_ = close + 2 < end_ ? close + 2 : end_;
            Push(node);
            return;
        }
        case '-': case '/': case '@':
            return;
        default:
            TextWord(mark, bits, s_ + start, length);
            return;
        }
    }
    uint32_t stop = pos_ + 1;
    while (stop < end_ && IsLetter(static_cast<unsigned char>(s_[stop]))) stop++;
    const std::string_view name(s_ + pos_ + 1, stop - pos_ - 1);
    uint32_t after = stop;
    while (after < end_ && (IsSpace(static_cast<unsigned char>(s_[after])) || IsControl(static_cast<unsigned char>(s_[after])))) after++;
    const int index = FindCommand(name);
    if (index >= 0) {
        const MathCommand& command = kMathCommands[index];
        int glyph = -1;
        switch (command.kind) {
        case kCmdText:
            pos_ = after;
            ParseTextArg((bits | command.a) & ~command.b, mark);
            return;
        case kCmdTextDecl:
            pos_ = after;
            bits = (bits | command.a) & ~command.b;
            return;
        case kCmdFontDecl:                                      // \cal and \mit set and clear nothing
            pos_ = after;
            bits = (bits | command.b) & ~command.c;
            return;
        case kCmdSpace:
            pos_ = after;
            Push(NewSpace(static_cast<int16_t>(command.a | command.b << 8)));
            return;
        case kCmdDots:
            glyph = kEllipsisCode;
            break;
        case kCmdGlyph:
            if ((command.b & 7) == kFontRoman && command.a == kEllipsisCode) glyph = kEllipsisCode;
            break;
        case kCmdTextOnly:
            if (command.a == 2) {
                pos_ = after;
                TextWord(mark, bits, name.data(), name.size());
                return;
            }
            glyph = command.a ? '\\' : kEllipsisCode;
            break;
        default:
            break;
        }
        if (glyph >= 0) {
            pos_ = after;
            TextByte(mark, bits, static_cast<uint32_t>(glyph));
            return;
        }
    }
    // Anything else is one math primary embedded in the text (symbols, \frac.., unknown names).
    Enter();
    const Scope scope = EnterScope(false);
    const uint16_t node = ParsePrimary();
    LeaveScope(scope);
    Leave();
    if (node) Push(node);
}

// ------------------------------------------------------------------------------------------
// Entry points
// ------------------------------------------------------------------------------------------

// Closes the gaps of the nodes that Orphan() took out of the tree, so that every node of the
// arena is referenced exactly once.
void Parser::Compact() {
    std::vector<MathNode>& nodes = out_.nodes;
    std::vector<uint16_t> moved(nodes.size());                  // new index of each node; 0 for a gap
    uint16_t count = 1;
    for (size_t i = 1; i < nodes.size(); i++) {
        if (nodes[i].kind != MathNodeKind::Null) moved[i] = count++;
    }
    for (size_t i = 1; i < nodes.size(); i++) {
        if (!moved[i]) continue;
        MathNode node = nodes[i];
        node.next = moved[node.next];
        for (uint16_t& child : node.child) child = moved[child];
        nodes[moved[i]] = node;
    }
    nodes.resize(count);
    out_.root = moved[out_.root];
    out_.tag = moved[out_.tag];
}

bool Parser::ParseFormula() {
    percentComment_ = size_ && std::memchr(s_, '\n', size_) != nullptr;
    MatchBraces();
    Rows rows;
    ParseRows(kRowsDetect, false, false, rows);
    RowTag tag = rows.tag;
    uint16_t root = rows.head;
    if (rows.array) {
        // Rows or columns outside an environment: an implicit aligned (r l r l ...) or gathered.
        const bool aligned = rows.maxCells > 1;
        tag = RowTag();
        root = MakeArray(MathClass::Ord, kDelimNone, kDelimNone,
                         static_cast<uint8_t>((display_ ? 0 : 1) | kArrayRowGap | (aligned ? kArrayAligned : 0)),
                         aligned ? 1000 : 0, rows, aligned ? kEnvColsPairs : kEnvColsCentre, kColumnCentre, ColumnSpec());
    }
    if (failed_) return false;
    if (!tag.head && root && !N(root).next && N(root).kind == MathNodeKind::Array) {
        // The row tag of a one-row array is the formula tag (\begin{equation} .. \tag{1} ..).
        const uint16_t row = N(root).child[0];
        if (row && !N(row).next && N(row).child[1]) {
            tag.head = N(row).child[1];
            tag.star = (N(row).flags & kRowTagNoParens) != 0;
            N(row).child[1] = 0;
            N(row).flags = 0;
        }
    }
    out_.root = root;
    out_.tag = tag.head;
    out_.tagParens = !(tag.star && tag.head);
    if (orphans_) Compact();
    return true;
}

void Parser::SourceFallback(std::string_view tex) {
    // The first kMathFallbackScanBytes bytes, cut back to a character boundary.
    size_t window = tex.size() < kMathFallbackScanBytes ? tex.size() : kMathFallbackScanBytes;
    if (window < tex.size()) {
        for (size_t back = 1; back <= 3 && back <= window; back++) {
            const unsigned char lead = static_cast<unsigned char>(tex[window - back]);
            if (lead < 0x80) break;
            if (lead >= 0xC0) {
                if ((lead < 0xE0 ? 2u : lead < 0xF0 ? 3u : 4u) > back) window -= back;
                break;
            }
        }
    }
    // White space collapsed, control bytes dropped, cut to kMathFallbackTextBytes whole characters.
    char shown[kMathFallbackTextBytes + 8];
    size_t size = 0;
    bool truncated = tex.size() > kMathFallbackScanBytes;
    bool space = false;
    size_t length = 1;
    for (size_t i = 0; i < window; i += length) {
        length = 1;
        const unsigned char c = static_cast<unsigned char>(tex[i]);
        if (IsSpace(c)) {
            space = true;
            continue;
        }
        if (IsControl(c)) continue;
        const uint32_t code = Decode(tex.data(), i, window, length);
        const size_t bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (space && size) {
            if (size == kMathFallbackTextBytes) {
                truncated = true;
                break;
            }
            shown[size++] = ' ';
        }
        space = false;
        if (size + bytes > kMathFallbackTextBytes) {
            truncated = true;
            break;
        }
        if (code < 0x80) {
            shown[size++] = static_cast<char>(code);
        } else if (code == 0xFFFD) {
            std::memcpy(shown + size, "\xEF\xBF\xBD", 3);
            size += 3;
        } else {
            std::memcpy(shown + size, tex.data() + i, bytes);   // well formed: copied as it is
            size += bytes;
        }
    }
    if (truncated) {
        std::memcpy(shown + size, " ...", 4);
        size += 4;
    }
    for (size_t i = 0; i < size;) {
        if (shown[i] == ' ') {
            TextByte(0, 0, ' ');
            i++;
            continue;
        }
        size_t stop = i;
        while (stop < size && shown[stop] != ' ') stop++;
        TextWord(0, 0, shown + i, stop - i);
        i = stop;
    }
    out_.root = NewUnary(MathNodeKind::Text, MathClass::Ord, LinkFrom(0));
    out_.sourceFallback = true;
}

void Reset(MathParseResult& out, size_t expectedNodes) {
    out.nodes.clear();
    out.nodes.reserve(expectedNodes + 1);
    out.nodes.emplace_back();
    out.text.clear();
    out.root = 0;
    out.tag = 0;
    out.tagParens = true;
    out.sourceFallback = false;
}

} // namespace

void ParseMath(std::string_view tex, bool display, bool hasFallbackFont, MathParseResult& out) {
    if (tex.size() <= kMathMaxSourceBytes) {
        Reset(out, tex.size() + 15);
        Parser parser(tex, display, hasFallbackFont, out);
        if (parser.ParseFormula()) return;
    }
    BuildMathSourceFallback(tex, hasFallbackFont, out);
}

void BuildMathSourceFallback(std::string_view tex, bool hasFallbackFont, MathParseResult& out) {
    Reset(out, 15);
    Parser parser(std::string_view(), false, hasFallbackFont, out);
    parser.SourceFallback(tex);
}

} // namespace TinyPdf::Internal
