// SPDX-License-Identifier: Apache-2.0

// The serve_request target: the request lines of `rayomd --serve --json` (src/cli/serve_request.h),
// untrusted input as far as the reader goes. The first byte chooses the size of the reads the
// line reader gets, the rest is standard input. Every request the reader accepts must hold the
// shape protocol 1 promises, and read again from its JSON text it must give the same fields.

#include "fuzz_targets.h"

#include "cli/serve_request.h"
#include "common/batch_report.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace RayoMdFuzz {

namespace {

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "serve_request: %s\n", what);
    std::abort();
}

struct Input {
    const uint8_t* data;
    size_t size;
    size_t at;
    size_t chunk;
};

size_t ReadChunk(void* context, char* buffer, size_t size) {
    Input& input = *static_cast<Input*>(context);
    const size_t take = std::min({ size, input.chunk, input.size - input.at });
    std::memcpy(buffer, input.data + input.at, take);
    input.at += take;
    return take;
}

void AppendField(std::string& out, const char* name, const std::string& value) {
    out += ",\"";
    out += name;
    out += "\":\"";
    RayoMd::Batch::AppendJsonString(out, value);
    out += '"';
}

// The request as JSON again: the id as it came, every string escaped as the report escapes them.
std::string Rewrite(const RayoMd::Serve::Request& request) {
    std::string out = "{\"output\":\"";
    RayoMd::Batch::AppendJsonString(out, request.output);
    out += '"';
    if (!request.id.empty()) {
        out += ",\"id\":";
        out += request.id;
    }
    if (request.hasInput) AppendField(out, "input", request.input);
    if (request.hasMarkdown) AppendField(out, "markdown", request.markdown);
    if (request.hasSourcePath) AppendField(out, "source_path", request.sourcePath);
    out += ",\"args\":[";
    for (size_t i = 0; i < request.args.size(); i++) {
        if (i != 0) out += ',';
        out += '"';
        RayoMd::Batch::AppendJsonString(out, request.args[i]);
        out += '"';
    }
    out += "]}";
    return out;
}

} // namespace

int ServeRequest(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    // Reads of 1 to 256 bytes, and lines of at most 4 KiB: a line split across reads at any byte,
    // and over-long lines, are what the limit below exercises.
    Input input{ data, size, 1, size_t(data[0]) + 1 };
    constexpr size_t kLimit = 4096;
    RayoMd::Serve::LineReader lines(ReadChunk, &input);
    RayoMd::Serve::Request request;
    RayoMd::Serve::Request again;
    std::string line;
    std::string error;
    for (;;) {
        const RayoMd::Serve::LineReader::Status status = lines.Next(line, kLimit);
        if (status == RayoMd::Serve::LineReader::Status::End) break;
        if (status == RayoMd::Serve::LineReader::Status::TooLong) {
            if (!line.empty()) Fail("an over-long line was kept");
            continue;
        }
        if (line.size() > kLimit) Fail("a line longer than the limit");
        if (line.find('\n') != std::string::npos) Fail("a line holds a line break");
        if (!RayoMd::Serve::ParseRequest(line, request, error)) {
            if (error.empty()) Fail("a refused request without an error");
            continue;
        }
        if (request.hasInput == request.hasMarkdown) Fail("a request without exactly one of input and markdown");
        if (request.output.empty() || (request.hasInput && request.input.empty())) Fail("an empty path");
        if (request.hasSourcePath && !request.hasMarkdown) Fail("source_path without markdown");
        if (request.args.size() > RayoMd::Serve::kMaxArgs) Fail("too many args");
        if (!request.id.empty() && request.id[0] != '"' && request.id[0] != '-' && (request.id[0] < '0' || request.id[0] > '9')) {
            Fail("an id that is neither a string nor an integer");
        }
        const std::string rewritten = Rewrite(request);
        if (!RayoMd::Serve::ParseRequest(rewritten, again, error)) Fail("a request refused once rewritten");
        if (again.id != request.id || again.input != request.input || again.markdown != request.markdown ||
            again.sourcePath != request.sourcePath || again.output != request.output || again.args != request.args ||
            again.hasInput != request.hasInput || again.hasMarkdown != request.hasMarkdown ||
            again.hasSourcePath != request.hasSourcePath) {
            Fail("a rewritten request reads differently");
        }
        (void)RayoMd::Serve::IsBatchFlag(line);
    }
    return 0;
}

} // namespace RayoMdFuzz
