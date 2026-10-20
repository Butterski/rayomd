// SPDX-License-Identifier: Apache-2.0

// The body of ParseMarkdownImpl (markdown_parser.cpp), one level of a document: included there as
// ParseMarkdownImpl itself with RAYOMD_SESSION_LEVEL 0, and in builds with RAYOMD_BUILD_SESSION a
// second time with 1 as ParseSessionDocument, the document level of a BuildSession's build, whose
// block loop goes on from the last build's (session_parse.h). Hooks in the one copy changed what GCC
// made of the other's (+0.4 to 0.8 % instructions on BuildPdf in the engine library); the copy
// every other parse runs is the same code as without sessions.

#if !RAYOMD_SESSION_LEVEL
    LevelLines level(depth);
    std::vector<std::string_view>& lines = level.lines;
    std::vector<LineInfo>& infos = level.infos;
    SplitLineViews(markdown, lines);
    infos.clear();
    infos.reserve(lines.size());
    bool hasMathOpeners = false;
    for (std::string_view line : lines) {
        LineInfo& info = infos.emplace_back();
        ClassifyLine(line, info);
        hasMathOpeners = hasMathOpeners || info.kind == LineKind::MathOpen;
    }
    if (hasMathOpeners) ResolveMathOpeners(lines, infos);

    ReferenceDefinitions definitions;
    // Lines that are a link definition (1) or a footnote definition (2), which leave the text.
    std::vector<unsigned char> suppressed;
    // Every link or footnote definition has "]:", and a document without it can refer to no note.
    const bool definitionSyntax = markdown.find("]:") != std::string::npos;
    if (notes != nullptr && depth == 0) notes->possible = definitionSyntax;
    // Text that may refer to a footnote waits in headings and tables until all are known.
    const bool notesPossible = notes != nullptr && notes->possible;
    const size_t firstNote = notes != nullptr ? notes->found.size() : 0;
    if (definitionSyntax) {
        suppressed.assign(lines.size(), 0);
        std::string_view activeFence;
        for (size_t index = 0; index < lines.size();) {
            if (infos[index].kind == LineKind::Fence) {
                if (activeFence.empty()) activeFence = infos[index].text;
                else if (IsMatchingClosingFence(infos[index], activeFence)) activeFence = {};
                index++;
                continue;
            }
            if (!activeFence.empty()) {
                index++;
                continue;
            }
            std::string_view ignoredIndented;
            if (RemoveIndentLevel(lines[index], ignoredIndented)) {
                index++;
                continue;
            }
            const std::string_view head = infos[index].trimmed;
            if (notes != nullptr && head.size() > 4 && head[0] == '[' && head[1] == '^') {
                const size_t noteLines = ReadFootnoteDefinition(lines, index, IsTextLine, &infos, *notes);
                if (noteLines != 0) {
                    for (size_t offset = 0; offset < noteLines; offset++) suppressed[index + offset] = 2;
                    index += noteLines;
                    continue;
                }
            }
            std::string label;
            ReferenceDefinition definition;
            size_t consumed = 0;
            if (ParseReferenceDefinition(lines, index, label, definition, consumed)) {
                definitions.emplace(std::move(label), std::move(definition));
                for (size_t offset = 0; offset < consumed; offset++) suppressed[index + offset] = 1;
                index += consumed;
                continue;
            }
            index++;
        }
    }
#else
    // The document's lines and classes in the record this parse leaves, those the edit left alone
    // shifted from the last build's (session_parse.cpp's ClassifyInSession).
    ReferenceDefinitions definitions;
    const bool definitionSyntax = DefinitionSyntaxInSession(markdown);
    notes->possible = definitionSyntax;
    const bool notesPossible = notes->possible;
    const size_t firstNote = notes->found.size();
    LineChange change;
    ParseLines& level = ClassifyInSession(markdown, definitionSyntax, definitions, *notes, change);
    std::vector<std::string_view>& lines = level.lines;
    std::vector<LineInfo>& infos = level.infos;
    std::vector<unsigned char>& suppressed = NextRecord(*tSessionDocument).suppressed;
#endif
    // The footnotes defined at this level, parsed once its link definitions are known.
    if (notes != nullptr && notes->found.size() > firstNote) DefineFootnotes(firstNote, definitions, depth, *notes);
    auto isSuppressed = [&](size_t index) {
        return !suppressed.empty() && suppressed[index] != 0;
    };

    std::vector<Block> blocks;
#if !RAYOMD_SESSION_LEVEL
    blocks.reserve(std::min(lines.size(), std::max<size_t>(8, lines.size() / 2)));
#endif
    size_t i = 0;
    // The HTML elements HTML blocks left open at this level, and where each HTML block's blocks
    // are with the alignment those elements give the blocks after it (Html::ApplyAlignments).
    Html::HtmlState htmlState;
    std::vector<Html::Alignment> alignments;

    if (!infos.empty() && infos[0].trimmed == "---") {
        size_t frontMatterEnd = 1;
        while (frontMatterEnd < infos.size() &&
            infos[frontMatterEnd].trimmed != "---" &&
            infos[frontMatterEnd].trimmed != "...") {
            frontMatterEnd++;
        }
        if (frontMatterEnd < infos.size()) i = frontMatterEnd + 1;
    }
#if RAYOMD_SESSION_LEVEL
    i = BeginInSession(lines, suppressed, definitions, definitionSyntax, i, change, notes);
    // A parse that does not go on from the last one's makes all of the document's blocks.
    if (!ParseReuses(*tSessionDocument)) blocks.reserve(std::min(lines.size(), std::max<size_t>(8, lines.size() / 2)));
#endif

    const bool stampLines = depth == 0;
    size_t stamped = 0;
    size_t passStart = i;
    while (i < lines.size()) {
        if (stampLines) {
            if (blocks.size() > stamped) StampSourceLines(blocks, stamped, passStart, i);
            passStart = i;
        }
#if RAYOMD_SESSION_LEVEL
        if (DocumentPass(*tSessionDocument, i, blocks.size(), htmlState.depth == 0)) break;
#endif
        if (isSuppressed(i)) {
            i++;
            continue;
        }
        const LineInfo& info = infos[i];
        std::string_view line = lines[i];
        std::string_view trimmed = info.trimmed;
        if (info.kind == LineKind::Empty) {
            i++;
            continue;
        }

        int setextLevel = 0;
        if (info.kind == LineKind::Plain && i + 1 < lines.size() && !isSuppressed(i + 1) &&
            ParseSetextUnderline(lines[i + 1], setextLevel)) {
            if (notesPossible && trimmed.find("[^") != std::string_view::npos) {
                AppendPendingHeading(blocks, setextLevel, trimmed, definitions, *notes);
                i += 2;
                continue;
            }
            bool headingHasMath = false;
            std::string heading = definitions.empty() || trimmed.find('[') == std::string_view::npos ?
                StripInlineMarkdownKeepMath(trimmed, headingHasMath) :
                StripInlineMarkdownKeepMath(ResolveReferenceLinks(trimmed, definitions), headingHasMath);
            Block& block = AppendBlock(blocks, BlockType::Heading);
            block.level = setextLevel;
            block.text = std::move(heading);
            block.hasMath = headingHasMath;
            i += 2;
            continue;
        }

        std::string_view indentedContent;
        if (RemoveIndentLevel(line, indentedContent)) {
            std::vector<std::string_view> codeLines;
            while (i < lines.size()) {
                std::string_view content;
                if (RemoveIndentLevel(lines[i], content)) {
                    codeLines.push_back(content);
                    i++;
                    continue;
                }
                if (infos[i].kind == LineKind::Empty) {
                    codeLines.push_back({});
                    i++;
                    continue;
                }
                break;
            }
            while (!codeLines.empty() && codeLines.back().empty()) codeLines.pop_back();
            std::string text;
            for (size_t codeIndex = 0; codeIndex < codeLines.size(); codeIndex++) {
                if (codeIndex) text.push_back('\n');
                text.append(codeLines[codeIndex]);
            }
            AppendBlock(blocks, BlockType::Code).text = std::move(text);
            continue;
        }

        if (!definitions.empty() && line.find("![") != std::string_view::npos) {
            std::string resolvedLine = ResolveReferenceLinks(line, definitions);
            ImageSyntax resolvedImage;
            if (ParseStandaloneImage(resolvedLine, &resolvedImage)) {
                blocks.push_back(ImageBlock(std::move(resolvedImage)));
                i++;
                continue;
            }
        }

        if (info.kind == LineKind::Fence) {
            std::string_view fence = info.text;
            const std::string_view infoString = trimmed.substr(fence.size());
            const bool mathFence = IsMathFenceInfo(infoString);
            const size_t firstContent = i + 1;
            i++;
            while (i < lines.size() && !IsMatchingClosingFence(infos[i], fence)) i++;
            const size_t contentEnd = i;
            const bool closed = i < lines.size();
            if (i < lines.size()) i++;
            if (mathFence && closed) {
                // ```math ... ``` is display math. An unterminated or empty math
                // fence stays the code block it has always been.
                std::string tex = JoinTrimmedLines(infos, firstContent, contentEnd);
                if (!tex.empty()) {
                    AppendBlock(blocks, BlockType::MathBlock).text = std::move(tex);
                    continue;
                }
            }
            Block& code = AppendBlock(blocks, BlockType::Code);
            FenceText(lines, firstContent, contentEnd, code.text);
            if (!infoString.empty()) code.codeLanguage = FenceLanguage(infoString);
            continue;
        }

        if (info.kind == LineKind::Math) {
            AppendBlock(blocks, BlockType::MathBlock).text = ToString(info.text);
            i++;
            continue;
        }

        if (info.kind == LineKind::MathOpen) {
            std::string_view lastContent;
            size_t close = FindMathBlockClose(infos, i, lastContent);
            if (close != std::string_view::npos) {
                std::string text = ToString(info.text);
                std::string body = JoinTrimmedLines(infos, i + 1, close);
                if (!body.empty()) {
                    if (!text.empty()) text += "\n";
                    text += body;
                }
                if (!lastContent.empty()) {
                    if (!text.empty()) text += "\n";
                    text.append(lastContent.data(), lastContent.size());
                }
                AppendBlock(blocks, BlockType::MathBlock).text = std::move(text);
                i = close + 1;
                continue;
            }
            // Only an opener that ResolveMathOpeners skipped can get here without a
            // closing line (it follows a fence marker that turned out to be indented
            // code). Keep the line as paragraph text.
            infos[i].kind = LineKind::Plain;
        }

        std::vector<int> aligns;
        if (IsTableStart(lines, i, &aligns)) {
            std::vector<std::vector<std::string>> rows;
            std::vector<TableMathCell> mathCells;
            rows.reserve(8);
            // A table that may refer to a footnote, one with a "[^" in a line it takes, keeps its
            // cells as Markdown, formulas included, until all footnotes are known; its formulas are
            // then found once its rows are read. (A look for "[^" up to the next block, past the
            // rows a spent ParseBudget leaves out, made tables in one run of lines quadratic.)
            const size_t start = i;
            bool notesPending = notesPossible && (LineMayReferToNote(lines[i]) || LineMayReferToNote(lines[i + 1]));
            // Reference links resolve cell by cell, so a link target cannot split a row.
            auto addRow = [&](std::string_view rowLine, std::vector<std::string> row) {
                if (!definitions.empty()) {
                    for (std::string& cell : row) {
                        if (cell.find('[') != std::string::npos) cell = ResolveReferenceLinks(cell, definitions);
                    }
                }
                if (!notesPossible) FindTableMath(rowLine, rows.size(), row, mathCells);
                else if (!notesPending) notesPending = LineMayReferToNote(rowLine);
                rows.push_back(std::move(row));
            };
            addRow(lines[i], SplitTableCells(lines[i]));
            const size_t columns = rows[0].size();
            i += 2;

            // As in GFM, the rows go on up to a blank line or the start of another block, and
            // every row has the cells of the header: missing ones are empty, extra ones dropped.
            // The empty cells come from the document's budget (ParseBudget); a row that would
            // pass it ends the table, and the lines from it on are text, as in cmark-gfm.
            ParseBudget* const budget = ParseBudget::Current();
            size_t ownCells = ParseBudget::kPaddedCells;
            size_t& cellsLeft = budget != nullptr ? budget->paddedCells : ownCells;
            while (i < lines.size() && infos[i].kind == LineKind::Plain && !isSuppressed(i)) {
                std::vector<std::string> row = SplitTableCells(lines[i], columns);
                if (row.size() < columns) {
                    if (columns - row.size() > cellsLeft) break;
                    cellsLeft -= columns - row.size();
                }
                row.resize(columns);
                addRow(lines[i], std::move(row));
                i++;
            }
            if (notesPossible && !notesPending) FindTableMathAfterRows(lines, start, rows, mathCells);

            Block table;
            table.type = BlockType::Table;
            table.rows = std::move(rows);
            table.aligns = std::move(aligns);
            if (notesPending) {
                table.notesPending = 1;
                notes->pending = true;
            } else if (!mathCells.empty()) {
                // A table with formulas shows its other cells as plain text. Literal control
                // bytes leave every cell first, so the only kMathText* bytes in this table
                // are the ones written below.
                constexpr unsigned char kSyntax = RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead;
                for (auto& row : table.rows) {
                    for (auto& cell : row) {
                        if (RayoMd::Text::ContainsByteClass(cell, kSyntax)) cell = StripInlineMarkdown(cell);
                        EraseMathTextBytes(cell);
                    }
                }
                for (TableMathCell& cell : mathCells) table.rows[cell.row][cell.column] = std::move(cell.text);
                table.hasMath = true;
            }
            blocks.push_back(std::move(table));
            continue;
        }

        if (info.kind == LineKind::Heading) {
            if (notesPossible && info.text.find("[^") != std::string_view::npos) {
                AppendPendingHeading(blocks, info.level, info.text, definitions, *notes);
                i++;
                continue;
            }
            bool headingHasMath = false;
            std::string heading = definitions.empty() || info.text.find('[') == std::string_view::npos ?
                StripInlineMarkdownKeepMath(info.text, headingHasMath) :
                StripInlineMarkdownKeepMath(ResolveReferenceLinks(info.text, definitions), headingHasMath);
            Block& block = AppendBlock(blocks, BlockType::Heading);
            block.level = info.level;
            block.text = std::move(heading);
            block.hasMath = headingHasMath;
            i++;
            continue;
        }

        if (info.kind == LineKind::Rule) {
            AppendBlock(blocks, BlockType::Rule);
            i++;
            continue;
        }

        if (info.kind == LineKind::PageBreak) {
            AppendBlock(blocks, BlockType::PageBreak);
            i++;
            continue;
        }

        if (info.kind == LineKind::HtmlComment) {
            // Not shown: everything up to the "-->" that closes it, or to the end of the
            // document when none does, as in CommonMark. Text after "-->" on that line is.
            size_t end = i;
            size_t close = std::string_view::npos;
            for (; end < lines.size(); end++) {
                const std::string_view text = lines[end];
                close = text.find("-->", end == i ? text.find("<!--") + 2 : 0);
                if (close != std::string_view::npos) break;
            }
            if (end < lines.size()) {
                const std::string_view rest = TrimView(lines[end].substr(close + 3));
                if (!rest.empty()) AppendBlock(blocks, BlockType::Paragraph).text = ToString(rest);
            }
            i = end + 1;
            continue;
        }

        if (info.kind == LineKind::HtmlBlock) {
            i = Html::MapBlockLines(lines, i, info.level, htmlState, blocks, alignments);
            continue;
        }

        if (info.kind == LineKind::Image) {
            ImageSyntax image;
            ParseStandaloneImage(line, &image);
            blocks.push_back(ImageBlock(std::move(image)));
            i++;
            continue;
        }

        if (info.kind == LineKind::Bullet || info.kind == LineKind::Numbered) {
            // Built in place: nothing else is appended to `blocks` while `item` is in use.
            Block& item = AppendBlock(blocks, info.kind == LineKind::Bullet ? BlockType::Bullet : BlockType::Numbered);
            item.level = info.level;
            item.number = info.number;
            int baseIndent = LeadingColumns(line);
            const int contentColumn = ListItemContentColumn(line, info.kind == LineKind::Bullet);
            // "[ ]", "[x]" or "[X]" and white space before the text make a task list item.
            std::string_view first = info.text;
            if (first.size() >= 4 && first[0] == '[' && first[2] == ']' && (first[3] == ' ' || first[3] == '\t') &&
                (first[1] == ' ' || first[1] == 'x' || first[1] == 'X')) {
                item.task = first[1] == ' ' ? 1 : 2;
                first = LTrimView(first.substr(4));
            }
            std::string itemMarkdown(first);
            bool sawBlank = false;
            i++;
            while (i < lines.size()) {
                if (isSuppressed(i)) {
                    i++;
                    continue;
                }
                if (infos[i].kind == LineKind::Empty) {
                    itemMarkdown += "\n\n";
                    sawBlank = true;
                    i++;
                    continue;
                }
                const int continuationIndent = LeadingColumns(lines[i]);
                // A list item that does not reach this item's content column is a sibling or
                // belongs to an outer list; one that does is nested in this item.
                if ((infos[i].kind == LineKind::Bullet || infos[i].kind == LineKind::Numbered) &&
                    continuationIndent < contentColumn) break;
                bool indented = continuationIndent > baseIndent;
                if (!indented && (sawBlank || IsBlockStart(infos[i]))) break;
                std::string_view continuation = indented ?
                    StripLeadingColumns(lines[i], contentColumn) : infos[i].trimmed;
                if (!itemMarkdown.empty() && itemMarkdown.back() != '\n') itemMarkdown.push_back('\n');
                itemMarkdown.append(continuation);
                sawBlank = false;
                i++;
            }
            if (!definitions.empty() && itemMarkdown.find('[') != std::string::npos) itemMarkdown = ResolveReferenceLinks(itemMarkdown, definitions);
            if (depth < 8) {
                // Blank lines after the item only add line feeds, which the nested parse skips.
                std::string_view single = itemMarkdown;
                while (!single.empty() && single.back() == '\n') single.remove_suffix(1);
                if (IsSingleParagraphLine(single)) {
                    itemMarkdown.resize(single.size());
                    item.text = std::move(itemMarkdown);
                } else {
                    item.children = ParseMarkdownImpl(itemMarkdown, depth + 1, notes);
                }
            } else {
                item.text = StripInlineMarkdown(itemMarkdown);
            }
            if (!item.children.empty() && item.children.front().type == BlockType::Paragraph) {
                item.text = std::move(item.children.front().text);
                item.children.erase(item.children.begin());
            }
            continue;
        }

        if (info.kind == LineKind::Quote) {
            std::string quoteMarkdown;
            bool allowLazyContinuation = true;
            while (i < lines.size()) {
                if (infos[i].kind == LineKind::Quote) {
                    if (!quoteMarkdown.empty()) quoteMarkdown.push_back('\n');
                    std::string_view quoted = StripQuoteMarker(lines[i]);
                    quoteMarkdown.append(quoted);
                    allowLazyContinuation = !TrimView(quoted).empty();
                    i++;
                    continue;
                }
                // A footnote definition starts a block of its own, which no lazy line can go into.
                if (allowLazyContinuation && infos[i].kind == LineKind::Plain && (suppressed.empty() || suppressed[i] != 2)) {
                    quoteMarkdown.push_back('\n');
                    quoteMarkdown.append(lines[i]);
                    i++;
                    continue;
                }
                break;
            }
            // A GitHub alert names itself before references resolve: its marker looks like one.
            std::string calloutTitle;
            const uint8_t alert = TakeQuoteMarker(quoteMarkdown, depth, calloutTitle);
            if (!definitions.empty() && quoteMarkdown.find('[') != std::string::npos) quoteMarkdown = ResolveReferenceLinks(quoteMarkdown, definitions);
            Block& quote = AppendBlock(blocks, BlockType::Quote);
            quote.alert = alert;
            if (depth < 8) {
                if (IsSingleParagraphLine(quoteMarkdown)) {
                    AppendBlock(quote.children, BlockType::Paragraph).text = std::move(quoteMarkdown);
                } else {
                    quote.children = ParseMarkdownImpl(quoteMarkdown, depth + 1, notes);
                }
            } else {
                quote.text = StripInlineMarkdown(quoteMarkdown);
            }
            if (quote.children.empty()) quote.text = StripInlineMarkdown(quoteMarkdown);
            if (!calloutTitle.empty()) quote.text = std::move(calloutTitle);
            continue;
        }

        std::string paragraph;
        bool previousHardBreak = false;
        // Line i was checked above and starts no table; only the lines after it can.
        const size_t paragraphStart = i;
        while (i < lines.size() && !isSuppressed(i) && !IsBlockStart(infos[i]) &&
            (i == paragraphStart || !IsTableStart(lines, i))) {
            if (!paragraph.empty()) paragraph += previousHardBreak ? "\n" : " ";
            std::string_view v = HasHardLineBreak(lines[i]) ? RTrimView(lines[i]) : infos[i].trimmed;
            paragraph.append(v.data(), v.size());
            previousHardBreak = HasHardLineBreak(lines[i]);
            i++;
        }
        if (paragraph.empty()) {
            paragraph = ToString(trimmed);
            i++;
        }
        if (!definitions.empty() && paragraph.find('[') != std::string::npos) paragraph = ResolveReferenceLinks(paragraph, definitions);
        // A paragraph of only images (a row of them) or only an <a id>: html.cpp's.
        if ((paragraph[0] == '<' || paragraph[0] == '!' || paragraph[0] == '[') && Html::MapParagraph(paragraph, blocks)) {
            continue;
        }
        AppendBlock(blocks, BlockType::Paragraph).text = std::move(paragraph);
    }
    if (stampLines && blocks.size() > stamped) StampSourceLines(blocks, stamped, passStart, i);
    if (!alignments.empty()) Html::ApplyAlignments(blocks, alignments);
#if RAYOMD_SESSION_LEVEL
    EndInSession(blocks, i, definitions, notes);
#endif

    return blocks;
