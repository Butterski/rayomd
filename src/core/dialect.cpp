// SPDX-License-Identifier: Apache-2.0

// The dialect of the build on this thread, which every build reads (ParseMarkdown): built at -Os
// as the rest of the per-build code around it; the rewrite of Obsidian's syntax, which reads
// every byte of a note, is dialect_rewrite.cpp, at -O2.

#include "dialect.h"

namespace TinyPdf::Internal {

namespace {

thread_local MarkdownDialect g_dialect = MarkdownDialect::GitHub;

} // namespace

DialectScope::DialectScope(MarkdownDialect dialect) : previous(g_dialect) {
    g_dialect = dialect;
}

DialectScope::~DialectScope() {
    g_dialect = previous;
}

bool ObsidianDialect() {
    return g_dialect == MarkdownDialect::Obsidian;
}

} // namespace TinyPdf::Internal
