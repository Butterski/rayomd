#include "tagged.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <utility>

#include "../common/text_utils.h"

namespace TinyPdf::Internal {
namespace {

// The /S names of StructType, in its order; arrays of characters, not pointers, so that the
// table needs no relocation.
constexpr char kTypeNames[][11] = {
    "Document", "P", "H1", "H2", "H3", "H4", "H5", "H6", "L", "LI", "Lbl", "LBody", "BlockQuote", "Div",
    "Code", "Table", "THead", "TBody", "TR", "TH", "TD", "Figure", "Formula", "Link", "Reference", "Note", "TOC",
    "TOCI", "Caption",
};
static_assert(sizeof(kTypeNames) / sizeof(kTypeNames[0]) == static_cast<size_t>(StructType::Caption) + 1);

// Their lengths, so that writing a name measures nothing.
constexpr std::array<uint8_t, sizeof(kTypeNames) / sizeof(kTypeNames[0])> MakeTypeNameLengths() {
    std::array<uint8_t, sizeof(kTypeNames) / sizeof(kTypeNames[0])> lengths{};
    for (size_t type = 0; type < lengths.size(); type++) {
        while (kTypeNames[type][lengths[type]] != '\0') lengths[type]++;
    }
    return lengths;
}
constexpr auto kTypeNameLengths = MakeTypeNameLengths();

// The UTF-16 code units of a link's text kept for its /Contents, and of an /Alt.
constexpr size_t kMaxTextUnits = 1000;

const char* TypeName(StructType type) {
    return kTypeNames[static_cast<size_t>(type)];
}

// The decimal digits of `value` at `at`; returns their end. A tenth by a multiply: at -Os GCC
// divides, and std::to_chars is a library loop several times as long.
char* WriteUnsigned(char* at, uint32_t value) {
    char digits[10];
    int count = 0;
    do {
        const uint32_t tenth = static_cast<uint32_t>((static_cast<uint64_t>(value) * 0xCCCCCCCDu) >> 35);
        digits[count++] = static_cast<char>('0' + (value - tenth * 10));
        value = tenth;
    } while (value != 0);
    while (count > 0) *at++ = digits[--count];
    return at;
}

void AppendUnsigned(std::string& out, size_t value) {
    char digits[10];
    out.append(digits, static_cast<size_t>(WriteUnsigned(digits, static_cast<uint32_t>(value)) - digits));
}

// Appends to a string through a pointer, room made ahead (Room) for what follows; Finish trims
// the string to what was written.
class Writer {
public:
    explicit Writer(std::string& text) : text_(text), at_(text.data()), end_(text.data() + text.size()) {}
    void Room(size_t bytes) {
        if (static_cast<size_t>(end_ - at_) < bytes) Grow(bytes);
    }
    template <size_t N>
    void Lit(const char (&literal)[N]) {
        memcpy(at_, literal, N - 1);
        at_ += N - 1;
    }
    void Bytes(const char* bytes, size_t length) {
        memcpy(at_, bytes, length);
        at_ += length;
    }
    void Char(char ch) { *at_++ = ch; }
    void Number(size_t value) { at_ = WriteUnsigned(at_, static_cast<uint32_t>(value)); }
    void Reference(int id) {
        Number(static_cast<size_t>(id));
        Lit(" 0 R");
    }
    // A footnote's /ID: "(note" and its element's index in ten digits, unique also in a book,
    // where each file numbers its notes from 1, and in the byte order of a name tree when in
    // index order.
    void NoteId(size_t element) {
        Lit("(note");
        for (int digit = 9; digit >= 0; digit--, element /= 10) at_[digit] = static_cast<char>('0' + element % 10);
        at_ += 10;
        Char(')');
    }
    size_t Size() const { return static_cast<size_t>(at_ - text_.data()); }
    void Finish() { text_.resize(Size()); }

private:
    void Grow(size_t bytes) {
        const size_t used = Size();
        text_.resize(std::max(text_.size() * 2, used + bytes + 4096));
        at_ = &text_[used];
        end_ = text_.data() + text_.size();
    }
    std::string& text_;
    char* at_;
    char* end_;
};

// `codePoint` as UTF-16, unless `text` is full; U+FEFF and U+FFFE, which a text string must not
// begin with or hold as text, and controls are left out.
void AppendUnit(std::u16string& text, uint32_t codePoint) {
    if (text.size() + 2 > kMaxTextUnits || codePoint < 0x20 || codePoint == 0xFEFF || codePoint == 0xFFFE) return;
    if (codePoint < 0x10000) {
        text += static_cast<char16_t>(codePoint);
    } else {
        codePoint -= 0x10000;
        text += static_cast<char16_t>(0xD800 + (codePoint >> 10));
        text += static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF));
    }
}

// A PDF text string: printable ASCII, which PDFDocEncoding shares, as a literal string, one byte
// a character; else UTF-16BE with its byte order mark, as hex.
void AppendTextString(std::string& out, std::u16string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    const bool ascii = std::all_of(text.begin(), text.end(), [](char16_t unit) { return unit >= 0x20 && unit < 0x7F; });
    if (ascii) {
        out += '(';
        for (const char16_t unit : text) {
            if (unit == '(' || unit == ')' || unit == '\\') out += '\\';
            out += static_cast<char>(unit);
        }
        out += ')';
        return;
    }
    const size_t start = out.size();
    out.resize(start + 6 + text.size() * 4);
    char* at = &out[start];
    memcpy(at, "<FEFF", 5);
    at += 5;
    for (const char16_t unit : text) {
        for (int shift = 12; shift >= 0; shift -= 4) *at++ = kHex[(unit >> shift) & 0xF];
    }
    *at = '>';
}

std::u16string Utf16(std::string_view utf8) {
    std::u16string text;
    for (size_t at = 0; at < utf8.size();) {
        uint32_t codePoint = static_cast<unsigned char>(utf8[at]);
        size_t length = 1;
        if (codePoint >= 0x80 && !RayoMd::Text::DecodeUtf8(utf8, at, codePoint, length)) codePoint = 0xFFFD;
        AppendUnit(text, codePoint);
        at += length;
    }
    return text;
}


} // namespace

StructTree::StructTree(std::string& content) : out(content), parentTree(1), linkRects(1) {
    elements.emplace_back();   // the Document, element 0
    stack.push_back(0);
}

void StructTree::AddKid(int element, Kid::Kind kind, int value, int page) {
    const int index = static_cast<int>(kids.size());
    kids.push_back({ kind, value, page, -1 });
    Element& parent = elements[static_cast<size_t>(element)];
    if (parent.lastKid >= 0) kids[static_cast<size_t>(parent.lastKid)].next = index;
    else parent.firstKid = index;
    parent.lastKid = index;
}

int StructTree::Begin(StructType type) {
    const int parent = stack.back();
    const int index = static_cast<int>(elements.size());
    Element& element = elements.emplace_back();
    element.type = type;
    element.parent = parent;
    AddKid(parent, Kid::Element, index, 0);
    stack.push_back(index);
    return index;
}

int StructTree::BeginHeading(int level) {
    heading = std::clamp(std::min(level, heading + 1), 1, 6);
    return Begin(static_cast<StructType>(static_cast<int>(StructType::H1) + heading - 1));
}

int StructTree::BeginParagraph() {
    const Element& top = elements[static_cast<size_t>(stack.back())];
    if (top.type == StructType::LBody && top.firstKid < 0) return -1;
    return Begin(StructType::P);
}

void StructTree::Resume(int element) {
    if (element > 0) stack.push_back(element);
}

void StructTree::End(int element) {
    if (element <= 0 || std::find(stack.begin(), stack.end(), element) == stack.end()) return;
    while (true) {
        const int top = stack.back();
        if (open == top) Close();
        stack.pop_back();
        if (elements[static_cast<size_t>(top)].type == StructType::L) {
            lists.erase(std::remove_if(lists.begin(), lists.end(), [top](const OpenList& entry) { return entry.list == top; }),
                lists.end());
        }
        for (OpenList& entry : lists) {
            if (entry.item == top) entry.item = -1;
        }
        if (top == label) label = -1;
        if (top == element) return;
    }
}

StructType StructTree::TypeOf(int element) const {
    return elements[static_cast<size_t>(element)].type;
}

// Adds dictionary entries to `element`'s, which stay one range of `pool`: moved to its end when
// another element's entries came after them.
void StructTree::AppendAttributes(int element, std::string_view entries) {
    Element& target = elements[static_cast<size_t>(element)];
    if (target.attributes == target.attributesEnd) {
        target.attributes = static_cast<uint32_t>(pool.size());
    } else if (target.attributesEnd != pool.size()) {
        const std::string earlier = pool.substr(target.attributes, target.attributesEnd - target.attributes);
        target.attributes = static_cast<uint32_t>(pool.size());
        pool += earlier;
    }
    pool.append(entries.data(), entries.size());
    target.attributesEnd = static_cast<uint32_t>(pool.size());
}

void StructTree::SetAlt(int element, std::string_view utf8) {
    std::string entries = " /Alt ";
    AppendTextString(entries, Utf16(utf8));
    AppendAttributes(element, entries);
}

void StructTree::SetActualText(int element, std::u16string_view text) {
    std::string entries = " /ActualText ";
    AppendTextString(entries, text);
    AppendAttributes(element, entries);
}

void StructTree::SetBox(int element, double left, double bottom, double right, double top) {
    std::string entries = " /A << /O /Layout /BBox [";
    for (const double value : { left, bottom, right, top }) {
        RayoMd::Text::AppendFixed2(entries, value);
        entries += ' ';
    }
    entries.back() = ']';
    entries += " >>";
    AppendAttributes(element, entries);
}

void StructTree::SetColumnScope(int element) {
    AppendAttributes(element, " /A << /O /Table /Scope /Column >>");
}

void StructTree::SetBlockPlacement(int element) {
    AppendAttributes(element, " /A << /O /Layout /Placement /Block >>");
}

void StructTree::MarkNote(int element) {
    elements[static_cast<size_t>(element)].note = true;
    notes = true;
}

bool StructTree::CanHoldContent(StructType type) const {
    switch (type) {
    case StructType::Document:
    case StructType::L:
    case StructType::LI:
    case StructType::Div:
    case StructType::Table:
    case StructType::THead:
    case StructType::TBody:
    case StructType::TR:
    case StructType::Note:
    case StructType::TOC:
        return false;
    default:
        return true;
    }
}

void StructTree::Open(int element) {
    if (open == element) return;
    Close();
    const StructType type = elements[static_cast<size_t>(element)].type;
    // Content where only elements may go is a renderer that did not open its element.
    assert(CanHoldContent(type));
    if (!CanHoldContent(type)) {
        Artifact();
        return;
    }
    const int page = static_cast<int>(parentTree.size()) - 1;
    std::vector<int>& mcids = parentTree.back();
    const int mcid = static_cast<int>(mcids.size());
    mcids.push_back(element);
    Element& target = elements[static_cast<size_t>(element)];
    if (target.page < 0) target.page = page;
    AddKid(element, Kid::Mcid, mcid, page);
    // "/P <</MCID 3>> BDC" in one append.
    char text[48];
    char* at = text;
    if (!out.empty() && out.back() != '\n') *at++ = '\n';
    *at++ = '/';
    const size_t length = kTypeNameLengths[static_cast<size_t>(type)];
    memcpy(at, TypeName(type), length);
    at += length;
    memcpy(at, " <</MCID ", 9);
    at = WriteUnsigned(at + 9, static_cast<uint32_t>(mcid));
    memcpy(at, ">> BDC\n", 7);
    out.append(text, static_cast<size_t>(at + 7 - text));
    open = element;
}

void StructTree::Close() {
    if (open >= 0 || artifactOpen) {
        NewLine();
        out += "EMC\n";
    }
    open = -1;
    artifactOpen = false;
}

void StructTree::Content(std::wstring_view text) {
    if (artifacts) {
        Artifact();
        return;
    }
    const int element = stack.back();
    Open(element);
    const Element& target = elements[static_cast<size_t>(element)];
    if (target.link < 0) return;
    std::u16string& linkText = links[static_cast<size_t>(target.link)].text;
    for (const wchar_t ch : text) {
        // wchar_t is UTF-16 on Windows: a surrogate is kept as it is.
        AppendUnit(linkText, static_cast<uint32_t>(ch));
    }
}

void StructTree::Artifact() {
    if (artifactOpen) return;
    Close();
    NewLine();
    out += "/Artifact BMC\n";
    artifactOpen = true;
}

void StructTree::PageBreak() {
    Close();
    parentTree.emplace_back();
    linkRects.emplace_back();
}

void StructTree::EnterBlocks() {
    scope++;
}

void StructTree::LeaveBlocks() {
    CloseLists(-1);
    scope--;
}

void StructTree::CloseLists(int level) {
    while (!lists.empty() && lists.back().scope == scope && lists.back().level > level) End(lists.back().list);
}

void StructTree::Block(bool listItem, ListKind kind, int level) {
    if (!listItem) {
        CloseLists(-1);
        return;
    }
    CloseLists(level);
    if (!lists.empty() && lists.back().scope == scope && lists.back().level == level) {
        // A numbered item after bullets begins a list of its own; task items join bullets, as on
        // GitHub.
        if ((lists.back().kind == ListKind::Ordered) != (kind == ListKind::Ordered)) {
            End(lists.back().list);
        } else if (lists.back().item >= 0) {
            End(lists.back().item);
        }
    }
    if (lists.empty() || lists.back().scope != scope || lists.back().level < level) {
        const int list = Begin(StructType::L);
        AppendAttributes(list, kind == ListKind::Ordered ? " /A << /O /List /ListNumbering /Decimal >>"
            : kind == ListKind::Bullet ? " /A << /O /List /ListNumbering /Disc >>" : " /A << /O /List /ListNumbering /None >>");
        lists.push_back({ scope, level, kind, list, -1 });
    }
    lists.back().item = Begin(StructType::LI);
    const int lbl = Begin(StructType::Lbl);
    End(lbl);
    label = lbl;
    labelCharacters = 0;
    Begin(StructType::LBody);
}

size_t StructTree::TakeLabel() {
    return label < 0 ? 0 : std::exchange(labelCharacters, 0);
}

void StructTree::LabelContent(std::u16string_view actualText) {
    if (label < 0) {
        Content();
        return;
    }
    if (!actualText.empty()) SetActualText(label, actualText);
    Open(label);
    label = -1;
}

void StructTree::LinkRun(std::string_view url) {
    if (artifacts) return;   // a repeated table header: its links are left out
    const int top = stack.back();
    const Element& current = elements[static_cast<size_t>(top)];
    if (current.type == StructType::Link) {
        if (!url.empty() && links[static_cast<size_t>(current.link)].url == url) return;
        End(top);
    }
    if (url.empty()) return;
    const int link = Begin(StructType::Link);
    elements[static_cast<size_t>(link)].link = static_cast<int>(links.size());
    links.push_back({ {}, std::string(url) });
}

void StructTree::LinkRect() {
    const int top = stack.back();
    linkRects.back().push_back(elements[static_cast<size_t>(top)].type == StructType::Link ? top : -1);
}

void StructTree::SetText(int element, std::string_view utf8) {
    Element& target = elements[static_cast<size_t>(element)];
    if (target.link < 0) {
        target.link = static_cast<int>(links.size());
        links.emplace_back();
    }
    links[static_cast<size_t>(target.link)].text = Utf16(utf8);
}

void StructTree::TableBegin(size_t rows) {
    table = Begin(StructType::Table);
    tableSection = -1;
    tableHead = rows > 1;
}

void StructTree::TableRow(size_t row, size_t columns, bool repeated) {
    if (repeated) {
        tableRow = -1;
        artifacts = true;
        return;
    }
    if (tableHead && row == 0) {
        tableSection = Begin(StructType::THead);
    } else if (tableHead && tableSection >= 0 && TypeOf(tableSection) == StructType::THead) {
        End(tableSection);
        tableSection = Begin(StructType::TBody);
    }
    tableRow = Begin(StructType::TR);
    tableCells.resize(columns);
    for (size_t column = 0; column < columns; column++) {
        const int cell = Begin(row == 0 ? StructType::TH : StructType::TD);
        if (row == 0) SetColumnScope(cell);
        End(cell);
        tableCells[column] = cell;
    }
}

void StructTree::TableCell(size_t column, bool openCell) {
    if (tableRow < 0 || column >= tableCells.size()) return;
    if (openCell) Resume(tableCells[column]);
    else End(tableCells[column]);
}

void StructTree::TableRowEnd() {
    if (tableRow < 0) artifacts = false;
    else End(tableRow);
}

void StructTree::TableEnd() {
    End(table);
}

void StructTree::Finish() {
    Close();
    lists.clear();
    stack.resize(1);
}

void StructTree::AddAnnotation(int element, int objectId) {
    annotations.push_back(element);
    annotationObjects.push_back(objectId);
    AddKid(element, Kid::Annotation, objectId, 0);
}

void StructTree::AppendLinkContents(std::string& entries, int element) const {
    const int link = elements[static_cast<size_t>(element)].link;
    const std::u16string_view text = link >= 0 ? std::u16string_view(links[static_cast<size_t>(link)].text) : u"";
    AppendTextString(entries, text.empty() ? std::u16string_view(u"Link") : text);
}

std::string StructTree::AnnotationEntries(int objectId) const {
    const auto found = std::lower_bound(annotationObjects.begin(), annotationObjects.end(), objectId);
    if (found == annotationObjects.end() || *found != objectId) return {};
    const size_t index = static_cast<size_t>(found - annotationObjects.begin());
    std::string entries = " /StructParent ";
    AppendUnsigned(entries, parentTree.size() + index);
    entries += " /Contents ";
    AppendLinkContents(entries, annotations[index]);
    return entries;
}

size_t StructTree::ObjectCount() const {
    return elements.size() + 2 + (notes ? 1 : 0);
}

std::string_view StructTree::Object(size_t index) const {
    const size_t start = index == 0 ? 0 : objectEnds[index - 1];
    return std::string_view(written).substr(start, objectEnds[index] - start);
}

void StructTree::Write(int rootId, const std::vector<int>& pageIds) {
    const auto id = [rootId](int element) { return rootId + 1 + element; };
    const int parentTreeId = rootId + 1 + static_cast<int>(elements.size());
    written.clear();
    objectEnds.clear();
    objectEnds.reserve(ObjectCount());
    Writer w(written);

    w.Room(160);
    w.Lit("<< /Type /StructTreeRoot /K ");
    w.Reference(id(0));
    w.Lit(" /ParentTree ");
    w.Reference(parentTreeId);
    w.Lit(" /ParentTreeNextKey ");
    w.Number(parentTree.size() + annotations.size());
    if (notes) {
        w.Lit(" /IDTree ");
        w.Reference(parentTreeId + 1);
    }
    w.Lit(" >>");
    objectEnds.push_back(w.Size());

    for (size_t index = 0; index < elements.size(); index++) {
        // /Type /StructElem is optional (ISO 32000-1, Table 323); left out, as a tagged document
        // has an element for every paragraph and table cell.
        const Element& element = elements[index];
        w.Room(96 + (element.attributesEnd - element.attributes));
        w.Lit("<< /S /");
        const char* name = TypeName(element.type);
        w.Bytes(name, kTypeNameLengths[static_cast<size_t>(element.type)]);
        w.Lit(" /P ");
        w.Reference(element.parent < 0 ? rootId : id(element.parent));
        if (element.page >= 0) {
            w.Lit(" /Pg ");
            w.Reference(pageIds[static_cast<size_t>(element.page)]);
        }
        if (element.note) {
            w.Lit(" /ID ");
            w.NoteId(index);
        }
        if (element.firstKid >= 0) {
            const bool many = element.firstKid != element.lastKid;
            if (many) w.Lit(" /K [");
            else w.Lit(" /K ");
            for (int at = element.firstKid; at >= 0;) {
                const Kid& kid = kids[static_cast<size_t>(at)];
                w.Room(56);
                if (kid.kind == Kid::Element) {
                    w.Reference(id(kid.value));
                } else if (kid.kind == Kid::Annotation) {
                    w.Lit("<< /Type /OBJR /Obj ");
                    w.Reference(kid.value);
                    w.Lit(" >>");
                } else if (kid.page == element.page) {
                    w.Number(static_cast<size_t>(kid.value));
                } else {
                    w.Lit("<< /Type /MCR /Pg ");
                    w.Reference(pageIds[static_cast<size_t>(kid.page)]);
                    w.Lit(" /MCID ");
                    w.Number(static_cast<size_t>(kid.value));
                    w.Lit(" >>");
                }
                at = kid.next;
                if (at >= 0) w.Char(' ');
            }
            if (many) w.Char(']');
        }
        w.Room(4 + (element.attributesEnd - element.attributes));
        w.Bytes(pool.data() + element.attributes, element.attributesEnd - element.attributes);
        w.Lit(" >>");
        objectEnds.push_back(w.Size());
    }

    // Each page's elements by MCID, then each annotation's Link by its key.
    w.Room(16);
    w.Lit("<< /Nums [");
    for (size_t page = 0; page < parentTree.size(); page++) {
        const std::vector<int>& elementsOfPage = parentTree[page];
        w.Room(24 + elementsOfPage.size() * 16);
        if (page != 0) w.Char(' ');
        w.Number(page);
        w.Lit(" [");
        for (size_t mcid = 0; mcid < elementsOfPage.size(); mcid++) {
            if (mcid != 0) w.Char(' ');
            w.Reference(id(elementsOfPage[mcid]));
        }
        w.Char(']');
    }
    for (size_t index = 0; index < annotations.size(); index++) {
        w.Room(32);
        w.Char(' ');
        w.Number(parentTree.size() + index);
        w.Char(' ');
        w.Reference(id(annotations[index]));
    }
    w.Room(8);
    w.Lit("] >>");
    objectEnds.push_back(w.Size());

    if (notes) {
        w.Room(16);
        w.Lit("<< /Names [");
        bool first = true;
        for (size_t index = 0; index < elements.size(); index++) {
            if (!elements[index].note) continue;
            w.Room(48);
            if (!first) w.Char(' ');
            first = false;
            w.NoteId(index);
            w.Char(' ');
            w.Reference(id(static_cast<int>(index)));
        }
        w.Room(8);
        w.Lit("] >>");
        objectEnds.push_back(w.Size());
    }
    w.Finish();
}

} // namespace TinyPdf::Internal
