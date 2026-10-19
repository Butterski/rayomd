// SPDX-License-Identifier: Apache-2.0

#pragma once

// What both command lines share for batch export: the JSON Lines report, the up-to-date check
// that lets a stopped batch go on, and the Markdown files of a folder tree.

#include "path.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace RayoMd::Batch {

// One document of a batch: a line of the report.
struct Record {
    std::string input;           // UTF-8
    std::string output;          // UTF-8
    const char* status = "ok";   // "ok", "skipped" or "error"
    int code = 0;                // the exit code this document alone would give
    std::string error;           // why it failed, for "error"
    uint32_t pages = 0;
    uint64_t bytes = 0;
    double milliseconds = 0.0;   // reading, converting and writing
    uint32_t missingCharacters = 0;
    uint32_t failedImages = 0;
};

// A JSON Lines report: one object per document, written by whichever worker finished it.
// Lines are buffered and written at least once a second, so the report of a large batch can
// be followed while it runs.
class Report {
public:
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report();

    // `path` in UTF-8, or "-" for standard output. False when the file cannot be created.
    bool Open(const std::string& path);
    bool IsOpen() const { return file != nullptr; }
    void Write(const Record& record);
    // Writes what is buffered and closes the file; false when any write failed.
    bool Close();

private:
    void FlushLocked();

    std::mutex mutex;
    FILE* file = nullptr;
    bool ownsFile = false;
    bool failed = false;
    std::string buffer;
    std::chrono::steady_clock::time_point lastWrite;
};

// Appends `text` to `out` as the contents of a JSON string: quotes, backslashes and control
// characters escaped, and bytes that are not UTF-8 (possible in Linux file names) as U+FFFD.
void AppendJsonString(std::string& out, std::string_view text);

// Whether `output` is a complete PDF at least as new as `input`, the check --skip-unchanged
// makes, as make does. A PDF that a stopped run left cut short does not end with %%EOF.
bool OutputUpToDate(const RayoMd::File::Path& input, const RayoMd::File::Path& output);

struct File {
    RayoMd::File::Path input;
    RayoMd::File::Path output;
};

// The .md files of `inputDir`, with `recursive` also those of its subfolders except hidden ones
// (".git"), sorted by path, each with its PDF at the same relative place under `outputDir`.
// False, with `error`, when the folder cannot be read.
bool CollectMarkdown(const RayoMd::File::Path& inputDir, const RayoMd::File::Path& outputDir, bool recursive,
    std::vector<File>& files, std::string& error);

// The threads of a batch: `run(index, buffer)` for every index below `count`, each thread passing an
// output buffer of its own, on `workers` threads (0: one a CPU the process may run on, at most 6;
// never more than `count`).
// With more than one, the threads first take the inputs' sizes between them (`size(index)`,
// UINT64_MAX for none), as an input over 16 MiB leaves the batch at most two threads and one over
// 64 MiB one; a stat a file on one thread before was a serial part of every batch.
void RunJobs(size_t count, unsigned workers, uint64_t (*size)(void*, size_t),
    void (*run)(void*, size_t, std::string&), void* context);
template <class Size, class Run>
void RunJobs(size_t count, unsigned workers, Size&& size, Run&& run) {
    struct Context {
        std::remove_reference_t<Size>* size;
        std::remove_reference_t<Run>* run;
    } context{ &size, &run };
    RunJobs(count, workers, [](void* self, size_t index) -> uint64_t { return (*static_cast<Context*>(self)->size)(index); },
        [](void* self, size_t index, std::string& buffer) { (*static_cast<Context*>(self)->run)(index, buffer); }, &context);
}

} // namespace RayoMd::Batch
