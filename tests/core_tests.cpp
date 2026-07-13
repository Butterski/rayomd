#include "rayomd/tiny_pdf.h"
#include "../src/common/text_utils.h"
#include "../src/core/rayomd_pdf_source.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef RAYOMD_TEST_SOURCE_DIR
#define RAYOMD_TEST_SOURCE_DIR "."
#endif

namespace {
bool ValidPdf(const std::string& pdf) {
    return pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos &&
        pdf.find("/Type /Page") != std::string::npos;
}
bool CheckFormatter() {
    struct Case { double value; const char* expected; };
    const Case cases[] = {
        {0.0, "0"}, {-0.0, "-0"}, {1.0, "1"}, {-1.25, "-1.25"},
        {12.30, "12.3"}, {12.345, "12.35"}, {595.0, "595"}, {0.004, "0"}
    };
    for (const Case& item : cases) {
        std::string actual;
        RayoMd::Text::AppendFixed2(actual, item.value);
        if (actual != item.expected) {
            std::cerr << "formatter mismatch: " << actual << " != " << item.expected << std::endl;
            return false;
        }
    }
    return true;
}
bool Build(const std::string& markdown, std::string& output) {
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.margin = TinyPdf::PdfMargin::Normal();
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    return TinyPdf::BuildPdf(markdown, options, output).Ok() && ValidPdf(output);
}
bool BuildReversible(const std::string& markdown, std::string& output) {
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.margin = TinyPdf::PdfMargin::Normal();
    options.embedSource = true;
    return TinyPdf::BuildPdf(markdown, options, output).Ok() && ValidPdf(output);
}
} // namespace

int main() {
    if (!CheckFormatter()) return 1;
    const std::vector<std::string> documents = {
        "# ASCII\n\nFast **native** export with a paragraph and a rule.\n\n---\n",
        u8"# Unicode\n\nZa\u017C\u00F3\u0142\u0107 g\u0119\u015Bl\u0105 ja\u017A\u0144. \u65E5\u672C\u8A9E \u0395\u03BB\u03BB\u03B7\u03BD\u03B9\u03BA\u03AC.\n",
        "# Links\n\n[one](https://example.com/one) and [two](https://example.com/two).\n",
        "# Local image\n\n![RayoMD](docs/assets/branding/rayomd.png)\n",
        "# Failed image\n\n![missing](docs/assets/branding/does-not-exist.png)\n",
        "# Mixed\n\n| Left | Right |\n|---|---:|\n| alpha | 42 |\n| wrapped cell content | 9000 |\n\n> quote\n\n- one\n- two\n"
    };
    std::vector<std::string> expected(documents.size());
    for (size_t i = 0; i < documents.size(); i++) {
        if (!Build(documents[i], expected[i])) {
            std::cerr << "baseline build failed for document " << i << std::endl;
            return 2;
        }
    }
    if (expected[2].find("/Subtype /Link") == std::string::npos) return 3;
    if (expected[3].find("/Subtype /Image") == std::string::npos) return 4;
    if (RayoMd::PdfSource::Sha256Hex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "SHA-256 test vector failed" << std::endl;
        return 7;
    }
    for (const std::string& source : {
        std::string(),
        std::string("# Exact\r\n\r\n<!-- hidden -->\r\n"),
        std::string("# Unicode\n\nEmoji: \xF0\x9F\x98\x80\n")
    }) {
        std::string reversible;
        if (!BuildReversible(source, reversible) || reversible.rfind("%PDF-2.0", 0) != 0) {
            std::cerr << "reversible build failed" << std::endl;
            return 8;
        }
        RayoMd::PdfSource::Result recovered = RayoMd::PdfSource::Inspect(reversible, true);
        if (!recovered.Ok() || recovered.source != source || !recovered.info.digestValid) {
            std::cerr << "exact recovery failed" << std::endl;
            return 9;
        }
        size_t payload = source.empty() ? std::string::npos : reversible.find(source);
        if (payload != std::string::npos) {
            reversible[payload] ^= 1;
            if (RayoMd::PdfSource::Inspect(reversible, false).status !=
                RayoMd::PdfSource::Status::IntegrityMismatch) {
                std::cerr << "tamper detection failed" << std::endl;
                return 10;
            }
        }
    }
    if (RayoMd::PdfSource::Inspect(expected.front(), false).status !=
        RayoMd::PdfSource::Status::NotReversible) return 11;
    std::string invalidUtf8(1, static_cast<char>(0xff));
    TinyPdf::PdfOptions invalidOptions;
    invalidOptions.embedSource = true;
    std::string ignoredPdf;
    if (TinyPdf::BuildPdf(invalidUtf8, invalidOptions, ignoredPdf).error !=
        TinyPdf::BuildError::InvalidSourceUtf8) return 12;

    if (expected[4].find("/Subtype /Image") != std::string::npos) return 5;

    for (unsigned workerCount : {1u, 2u, 4u, 6u}) {
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < workerCount; worker++) {
            workers.emplace_back([&, worker] {
                for (unsigned iteration = 0; iteration < 20 && !failed.load(); iteration++) {
                    size_t index = (worker + iteration) % documents.size();
                    std::string pdf;
                    if (!Build(documents[index], pdf) || pdf != expected[index]) {
                        failed.store(true);
                        break;
                    }
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        if (failed.load()) {
            std::cerr << "concurrency stress failed with " << workerCount << " worker(s)" << std::endl;
            return 6;
        }
    }
    std::cout << "RayoMD formatter and 1/2/4/6-worker concurrency stress passed" << std::endl;
    return 0;
}