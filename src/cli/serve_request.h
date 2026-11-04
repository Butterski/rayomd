// SPDX-License-Identifier: Apache-2.0

#pragma once

// A request of `rayomd --serve --json`, protocol 1: one JSON object a line, of the fields below
// and no other. The reader takes exactly that shape (strings, numbers, booleans and arrays of
// strings as values; no nesting, no null) and rejects anything else, so it reads each byte once
// and holds nothing but the request. Both command lines share it; the fuzz target serve_request
// reads untrusted lines through it.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace RayoMd::Serve {

constexpr int kProtocol = 1;
// The longest request line, without its line break: larger documents go by `input`.
constexpr size_t kMaxRequestBytes = size_t(32) << 20;
// The most `args` a request may have.
constexpr size_t kMaxArgs = 256;

struct Request {
    // The id as it was sent, as JSON text (a string with its quotes, or an integer); empty for
    // none. The answer carries it back unchanged.
    std::string id;
    // UTF-8. Exactly one of `input` (a Markdown file) and `markdown` (its text) is given;
    // `sourcePath` only with `markdown`, the file the text stands for.
    std::string input;
    std::string markdown;
    std::string sourcePath;
    std::string output;
    std::vector<std::string> args;
    bool hasInput = false;
    bool hasMarkdown = false;
    bool hasSourcePath = false;
};

// Reads `line` into `request`, whose buffers are reused from request to request. False, with
// `error`, when it is not a request of protocol 1; `request.id` is kept once it was read, so the
// answer to a request with a bad field still carries it.
bool ParseRequest(std::string_view line, Request& request, std::string& error);

// Whether `arg` is a flag of the batch modes or of books (--report, --workers, --recursive,
// --skip-unchanged, --follow-links), which a serve session refuses: its answers are the report.
bool IsBatchFlag(std::string_view arg);

// The request lines of standard input, through a buffer of its own: `read` fills it with what
// the input has (a read(2) or ReadFile, never a stdio call that waits for a full buffer), and
// returns 0 at the end or on an error.
class LineReader {
public:
    using ReadFunction = size_t (*)(void* context, char* buffer, size_t size);
    enum class Status { Line, TooLong, End };

    LineReader(ReadFunction read, void* context) : read_(read), context_(context) {}

    // The next line, without its line break and a '\r' before it. A line longer than `limit`
    // bytes is read to its end without being kept and is TooLong; End when the input is over.
    Status Next(std::string& line, size_t limit);

private:
    ReadFunction read_;
    void* context_;
    size_t begin_ = 0;
    size_t end_ = 0;
    char buffer_[64 * 1024];
};

} // namespace RayoMd::Serve
