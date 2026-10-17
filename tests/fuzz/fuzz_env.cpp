// SPDX-License-Identifier: Apache-2.0

#include "fuzz_targets.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifndef RAYOMD_FUZZ_SOURCE_DIR
#error "RAYOMD_FUZZ_SOURCE_DIR names the repository (tests/fuzz/CMakeLists.txt)"
#endif

// Under clang 23 with GCC 16's libstdc++, the temporary buffer of std::stable_sort (allocated by
// _GLIBCXX_OPERATOR_NEW, given back by _GLIBCXX_OPERATOR_DELETE, stl_tempbuf.h) is reported as an
// alloc-dealloc mismatch, "operator new vs free": the library's pair, not the engine's code.
extern "C" const char* __asan_default_options() {
    return "alloc_dealloc_mismatch=0";
}

namespace RayoMdFuzz {

const NamedTarget kTargets[] = {
    { "buildpdf", BuildPdf },
    { "pdfsource", PdfSource },
    { "theme", Theme },
    { "font", Font },
    { "cff", Cff },
    { "image", Image },
    { "svg", Svg },
    { "mermaid", Mermaid },
};
const size_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

namespace {

namespace fs = std::filesystem;

void CopyFlat(const fs::path& from, const fs::path& to) {
    std::error_code error;
    for (fs::directory_iterator it(from, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file()) fs::copy_file(it->path(), to / it->path().filename(), fs::copy_options::overwrite_existing, error);
    }
}

struct Scratch {
    std::string dir;
    Scratch() {
        const char* tmp = std::getenv("TMPDIR");
        std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/rayomd-fuzz-XXXXXX";
        if (!mkdtemp(&pattern[0])) {
            std::perror("rayomd-fuzz: mkdtemp");
            std::abort();
        }
        dir = pattern;
        const fs::path root(dir);
        const fs::path seeds = fs::path(RAYOMD_FUZZ_SOURCE_DIR) / "tests" / "fuzz" / "seeds";
        CopyFlat(seeds / "image", root);
        CopyFlat(seeds / "svg", root);
        CopyFlat(seeds / "font", root);
        // tester.md's own image.
        std::error_code error;
        fs::create_directories(root / "docs" / "assets" / "branding", error);
        fs::copy_file(fs::path(RAYOMD_FUZZ_SOURCE_DIR) / "docs" / "assets" / "branding" / "rayomd.png",
            root / "docs" / "assets" / "branding" / "rayomd.png", fs::copy_options::overwrite_existing, error);
        fs::create_directories(root / "sub", error);
        CopyFlat(seeds / "image", root / "sub");
        if (const char* fifo = std::getenv("RAYOMD_FUZZ_FIFO"); fifo && *fifo) {
            mkfifo((root / "fifo.png").c_str(), 0600);
        }
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(dir, error);
    }
};

struct Job {
    Target target;
    const uint8_t* data;
    size_t size;
    int result;
};

void* RunJob(void* argument) {
    Job& job = *static_cast<Job*>(argument);
    job.result = job.target(job.data, job.size);
    return nullptr;
}

} // namespace

const std::string& ScratchDir() {
    static const Scratch scratch;
    return scratch.dir;
}

int RunTarget(Target target, const uint8_t* data, size_t size) {
    ScratchDir();
    Job job{ target, data, size, 0 };
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 2u * 1024u * 1024u);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, RunJob, &job) != 0) {
        std::perror("rayomd-fuzz: pthread_create");
        std::abort();
    }
    pthread_attr_destroy(&attributes);
    pthread_join(thread, nullptr);
    return job.result;
}

} // namespace RayoMdFuzz
