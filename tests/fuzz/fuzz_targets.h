// SPDX-License-Identifier: Apache-2.0

#pragma once

// The engine's entry points for untrusted input, each as a fuzz target: a function of the input
// bytes that returns 0 and aborts (or trips a sanitizer) when the engine misbehaves. One libFuzzer
// executable per target (fuzz_main.cpp, RAYOMD_FUZZ=ON with clang) and the plain replay driver
// of ctest (replay_main.cpp) call them the same way, through RunTarget.

#include <cstddef>
#include <cstdint>
#include <string>

namespace RayoMdFuzz {

using Target = int (*)(const uint8_t* data, size_t size);

// Markdown through TinyPdf::BuildPdf. The first two bytes choose the options (OptionsFrom in
// fuzz_public.cpp), the rest is the document, whose local images resolve in the scratch folder.
int BuildPdf(const uint8_t* data, size_t size);
// A PDF through RayoMd::PdfSource::Inspect, with and without recovering the source.
int PdfSource(const uint8_t* data, size_t size);
// A theme file through TinyPdf::Internal::ParseTheme, then a build with the theme it gives.
int Theme(const uint8_t* data, size_t size);
// A TrueType or OpenType font: loaded as the engine loads a theme's font and subset by two builds.
int Font(const uint8_t* data, size_t size);
// A bare 'CFF ' table after a 2-byte big-endian glyph count: Cff::ParseCff, Desubroutinize and
// WriteCidKeyedSubset.
int Cff(const uint8_t* data, size_t size);
// Image bytes through the engine's PNG and JPEG decoding (DecodeImageBytes).
int Image(const uint8_t* data, size_t size);
// SVG markup through ConvertSvg with both of the engine's hosts (standard and TrueType fonts).
int Svg(const uint8_t* data, size_t size);

struct NamedTarget {
    const char* name;
    Target target;
};
extern const NamedTarget kTargets[];
extern const size_t kTargetCount;

// The scratch folder of the process: the document folder of BuildPdf (doc.md), holding copies of
// tests/fuzz/seeds/image and tests/fuzz/seeds/svg, and the fonts of tests/fuzz/seeds/font.
// Made on first use, removed at exit. With RAYOMD_FUZZ_FIFO set, it also holds a FIFO, fifo.png.
const std::string& ScratchDir();

// Runs `target` on a thread of 2 MiB of stack, what a MinGW thread gets (the GUI builds on one):
// recursion that a Linux main thread's 8 MiB would hide fails here too.
int RunTarget(Target target, const uint8_t* data, size_t size);

} // namespace RayoMdFuzz
