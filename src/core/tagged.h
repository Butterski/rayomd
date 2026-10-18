// SPDX-License-Identifier: Apache-2.0

#pragma once

// Tagged PDF for PDF/UA-1 (PdfOptions::pdfua): the structure tree the Unicode renderer builds as
// it paints, and the marked content that ties what it paints to the tree. Every text, path and
// image of a page is either in a marked-content sequence of an element ("/P <</MCID 3>> BDC ...
// EMC") or an artifact ("/Artifact BMC ... EMC"), and the two never nest: the renderer says
// before each paint whether it draws content or decoration, and the tree opens and closes the
// sequences, one for each run of content of one element on one page. Built at -Os: only tagged
// documents use it. Its elements hold no heap memory of their own, and the tree is written into
// one buffer, which the PDF's objects view.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// The standard structure types of PDF 1.7 (ISO 32000-1, 14.8.4) that Markdown needs.
enum class StructType : uint8_t {
    Document, P, H1, H2, H3, H4, H5, H6, L, LI, Lbl, LBody, BlockQuote, Div, Code, Table, THead,
    TBody, TR, TH, TD, Figure, Formula, Link, Reference, Note, TOC, TOCI, Caption,
};

// The kind of a list, for its /ListNumbering.
enum class ListKind : uint8_t { Bullet, Ordered, Task };

class StructTree {
public:
    // `content` is the renderer's output, whose tail is the page being painted.
    explicit StructTree(std::string& content);

    // Opens an element as the last child of the open one, and returns its index.
    int Begin(StructType type);
    // A heading of Markdown `level`: H1 to H6, one level below the previous heading at most, as
    // PDF/UA asks; the first heading is H1.
    int BeginHeading(int level);
    // A paragraph: a P, but the first one of a list item's body is that body's own content, as
    // ISO 32000-1 allows, which saves an element per item. -1 when no P is opened.
    int BeginParagraph();
    // Opens `element` again (a table cell drawn in slices on several pages), for End to close.
    void Resume(int element);
    // Closes `element` and the elements open in it, and their marked content.
    void End(int element);
    // An element as the last child of `parent` (0, the Document), opened over the elements open
    // now, which stay open: a footnote set at the foot of a page while a paragraph, a list item or
    // a table row goes on (PdfOptions::footnotes). The flow's list label, table and artifact state
    // wait until EndUnder closes it and what opened in it.
    int BeginUnder(int parent, StructType type);
    void EndUnder(int element);

    // /Alt of a figure, /ActualText, the Layout attributes of a figure's box on its page, a table
    // header cell's scope, a code block's placement, and a note's /ID, which the IDTree lists.
    void SetAlt(int element, std::string_view utf8);
    void SetActualText(int element, std::u16string_view text);
    void SetBox(int element, double left, double bottom, double right, double top);
    void SetColumnScope(int element);
    void SetBlockPlacement(int element);
    void MarkNote(int element);

    // Marked content. Content() comes before painting what belongs to the open element (`text`,
    // where it is text, gives a link its /Contents); Artifact() before painting decoration.
    // While artifacts are on (a table header repeated on a new page, an image without alt text),
    // content is painted as artifacts.
    void Content(std::wstring_view text = {});
    void Artifact();
    void SetArtifacts(bool on) { artifacts = on; }
    // The page ends: the open sequence closes in it, and MCIDs count from 0 on the next.
    void PageBreak();

    // Lists, from the blocks the renderer draws: EnterBlocks and LeaveBlocks around a sequence
    // of blocks, Block before each. A list item opens its L when its list starts, its LI, an Lbl
    // and an LBody, which stays open for the item's text and the items and blocks nested in it;
    // any other block closes the lists of its sequence.
    void EnterBlocks();
    void LeaveBlocks();
    // `depth` is the list nesting of the item's marker, for a bullet list's /ListNumbering.
    void Block(bool listItem, ListKind kind, int level, int depth);
    // The next `characters` of text painted are the open list item's label ("2. "), which the
    // renderer paints after LabelContent(); TakeLabel returns and clears the count. A bullet's
    // marker and a task's checkbox, drawn as paths, are the label's content, with the character
    // they stand for (the box's state) as `actualText`.
    void Label(size_t characters) { labelCharacters = characters; }
    size_t TakeLabel();
    void LabelContent(std::u16string_view actualText = {});

    // A link run of a paragraph: opens a Link for `url` unless the open one is for it; an empty
    // `url` closes an open Link.
    void LinkRun(std::string_view url);
    // A link rectangle the renderer added to the current page (its page's PageLinks), with the
    // Link it belongs to.
    void LinkRect();
    // A Link's text for its annotations, where it encloses no text (a linked image).
    void SetText(int element, std::string_view utf8);

    // Tables: a Table, under THead its header row and under TBody the others when it has both
    // (`rows` > 1); a TR per row with its TH (scope Column) or TD cells, made before the row is
    // drawn and opened again for each slice of the row a page holds. A header row repeated on a
    // later page is an artifact.
    void TableBegin(size_t rows);
    void TableRow(size_t row, size_t columns, bool repeated);
    void TableCell(size_t column, bool open);
    void TableRowEnd();
    void TableEnd();

    // Closes everything: after the last page is painted.
    void Finish();

    // What the builder needs once the pages exist.
    // The Link elements of each page's link rectangles, -1 for one outside a Link.
    const std::vector<std::vector<int>>& LinkRects() const { return linkRects; }
    // A link annotation of `element`, object `objectId`, in increasing object order; then
    // AnnotationEntries gives its " /StructParent ... /Contents ..." entries.
    void AddAnnotation(int element, int objectId);
    std::string AnnotationEntries(int objectId) const;

    // The structure tree's objects: the StructTreeRoot as object `rootId`, the elements, the
    // ParentTree and an IDTree as the objects after it, ObjectCount() of them in all, written by
    // Write; Object(i) is the body of object rootId + i, valid as long as the tree.
    size_t ObjectCount() const;
    void Write(int rootId, const std::vector<int>& pageIds);
    std::string_view Object(size_t index) const;

private:
    struct Kid {
        enum Kind : uint8_t { Element, Mcid, Annotation };
        Kind kind;
        int value;     // element index, MCID, or annotation object id
        int page;      // the page of an MCID
        int next;      // the element's next kid, or -1
    };
    struct Element {
        StructType type = StructType::Document;
        bool note = false;      // a footnote, with an /ID
        int parent = -1;
        int page = -1;          // the page of its first MCID
        int firstKid = -1;      // in `kids`
        int lastKid = -1;
        uint32_t attributes = 0;    // its dictionary entries, [attributes, attributesEnd) of `pool`
        uint32_t attributesEnd = 0;
        int link = -1;          // a Link's entry in `links`
    };
    struct Link {
        std::u16string text;    // for its annotations' /Contents
        std::string url;
    };
    struct OpenList {
        int scope;     // the EnterBlocks depth it opened in
        int level;
        ListKind kind;
        int list;      // the L
        int item;      // its open LI, or -1
    };

    void AddKid(int element, Kid::Kind kind, int value, int page);
    void AppendAttributes(int element, std::string_view entries);
    bool CanHoldContent(StructType type) const;
    StructType TypeOf(int element) const;
    void Open(int element);
    void Close();
    // Ends the line the content stream is on, so that an operator starts a line of its own.
    void NewLine() {
        if (!out.empty() && out.back() != '\n') out += '\n';
    }
    // Ends the lists of the current block sequence nested deeper than `level` (all for -1).
    void CloseLists(int level);
    void AppendLinkContents(std::string& entries, int element) const;

    std::string& out;
    std::vector<Element> elements;
    std::vector<Kid> kids;
    std::vector<Link> links;
    std::string pool;                              // the elements' attributes
    std::vector<int> stack;                        // the open elements, the Document first
    std::vector<std::vector<int>> parentTree;      // each page's elements by MCID
    std::vector<std::vector<int>> linkRects;       // each page's link rectangles' Links
    std::vector<int> annotations;                  // by /StructParent key minus the page count
    std::vector<int> annotationObjects;            // the same annotations' object ids, increasing
    std::vector<OpenList> lists;
    std::string written;                           // Write's objects, one after another
    std::vector<size_t> objectEnds;                // where each of them ends in `written`
    bool notes = false;         // an element has an /ID
    int scope = 0;
    int heading = 0;            // the level of the last heading tagged
    int open = -1;              // the element whose marked content is open
    bool artifactOpen = false;
    bool artifacts = false;
    size_t labelCharacters = 0;
    int label = -1;             // the open list item's Lbl, until its content is painted
    // The table being drawn: its Table, THead or TBody, the row's TR (-1 for a repeated header
    // row) and cells, and whether it has a THead.
    int table = -1;
    int tableSection = -1;
    int tableRow = -1;
    bool tableHead = false;
    std::vector<int> tableCells;
    // The flow's state while an element opened by BeginUnder is open (set there, read by EndUnder).
    struct Flow {
        size_t labelCharacters;
        int label;
        bool artifacts;
        int table;
        int tableSection;
        int tableRow;
        bool tableHead;
    } flow;
    std::vector<int> flowCells;
};

} // namespace TinyPdf::Internal
