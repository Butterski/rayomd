# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOMD_VERSION LIMIT_COUNT 1)
project(rayomd VERSION ${RAYOMD_VERSION} LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(RAYOMD_TOP_LEVEL ON)
else()
    set(RAYOMD_TOP_LEVEL OFF)
endif()
option(RAYOMD_BUILD_CLI "Build the rayomd command-line tool" ${RAYOMD_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(RAYOMD_USE_CURL "Use libcurl for HTTP/HTTPS image URLs on non-Windows" OFF)
option(RAYOMD_ENABLE_LTO "Enable interprocedural optimization for measured release experiments" OFF)
option(RAYOMD_ENABLE_PROFILING "Enable phase timing and allocation counters" OFF)
option(RAYOMD_ENABLE_TSAN "Enable ThreadSanitizer on supported non-Windows builds" OFF)
# A fully static Linux CLI, glibc linked in: no runtime dependency and a process that starts
# about 0.3 ms sooner, for about 0.9 MB more executable. The release ships it beside the default
# build. musl instead of glibc was 0.9 MB smaller but its allocator slowed exports.
option(RAYOMD_STATIC "Link the Linux CLI fully statically, glibc included" OFF)
if(RAYOMD_STATIC AND (WIN32 OR APPLE))
    message(FATAL_ERROR "RAYOMD_STATIC is for Linux; Windows release builds are static already.")
endif()
if(RAYOMD_STATIC AND RAYOMD_USE_CURL)
    message(FATAL_ERROR "RAYOMD_STATIC cannot link libcurl and its TLS stack statically.")
endif()
# libFuzzer targets of the engine's untrusted-input entry points (tests/fuzz/), clang only. The
# normal build never builds them; a test build runs their regression inputs without libFuzzer.
option(RAYOMD_FUZZ "Build the libFuzzer targets of tests/fuzz (clang, ASan and UBSan)" OFF)

function(rayomd_release_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(WIN32)
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-O2>
                $<$<CONFIG:Release>:-fno-exceptions>
                $<$<CONFIG:Release>:-fno-rtti>
                $<$<CONFIG:Release>:-ffunction-sections>
                $<$<CONFIG:Release>:-fdata-sections>)
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-s>
                $<$<CONFIG:Release>:-static-libgcc>
                $<$<CONFIG:Release>:-static-libstdc++>
                $<$<CONFIG:Release>:-static>
                $<$<CONFIG:Release>:-Wl,--gc-sections>)
        elseif(APPLE)
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O3>)
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-s>)
        else()
            # The C++ runtime is linked in, its unused sections dropped: a process starts 0.6 ms
            # sooner without loading and relocating libstdc++.so, and calls into it skip the PLT
            # (warm builds 5 % faster) for about 180 KB. The code uses no iostreams, which would
            # pull in some 460 KB of locale and stream code here.
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-O3>
                $<$<CONFIG:Release>:-ffunction-sections>
                $<$<CONFIG:Release>:-fdata-sections>)
            if(RAYOMD_STATIC)
                # Position-independent, so the kernel maps it at a random address as it does the
                # dynamic build: -static alone overrides the compiler's default PIE and makes an
                # ET_EXEC at a fixed address, a gift to any memory-safety bug in reading untrusted
                # Markdown, SVG, PNG and fonts. glibc relocates it itself at start. Without
                # -static-libgcc and -static-libstdc++, which GCC 13 follows with -Bdynamic even
                # under -static-pie: libc was then linked dynamically, and the program crashed.
                target_compile_options(${target} PRIVATE -fPIE)
                target_link_options(${target} PRIVATE
                    $<$<CONFIG:Release>:-s>
                    $<$<CONFIG:Release>:-Wl,--gc-sections>
                    -static-pie)
            else()
                target_link_options(${target} PRIVATE
                    $<$<CONFIG:Release>:-s>
                    $<$<CONFIG:Release>:-static-libgcc>
                    $<$<CONFIG:Release>:-static-libstdc++>
                    $<$<CONFIG:Release>:-Wl,--gc-sections>)
            endif()
        endif()
    endif()
endfunction()

# Warnings for RayoMD's own C++ (GCC and Clang): -Wall -Wextra -Wshadow, as cpp-best-practices
# recommends. Not errors: a newer compiler's new warnings must not break a build.
function(rayomd_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wshadow>")
    endif()
endfunction()

function(rayomd_common_definitions target)
    target_compile_definitions(${target} PRIVATE RAYOMD_VERSION="${PROJECT_VERSION}")
endfunction()

# A dynamic glibc build does not link libm: rounding, sqrt and fmod are RayoMD's own
# (src/common/exact_math.h) and svg.cpp loads libm.so.6 for its transcendental functions when an
# SVG first needs one. A fresh export then starts 0.07 ms sooner (10 page faults fewer); one with
# such an SVG takes 0.04 ms longer. --as-needed drops the -lm the C++ driver adds. Static builds
# link libm.a as before.
set(RAYOMD_LIBM_DLOPEN OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT RAYOMD_STATIC)
    include(CheckSymbolExists)
    check_symbol_exists(gnu_get_libc_version "gnu/libc-version.h" RAYOMD_HAVE_GLIBC)
    if(RAYOMD_HAVE_GLIBC)
        set(RAYOMD_LIBM_DLOPEN ON)
        check_symbol_exists(dlopen "dlfcn.h" RAYOMD_DLOPEN_IN_LIBC)
    endif()
endif()

function(rayomd_libm target)
    if(RAYOMD_LIBM_DLOPEN)
        target_compile_definitions(${target} PRIVATE RAYOMD_LIBM_DLOPEN=1)
        target_link_options(${target} PRIVATE -Wl,--as-needed)
        if(NOT RAYOMD_DLOPEN_IN_LIBC)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
endfunction()

# Release builds with GNU ld on Linux and the like place the code fresh exports run first, in
# the order of src/hot.ld (generated by scripts/hot_sections.py; an INSERT script, so the default
# layout stays for the rest): fewer 64 KB windows of code to fault in. Names it lists that no
# longer exist match nothing.
set(RAYOMD_HOT_SECTIONS OFF)
if(NOT WIN32 AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    separate_arguments(rayomd_linker_flags UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${rayomd_linker_flags} -Wl,--version
        OUTPUT_VARIABLE rayomd_linker_version ERROR_QUIET)
    if(rayomd_linker_version MATCHES "^GNU ld")
        set(RAYOMD_HOT_SECTIONS ON)
    endif()
endif()

function(rayomd_hot_sections target)
    if(RAYOMD_HOT_SECTIONS)
        set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/hot.ld")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:-Wl,-T,${script}>")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${script}")
    endif()
endfunction()

function(rayomd_experiments target)
    if(RAYOMD_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT rayomd_ipo_supported OUTPUT rayomd_ipo_error)
        if(NOT rayomd_ipo_supported)
            message(FATAL_ERROR "RAYOMD_ENABLE_LTO is unsupported: ${rayomd_ipo_error}")
        endif()
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()

    if(RAYOMD_ENABLE_PROFILING)
        target_sources(${target} PRIVATE src/common/profiling.cpp)
        target_compile_definitions(${target} PRIVATE RAYOMD_ENABLE_PROFILING=1)
    endif()

    if(RAYOMD_ENABLE_TSAN)
        if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "RAYOMD_ENABLE_TSAN requires GCC or Clang on a non-Windows build")
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

# Recovery is cold, opt-in code, the math parser and layout run only for documents with
# formulas, options and themes are parsed once per run, a theme's header, footer and cover,
# PDF/A's metadata and the footnotes' numbering are made once per document, an OpenType font's
# CFF table is read once and subset once per document, an HTML block is mapped once, and the
# image encoder's threads only hand out segments that flate.cpp compresses. Optimize these
# translation units for package size while leaving the performance-critical renderer and parser
# on the normal release flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        src/core/rayomd_pdf_source.cpp
        src/core/image_cache.cpp
        src/core/image_encoder.cpp
        src/core/math_parser.cpp
        src/core/math_layout.cpp
        src/core/export_options.cpp
        src/core/theme.cpp
        src/core/pdfa.cpp
        src/core/footnotes.cpp
        src/core/contents.cpp
        src/core/book.cpp
        src/core/svg.cpp
        src/core/mermaid.cpp
        src/core/cff.cpp
        src/core/tagged.cpp
        src/core/html_blocks.cpp
        src/core/access_policy.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release>:-Os>")
    # The DEFLATE encoder of --compress and re-encoded images: -O2 is within 1 % of -O3 on
    # RayoMD's streams at half the code; the inflater, 1.5 % slower than -O3 at 8.9 KB of code
    # against 17.7. The highlighter's lexers read every byte of a code block: 1.5 times as fast at
    # -O2 as at -Os. The HTML scanner reads every byte of the tags in a document.
    set_source_files_properties(src/core/flate.cpp src/core/inflate.cpp src/core/highlight.cpp src/core/html.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release>:-O2>")
endif()

# libdeflate's decompressor (third_party/libdeflate, v1.26, the files it needs and unchanged) is
# the tests' independent reference for RayoMD's own inflater (src/core/inflate.cpp) and encoder
# (src/core/flate.cpp); no product links it.
add_library(rayomd_inflate STATIC EXCLUDE_FROM_ALL
    third_party/libdeflate/lib/deflate_decompress.c
    third_party/libdeflate/lib/utils.c
    third_party/libdeflate/lib/x86/cpu_features.c)
target_include_directories(rayomd_inflate PUBLIC third_party/libdeflate)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rayomd_inflate PRIVATE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>)
endif()

# The exporter's sources, shared by both command lines and the core tests. The order is the
# Linux link order: changing it moves code in the binary.
set(RAYOMD_CORE_SOURCES
    src/core/tiny_pdf.cpp
    src/core/theme.cpp
    src/core/pdfa.cpp
    src/core/png_filter.cpp
    src/core/flate.cpp
    src/core/image_cache.cpp
    src/core/image_encoder.cpp
    src/core/inflate.cpp
    src/core/export_options.cpp
    src/core/inline_markdown.cpp
    src/core/markdown_parser.cpp
    src/core/footnotes.cpp
    src/core/highlight.cpp
    src/core/html.cpp
    src/core/html_blocks.cpp
    src/core/contents.cpp
    src/core/book.cpp
    src/core/svg.cpp
    src/core/mermaid.cpp
    src/core/cff.cpp
    src/core/tagged.cpp
    src/core/math_parser.cpp
    src/core/math_layout.cpp
    src/common/text_utils.cpp
    src/common/batch_report.cpp
    src/common/file_io.cpp
    src/common/exact_math.cpp
    src/core/rayomd_pdf_source.cpp
    src/core/access_policy.cpp)

if(NOT WIN32)
    if(RAYOMD_USE_CURL)
        find_package(CURL REQUIRED)
    endif()
endif()

# The exporter as a static library for other programs (rayomd-gui adds this directory with
# add_subdirectory). Not built by default; rayomd's own executables compile the sources directly.
add_library(rayomd_engine STATIC EXCLUDE_FROM_ALL ${RAYOMD_CORE_SOURCES})
add_library(rayomd::engine ALIAS rayomd_engine)
target_include_directories(rayomd_engine PUBLIC include src)
target_link_libraries(rayomd_engine PUBLIC Threads::Threads)
if(WIN32)
    target_compile_definitions(rayomd_engine PUBLIC UNICODE _UNICODE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_sources(rayomd_engine PRIVATE src/win32/delay_image_load.cpp src/win32/delay_image_import_pointers.cpp)
        target_link_libraries(rayomd_engine PUBLIC windowscodecs)
    else()
        target_link_libraries(rayomd_engine PUBLIC ole32 winhttp windowscodecs ws2_32)
    endif()
elseif(RAYOMD_USE_CURL)
    target_link_libraries(rayomd_engine PUBLIC CURL::libcurl)
    target_compile_definitions(rayomd_engine PRIVATE RAYOMD_USE_CURL=1)
endif()
if(RAYOMD_LIBM_DLOPEN)
    target_compile_definitions(rayomd_engine PRIVATE RAYOMD_LIBM_DLOPEN=1)
    if(NOT RAYOMD_DLOPEN_IN_LIBC)
        target_link_libraries(rayomd_engine INTERFACE ${CMAKE_DL_LIBS})
    endif()
endif()
rayomd_common_definitions(rayomd_engine)
rayomd_release_flags(rayomd_engine)
rayomd_warnings(rayomd_engine)

if(RAYOMD_BUILD_CLI)
    if(WIN32)
        enable_language(RC)
        add_executable(rayomd ${RAYOMD_CORE_SOURCES} src/win32/main_windows_cli.cpp src/win32/rayomd.rc)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_sources(rayomd PRIVATE
                src/win32/delay_load.cpp
                src/win32/delay_import_pointers.cpp
                src/win32/delay_image_load.cpp
                src/win32/delay_image_import_pointers.cpp)
            target_link_options(rayomd PRIVATE -Wl,--wrap=CommandLineToArgvW)
        endif()
        target_compile_definitions(rayomd PRIVATE UNICODE _UNICODE)
        target_include_directories(rayomd PRIVATE include)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_libraries(rayomd PRIVATE windowscodecs Threads::Threads)
        else()
            target_link_libraries(rayomd PRIVATE
                shell32 ole32 winhttp windowscodecs ws2_32 Threads::Threads)
        endif()
        rayomd_common_definitions(rayomd)
        rayomd_release_flags(rayomd)
        rayomd_warnings(rayomd)
        rayomd_experiments(rayomd)
    else()
        add_executable(rayomd ${RAYOMD_CORE_SOURCES} src/cli/main_cli.cpp)
        if(RAYOMD_STATIC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
            # No .eh_frame registration at startup: the unwinder reads PT_GNU_EH_FRAME (static_frames.cpp).
            target_sources(rayomd PRIVATE src/cli/static_frames.cpp)
            target_link_options(rayomd PRIVATE -Wl,--eh-frame-hdr -Wl,--wrap=__register_frame_info
                -Wl,--wrap=__deregister_frame_info)
        endif()
        target_include_directories(rayomd PRIVATE include)
        target_link_libraries(rayomd PRIVATE Threads::Threads)
        if(RAYOMD_USE_CURL)
            target_link_libraries(rayomd PRIVATE CURL::libcurl)
            target_compile_definitions(rayomd PRIVATE RAYOMD_USE_CURL=1)
        endif()
        rayomd_common_definitions(rayomd)
        rayomd_libm(rayomd)
        rayomd_release_flags(rayomd)
        rayomd_warnings(rayomd)
        rayomd_hot_sections(rayomd)
        rayomd_experiments(rayomd)
    endif()
endif()

if(RAYOMD_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        add_executable(rayomd-core-tests tests/core_tests.cpp ${RAYOMD_CORE_SOURCES})
        target_include_directories(rayomd-core-tests PRIVATE include)
        target_compile_definitions(rayomd-core-tests PRIVATE RAYOMD_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        rayomd_libm(rayomd-core-tests)
        target_link_libraries(rayomd-core-tests PRIVATE rayomd_inflate Threads::Threads)
        if(WIN32)
            target_link_libraries(rayomd-core-tests PRIVATE ole32 winhttp windowscodecs ws2_32)
        endif()
        rayomd_release_flags(rayomd-core-tests)
        rayomd_warnings(rayomd-core-tests)
        rayomd_experiments(rayomd-core-tests)
        add_test(NAME rayomd-core COMMAND rayomd-core-tests)
        if(Python3_Interpreter_FOUND AND TARGET rayomd)
            add_test(
                NAME rayomd-cli
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_cli.py"
                    --binary "$<TARGET_FILE:rayomd>")
        endif()
    endif()
    if(RAYOMD_FUZZ OR (BUILD_TESTING AND NOT WIN32))
        add_subdirectory(tests/fuzz)
    endif()
endif()