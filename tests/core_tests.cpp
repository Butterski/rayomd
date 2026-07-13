#include "rayomd/tiny_pdf.h"
#include "../src/common/text_utils.h"
#include "../src/core/rayomd_pdf_source.h"

#include <atomic>
#include <cstdio>
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
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    options.embedSource = true;
    return TinyPdf::BuildPdf(markdown, options, output).Ok() && ValidPdf(output);
}

struct FixtureOptions {
    std::string attachmentName = "source.md";
    std::string profile = "rayomd-source/1";
    std::string catalogExtra;
    std::string sourceExtra;
    std::string fileSpecExtra;
    std::string metadataExtra;
    std::string trailerExtra;
    size_t sourceReference = 2;
    size_t metadataReference = 4;
    long long sourceLengthDelta = 0;
    bool includeMetadata = true;
    bool duplicateNameEntry = false;
    bool additionalNameEntry = false;
};

std::string BuildClassicProfileFixture(const std::string& source, const FixtureOptions& options = {}) {
    std::string xmp = RayoMd::PdfSource::BuildXmpMetadata(source, "fixture");
    size_t profile = xmp.find("rayomd-source/1");
    if (profile != std::string::npos) xmp.replace(profile, 15, options.profile);
    std::string names = "[(" + options.attachmentName + ") 3 0 R";
    if (options.duplicateNameEntry) names += " (" + options.attachmentName + ") 3 0 R";
    if (options.additionalNameEntry) names += " (notes.md) 3 0 R";
    names += "]";
    std::string catalog = "<< /Type /Catalog";
    if (options.includeMetadata) catalog += " /Metadata " + std::to_string(options.metadataReference) + " 0 R";
    catalog += " /Names << /EmbeddedFiles << /Names " + names + " >> >> /AF [3 0 R]" +
        options.catalogExtra + " >>";
    long long declaredLength = static_cast<long long>(source.size()) + options.sourceLengthDelta;
    std::string sourceObject = "<< /Type /EmbeddedFile /Subtype /text#2Fmarkdown /Params << /Size " +
        std::to_string(source.size()) + " >> /Length " + std::to_string(declaredLength) + options.sourceExtra +
        " >>\nstream\n" + source + "\nendstream";
    std::string fileSpec = "<< /Type /Filespec /F (" + options.attachmentName + ") /UF (" +
        options.attachmentName + ") /EF << /F " + std::to_string(options.sourceReference) +
        " 0 R /UF " + std::to_string(options.sourceReference) +
        " 0 R >> /AFRelationship /Source" + options.fileSpecExtra + " >>";
    std::string metadata = "<< /Type /Metadata /Subtype /XML /Length " + std::to_string(xmp.size()) +
        options.metadataExtra + " >>\nstream\n" + xmp + "\nendstream";
    std::vector<std::string> objects = {catalog, sourceObject, fileSpec, metadata};

    std::string pdf = "%PDF-2.0\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(1, 0);
    for (size_t i = 0; i < objects.size(); i++) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    char entry[32];
    for (size_t i = 1; i < offsets.size(); i++) {
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[i]);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R" +
        options.trailerExtra + " >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

bool ExpectStatus(const std::string& pdf, RayoMd::PdfSource::Status status, const char* label) {
    RayoMd::PdfSource::Status actual = RayoMd::PdfSource::Inspect(pdf, true).status;
    if (actual == status) return true;
    std::cerr << label << " status mismatch: " << static_cast<int>(actual) << " != "
        << static_cast<int>(status) << std::endl;
    return false;
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
        "# Mixed\n\n| Left | Right |\n|---|---:|\n| alpha | 42 |\n| wrapped cell content | 9000 |\n\n> quote\n\n- one\n- two\n",
        "# Remote fallback\n\n![blocked](https://127.0.0.1/image.png)\n",
        "# Multipage\n\n" + std::string(24000, 'x') + "\n"
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
    const std::vector<std::string> exactSources = {
        std::string(),
        std::string("# ASCII LF\n\nplain\n"),
        std::string("# Exact\r\n\r\n<!-- hidden -->\r\n"),
        std::string("# Unicode\n\nEmoji: \xF0\x9F\x98\x80\n"),
        std::string("\xEF\xBB\xBF# BOM\n"),
        std::string("---\nprivate: true\n---\n\n:::unsupported value\n"),
        std::string(" \t\n\r\n  \n"),
        std::string("a\0b\n", 4)
    };
    for (const std::string& source : exactSources) {
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

    for (TinyPdf::PdfStyle style : {TinyPdf::PdfStyle::Elegant, TinyPdf::PdfStyle::Modern, TinyPdf::PdfStyle::Tech}) {
        for (TinyPdf::PdfMargin margin : {TinyPdf::PdfMargin::Compact(), TinyPdf::PdfMargin::Normal(),
             TinyPdf::PdfMargin::Wide(), TinyPdf::PdfMargin::CustomPoints(54.0)}) {
            TinyPdf::PdfOptions options;
            options.style = style;
            options.margin = margin;
            options.embedSource = true;
            std::string source = style == TinyPdf::PdfStyle::Tech ? "# Matrix \xF0\x9F\x98\x80\n" : "# Matrix\n";
            std::string pdf;
            if (!TinyPdf::BuildPdf(source, options, pdf).Ok() ||
                RayoMd::PdfSource::Inspect(pdf, true).source != source) return 13;
        }
    }

    for (size_t i = 0; i < documents.size(); i++) {
        const std::string& source = documents[i];
        std::string pdf;
        if (!BuildReversible(source, pdf) || RayoMd::PdfSource::Inspect(pdf, true).source != source) return 14;
        if (i == 2 && pdf.find("/Subtype /Link") == std::string::npos) return 33;
        if (i == 3 && pdf.find("/Subtype /Image") == std::string::npos) return 34;
        if (i == 7 && pdf.find("/Count 1 /Kids") != std::string::npos) return 35;
    }

    const std::string fixtureSource = "# hostile fixture\n";
    std::string fixture = BuildClassicProfileFixture(fixtureSource);
    RayoMd::PdfSource::Result fixtureResult = RayoMd::PdfSource::Inspect(fixture, true);
    if (!fixtureResult.Ok() || fixtureResult.source != fixtureSource) return 15;

    FixtureOptions hostile;
    hostile.sourceExtra = " /Type /EmbeddedFile";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "duplicate key")) return 16;
    hostile = {};
    hostile.sourceExtra = " /Filter /FlateDecode";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "unsupported filter")) return 17;
    hostile.sourceExtra = " /Filter [/FlateDecode /FlateDecode]";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "repeated filters")) return 18;
    hostile = {};
    hostile.sourceLengthDelta = 1;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "bad stream length")) return 19;
    hostile = {};
    hostile.profile = "rayomd-source/2";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::UnsupportedProfile, "unsupported profile")) return 20;
    hostile = {};
    hostile.attachmentName = "../evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "path traversal name")) return 21;
    hostile.attachmentName = "C:\\\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "absolute name")) return 22;
    hostile.attachmentName = "\\\\server\\share\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "UNC name")) return 44;
    hostile.attachmentName = "\\\\?\\C:\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "device name")) return 45;
    hostile = {};
    hostile.duplicateNameEntry = true;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "duplicate attachment")) return 23;
    hostile = {};
    hostile.additionalNameEntry = true;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "multiple attachments")) return 46;
    hostile = {};
    hostile.sourceReference = 3;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "cyclic source reference")) return 24;
    hostile = {};
    hostile.metadataReference = 1000001;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::NotReversible, "huge object id")) return 25;
    hostile = {};
    hostile.trailerExtra = " /Prev 1";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "incremental update")) return 26;
    hostile.trailerExtra = " /XRefStm 1";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "xref stream")) return 27;
    hostile.trailerExtra = " /Encrypt 4 0 R";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "encrypted PDF")) return 28;
    hostile = {};
    hostile.catalogExtra = " /Deep " + std::string(18, '[') + "0" + std::string(18, ']');
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "deep arrays")) return 29;
    hostile.catalogExtra = " /Deep " + std::string(17, '<') + std::string(17, '<') +
        " /Value 0 " + std::string(17, '>') + std::string(17, '>');
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "deep dictionaries")) return 47;
    hostile = {};
    hostile.includeMetadata = false;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::NotReversible, "unrelated attachment")) return 30;
    std::string invalidFixture(1, static_cast<char>(0xff));
    if (!ExpectStatus(BuildClassicProfileFixture(invalidFixture),
        RayoMd::PdfSource::Status::InvalidUtf8, "invalid UTF-8")) return 31;
    std::string truncated = fixture.substr(0, fixture.size() - 8);
    if (!ExpectStatus(truncated, RayoMd::PdfSource::Status::CorruptPdf, "truncated PDF")) return 32;
    std::string badOffset = fixture;
    size_t freeEntry = badOffset.find("0000000000 65535 f \n");
    if (freeEntry == std::string::npos) return 36;
    badOffset[freeEntry + 20] = '9';
    if (!ExpectStatus(badOffset, RayoMd::PdfSource::Status::CorruptPdf, "bad xref offset")) return 37;
    std::string badState = fixture;
    size_t activeState = badState.find(" 00000 n \n");
    if (activeState == std::string::npos) return 38;
    badState[activeState + 7] = 'q';
    if (!ExpectStatus(badState, RayoMd::PdfSource::Status::CorruptPdf, "malformed xref entry")) return 39;
    std::string badObjectId = fixture;
    size_t firstObject = badObjectId.find("1 0 obj");
    if (firstObject == std::string::npos) return 40;
    badObjectId[firstObject] = '9';
    if (!ExpectStatus(badObjectId, RayoMd::PdfSource::Status::CorruptPdf, "object id mismatch")) return 41;
    if (!ExpectStatus(fixture + "junk", RayoMd::PdfSource::Status::CorruptPdf, "trailing bytes")) return 42;

    std::string tooLarge(RayoMd::PdfSource::kMaxSourceBytes + 1, 'x');
    TinyPdf::PdfOptions tooLargeOptions;
    tooLargeOptions.embedSource = true;
    if (TinyPdf::BuildPdf(tooLarge, tooLargeOptions, ignoredPdf).error !=
        TinyPdf::BuildError::SourceTooLarge) return 43;

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
    std::cout << "RayoMD formatter, reversible-profile hardening, and 1/2/4/6-worker concurrency stress passed"
              << std::endl;
    return 0;
}
