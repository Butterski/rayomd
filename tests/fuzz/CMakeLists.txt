# SPDX-License-Identifier: Apache-2.0

# The fuzz targets of the engine's untrusted-input entry points (fuzz_targets.h). Two builds:
# - RAYOMD_FUZZ=ON (clang): one libFuzzer executable per target, rayomd-fuzz-<name>, the engine and
#   the targets instrumented with -fsanitize=fuzzer-no-link,address,undefined.
# - Any test build on Linux and the like: rayomd-fuzz-replay and the ctest entry rayomd-fuzz-replay,
#   which run tests/fuzz/regressions and tests/fuzz/seeds through the same targets, no libFuzzer.
# fuzz_internal.cpp compiles src/core/tiny_pdf.cpp inside itself, so the engine's other sources are
# listed here without it.

set(rayomd_fuzz_engine ${RAYOMD_CORE_SOURCES})
list(REMOVE_ITEM rayomd_fuzz_engine src/core/tiny_pdf.cpp)
list(TRANSFORM rayomd_fuzz_engine PREPEND "${PROJECT_SOURCE_DIR}/")

add_library(rayomd_fuzz_targets STATIC EXCLUDE_FROM_ALL
    ${rayomd_fuzz_engine}
    fuzz_env.cpp
    fuzz_public.cpp
    fuzz_internal.cpp)
target_include_directories(rayomd_fuzz_targets PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(rayomd_fuzz_targets PUBLIC
    RAYOMD_VERSION="${PROJECT_VERSION}" RAYOMD_FUZZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(rayomd_fuzz_targets PUBLIC Threads::Threads)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # The replay runs small inputs: -O1 halves the time tiny_pdf.cpp takes to compile once more.
    target_compile_options(rayomd_fuzz_targets PRIVATE -O1)
endif()

add_executable(rayomd-fuzz-replay replay_main.cpp)
target_link_libraries(rayomd-fuzz-replay PRIVATE rayomd_fuzz_targets)

if(RAYOMD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RAYOMD_FUZZ needs clang (libFuzzer): -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
    endif()
    set(rayomd_fuzz_flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1)
    target_compile_options(rayomd_fuzz_targets PUBLIC ${rayomd_fuzz_flags} -fsanitize=fuzzer-no-link)
    target_link_options(rayomd_fuzz_targets PUBLIC -fsanitize=address,undefined)
    # Each target's function, as fuzz_targets.h declares it.
    set(function_buildpdf BuildPdf)
    set(function_pdfsource PdfSource)
    set(function_theme Theme)
    set(function_font Font)
    set(function_cff Cff)
    set(function_image Image)
    set(function_svg Svg)
    set(function_mermaid Mermaid)
    foreach(name buildpdf pdfsource theme font cff image svg mermaid)
        add_executable(rayomd-fuzz-${name} fuzz_main.cpp)
        target_compile_definitions(rayomd-fuzz-${name} PRIVATE RAYOMD_FUZZ_TARGET=${function_${name}})
        target_link_libraries(rayomd-fuzz-${name} PRIVATE rayomd_fuzz_targets)
        target_link_options(rayomd-fuzz-${name} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(NAME rayomd-fuzz-replay
        COMMAND rayomd-fuzz-replay "${CMAKE_CURRENT_SOURCE_DIR}/regressions" "${CMAKE_CURRENT_SOURCE_DIR}/seeds")
endif()
