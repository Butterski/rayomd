#pragma once

// Syntax highlighting of fenced code blocks. The first word of a fence's info string names the
// language as GitHub reads it (Linguist's names and aliases); one forward pass of a small lexer
// for that language's family then gives each byte of the block a token class, which the
// renderers paint in the colours of GitHub's light theme. Linear in the length of the block for
// any input: no rule reads a byte more than a bounded number of times. Kept out of tiny_pdf.cpp,
// which sits at GCC's inlining limit.

#include <cstdint>
#include <string_view>

namespace TinyPdf::Internal {

enum CodeClass : uint8_t {
    kCodePlain,      // names, operators, punctuation, white space
    kCodeComment,
    kCodeString,
    kCodeNumber,
    kCodeKeyword,    // reserved words and built-in types
    kCodeConstant,   // true, null and the like, built-ins, JSON keys, CSS properties
    kCodeEntity,     // names of functions and types where they are defined or called
    kCodeTag,        // HTML and XML tag names, YAML keys
    kCodeVariable,   // $name and the like
    kCodeInserted,   // diff lines
    kCodeDeleted,
    kCodeRange,
    kCodeClassCount
};

// The language that the info string of a fenced code block names, or 0 for none that is
// highlighted: the block stays one colour.
uint8_t CodeLanguage(std::string_view info);

// Writes the class of each byte of `code` to `classes`, which has room for code.size().
void ClassifyCode(uint8_t language, std::string_view code, uint8_t* classes);

// The colour of each class: classes of one colour share an index, so that the renderers paint
// a stretch of them as one run. CodeColor gives the fill colour of an index as "r g b" operands.
constexpr uint8_t kCodeColorOfClass[kCodeClassCount] = { 0, 1, 2, 3, 4, 3, 5, 3, 6, 7, 8, 9 };
const char* CodeColor(uint8_t colorIndex);

// The tile behind a line whose first byte has class `codeClass`: a diff's added and removed lines
// are tinted; null for the others, which keep the tile of the code block.
const char* CodeLineTile(uint8_t codeClass);

} // namespace TinyPdf::Internal
