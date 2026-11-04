// SPDX-License-Identifier: Apache-2.0

#include "serve_request.h"

#include "../common/text_utils.h"

#include <cstdint>
#include <cstring>

namespace RayoMd::Serve {

namespace {

enum Field : unsigned { kId, kInput, kMarkdown, kSourcePath, kOutput, kArgs, kFieldCount };

constexpr const char* kFieldNames[kFieldCount] = { "id", "input", "markdown", "source_path", "output", "args" };

void AppendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

// A forward cursor over the line: every byte is read once, and no value nests.
struct Reader {
    std::string_view text;
    size_t at = 0;
    std::string& error;

    bool Fail(std::string what) {
        error = std::move(what) + " at byte " + std::to_string(at);
        return false;
    }
    void SkipSpace() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) at++;
    }
    // Skips white space, then takes `c` when it comes next.
    bool Take(char c) {
        SkipSpace();
        if (at < text.size() && text[at] == c) {
            at++;
            return true;
        }
        return false;
    }
    bool Next(char c) const { return at < text.size() && text[at] == c; }

    bool Hex4(uint32_t& value) {
        if (text.size() - at < 4) return false;
        value = 0;
        for (int i = 0; i < 4; i++) {
            const char c = text[at++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    // The string after its opening quote into `out`: escapes decoded, surrogate pairs joined,
    // raw bytes kept only when they are UTF-8.
    bool String(std::string& out) {
        out.clear();
        for (;;) {
            const size_t run = at;
            while (at < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[at]);
                if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
                at++;
            }
            out.append(text.data() + run, at - run);
            if (at == text.size()) return Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text[at]);
            if (c == '"') {
                at++;
                return true;
            }
            if (c < 0x20) return Fail("control character in a string");
            if (c >= 0x80) {
                uint32_t codePoint = 0;
                size_t length = 0;
                if (!RayoMd::Text::DecodeUtf8(text, at, codePoint, length)) return Fail("invalid UTF-8");
                out.append(text.data() + at, length);
                at += length;
                continue;
            }
            if (++at == text.size()) return Fail("unterminated string");
            switch (text[at++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t codePoint = 0;
                if (!Hex4(codePoint)) return Fail("bad \\u escape");
                if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return Fail("unpaired surrogate");
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    uint32_t low = 0;
                    if (text.size() - at < 2 || text[at] != '\\' || text[at + 1] != 'u') return Fail("unpaired surrogate");
                    at += 2;
                    if (!Hex4(low)) return Fail("bad \\u escape");
                    if (low < 0xDC00 || low > 0xDFFF) return Fail("unpaired surrogate");
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                at--;
                return Fail("bad escape");
            }
        }
    }

    // An integer as JSON writes it (-?(0|[1-9][0-9]*)); false for a fraction or an exponent too.
    bool Integer() {
        if (Next('-')) at++;
        if (Next('0')) {
            at++;
        } else {
            if (at == text.size() || text[at] < '1' || text[at] > '9') return false;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') at++;
        }
        return !(Next('.') || Next('e') || Next('E') || (at < text.size() && text[at] >= '0' && text[at] <= '9'));
    }
};

bool HasNul(const std::string& text) {
    return text.find('\0') != std::string::npos;
}

} // namespace

bool ParseRequest(std::string_view line, Request& request, std::string& error) {
    request.id.clear();
    request.input.clear();
    request.markdown.clear();
    request.sourcePath.clear();
    request.output.clear();
    request.args.clear();
    request.hasInput = request.hasMarkdown = request.hasSourcePath = false;

    Reader reader{ line, 0, error };
    if (!reader.Take('{')) return reader.Fail("expected a JSON object");
    unsigned seen = 0;
    std::string key;
    if (!reader.Take('}')) {
        for (;;) {
            if (!reader.Take('"')) return reader.Fail("expected a field name");
            if (!reader.String(key)) return false;
            if (!reader.Take(':')) return reader.Fail("expected ':'");
            unsigned field = 0;
            while (field < kFieldCount && key != kFieldNames[field]) field++;
            if (field == kFieldCount) {
                if (key.size() > 64) key.resize(64);
                return reader.Fail("unknown field \"" + key + "\"");
            }
            if (seen & (1u << field)) return reader.Fail("duplicate field \"" + key + "\"");
            seen |= 1u << field;
            reader.SkipSpace();
            const size_t valueStart = reader.at;
            if (field == kId) {
                if (reader.Next('"')) {
                    reader.at++;
                    if (!reader.String(key)) return false;
                } else if (!reader.Integer()) {
                    reader.at = valueStart;
                    return reader.Fail("id must be a string or an integer");
                }
                // Valid JSON as it was read: the answer carries it back as it came.
                request.id.assign(line.data() + valueStart, reader.at - valueStart);
            } else if (field == kArgs) {
                if (!reader.Take('[')) return reader.Fail("args must be an array of strings");
                if (!reader.Take(']')) {
                    for (;;) {
                        if (request.args.size() == kMaxArgs) {
                            return reader.Fail("more than " + std::to_string(kMaxArgs) + " args");
                        }
                        if (!reader.Take('"')) return reader.Fail("args must be an array of strings");
                        request.args.emplace_back();
                        if (!reader.String(request.args.back())) return false;
                        if (reader.Take(',')) continue;
                        if (reader.Take(']')) break;
                        return reader.Fail("expected ',' or ']'");
                    }
                }
            } else {
                if (!reader.Next('"')) return reader.Fail(std::string(kFieldNames[field]) + " must be a string");
                reader.at++;
                std::string& target = field == kInput ? request.input
                    : field == kMarkdown ? request.markdown
                    : field == kSourcePath ? request.sourcePath
                    : request.output;
                if (!reader.String(target)) return false;
            }
            if (reader.Take(',')) continue;
            if (reader.Take('}')) break;
            return reader.Fail("expected ',' or '}'");
        }
    }
    reader.SkipSpace();
    if (reader.at != line.size()) return reader.Fail("text after the request");

    request.hasInput = (seen & (1u << kInput)) != 0;
    request.hasMarkdown = (seen & (1u << kMarkdown)) != 0;
    request.hasSourcePath = (seen & (1u << kSourcePath)) != 0;
    if (request.hasInput == request.hasMarkdown) {
        error = request.hasInput ? "a request gives input or markdown, not both" : "a request needs input or markdown";
        return false;
    }
    if (request.hasSourcePath && !request.hasMarkdown) {
        error = "source_path goes with markdown";
        return false;
    }
    if (!(seen & (1u << kOutput))) {
        error = "a request needs output";
        return false;
    }
    if (request.hasInput && request.input.empty()) {
        error = "input is empty";
        return false;
    }
    if (request.output.empty()) {
        error = "output is empty";
        return false;
    }
    if (HasNul(request.input) || HasNul(request.sourcePath) || HasNul(request.output)) {
        error = "a path contains a NUL character";
        return false;
    }
    for (const std::string& arg : request.args) {
        if (HasNul(arg)) {
            error = "an argument contains a NUL character";
            return false;
        }
    }
    return true;
}

namespace {

bool StartsWithAsciiLower(std::string_view text, std::string_view lowerPrefix) {
    if (text.size() < lowerPrefix.size()) return false;
    for (size_t i = 0; i < lowerPrefix.size(); i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lowerPrefix[i]) return false;
    }
    return true;
}

} // namespace

bool IsBatchFlag(std::string_view arg) {
    for (std::string_view flag : { std::string_view("--recursive"), std::string_view("--skip-unchanged"),
             std::string_view("--follow-links") }) {
        if (arg.size() == flag.size() && StartsWithAsciiLower(arg, flag)) return true;
    }
    return StartsWithAsciiLower(arg, "--report=") || StartsWithAsciiLower(arg, "--workers=") ||
        StartsWithAsciiLower(arg, "--follow-links=");
}

LineReader::Status LineReader::Next(std::string& line, size_t limit) {
    line.clear();
    bool any = false;
    bool tooLong = false;
    for (;;) {
        if (begin_ == end_) {
            const size_t got = read_(context_, buffer_, sizeof(buffer_));
            if (got == 0) break;
            begin_ = 0;
            end_ = got;
        }
        any = true;
        const char* start = buffer_ + begin_;
        const size_t available = end_ - begin_;
        const void* found = std::memchr(start, '\n', available);
        const size_t take = found ? static_cast<size_t>(static_cast<const char*>(found) - start) : available;
        if (!tooLong) {
            // One byte more than the limit: the '\r' of a CRLF line.
            if (line.size() + take > limit + 1) {
                tooLong = true;
                line.clear();
            } else {
                line.append(start, take);
            }
        }
        begin_ += take;
        if (found) {
            begin_++;
            break;
        }
    }
    if (!any) return Status::End;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (tooLong || line.size() > limit) {
        line.clear();
        return Status::TooLong;
    }
    return Status::Line;
}

} // namespace RayoMd::Serve
