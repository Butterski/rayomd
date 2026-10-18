// SPDX-License-Identifier: Apache-2.0

// The dialect of the build on this thread, which every build reads (ParseMarkdown): built at -Os
// as the rest of the per-build code around it; the rewrite of Obsidian's syntax, which reads
// every byte of a note, is dialect_rewrite.cpp, at -O2.

#include "dialect.h"

namespace TinyPdf::Internal {

namespace {

thread_local MarkdownDialect g_dialect = MarkdownDialect::GitHub;
thread_local bool g_hideTags = false;

} // namespace

DialectScope::DialectScope(MarkdownDialect dialect, bool hideTags) : previous(g_dialect), previousHideTags(g_hideTags) {
    g_dialect = dialect;
    g_hideTags = hideTags;
}

DialectScope::~DialectScope() {
    g_dialect = previous;
    g_hideTags = previousHideTags;
}

bool ObsidianDialect() {
    return g_dialect == MarkdownDialect::Obsidian;
}

bool HiddenTags() {
    return g_hideTags;
}

} // namespace TinyPdf::Internal
