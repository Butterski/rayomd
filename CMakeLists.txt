cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOMD_VERSION LIMIT_COUNT 1)
project(rayomd VERSION ${RAYOMD_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(RAYOMD_USE_SIMDUTF "Use vendored simdutf for UTF conversion" OFF)
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
set(RAYOMD_PGO "OFF" CACHE STRING "PGO mode: OFF, GENERATE, or USE")
set_property(CACHE RAYOMD_PGO PROPERTY STRINGS OFF GENERATE USE)
set(RAYOMD_PGO_DIR "" CACHE PATH "Shared profile-data directory for PGO GENERATE/USE builds")

set(RAYOMD_SIMDUTF_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdutf/singleheader/simdutf.cpp")
set(RAYOMD_SIMDUTF_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdutf/singleheader")

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
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-s>
                $<$<CONFIG:Release>:-static-libgcc>
                $<$<CONFIG:Release>:-static-libstdc++>
                $<$<CONFIG:Release>:-Wl,--gc-sections>)
            if(RAYOMD_STATIC)
                target_link_options(${target} PRIVATE -static)
            endif()
        endif()
    endif()
endfunction()

function(rayomd_simdutf target)
    if(RAYOMD_USE_SIMDUTF AND EXISTS "${RAYOMD_SIMDUTF_SOURCE}")
        target_sources(${target} PRIVATE "${RAYOMD_SIMDUTF_SOURCE}")
        target_include_directories(${target} PRIVATE "${RAYOMD_SIMDUTF_INCLUDE}")
        target_compile_definitions(${target} PRIVATE RAYOMD_USE_SIMDUTF=1)
    endif()
endfunction()

function(rayomd_common_definitions target)
    target_compile_definitions(${target} PRIVATE RAYOMD_VERSION="${PROJECT_VERSION}")
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

    if(NOT RAYOMD_PGO STREQUAL "OFF")
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "RAYOMD_PGO currently requires GCC or Clang")
        endif()
        if(RAYOMD_PGO_DIR)
            set(rayomd_profile_dir "${RAYOMD_PGO_DIR}")
        else()
            set(rayomd_profile_dir "${CMAKE_BINARY_DIR}/pgo-data")
        endif()
        if(RAYOMD_PGO STREQUAL "GENERATE")
            target_compile_options(${target} PRIVATE "-fprofile-generate=${rayomd_profile_dir}")
            target_link_options(${target} PRIVATE "-fprofile-generate=${rayomd_profile_dir}")
        elseif(RAYOMD_PGO STREQUAL "USE")
            target_compile_options(${target} PRIVATE "-fprofile-use=${rayomd_profile_dir}" -fprofile-correction)
            target_link_options(${target} PRIVATE "-fprofile-use=${rayomd_profile_dir}" -fprofile-correction)
        else()
            message(FATAL_ERROR "RAYOMD_PGO must be OFF, GENERATE, or USE")
        endif()
    endif()
endfunction()

# Recovery is cold, opt-in code, the math parser and layout run only for documents with
# formulas, options and themes are parsed once per run, a theme's header, footer and cover,
# PDF/A's metadata and the footnotes' numbering are made once per document, and the image
# encoder's threads only hand out segments that flate.cpp compresses. Optimize these
# translation units for package size while leaving the performance-critical renderer and
# parser on the normal release flags.
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
        src/core/tagged.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release>:-Os>")
    # The DEFLATE encoder of --compress and re-encoded images: -O2 is within 1 % of -O3 on
    # RayoMD's streams at half the code; the inflater, 1.5 % slower than -O3 at 8.9 KB of code
    # against 17.7. The highlighter's lexers read every byte of a code block: 1.5 times as fast at
    # -O2 as at -Os.
    set_source_files_properties(src/core/flate.cpp src/core/inflate.cpp src/core/highlight.cpp
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

if(WIN32)
    enable_language(RC)
    add_executable(rayomd WIN32
        src/core/tiny_pdf.cpp
        src/core/theme.cpp
        src/core/pdfa.cpp
        src/core/png_filter.cpp
        src/core/flate.cpp
        src/core/image_cache.cpp
        src/core/image_encoder.cpp
        src/core/inflate.cpp
        src/core/rayomd_pdf_source.cpp
        src/core/export_options.cpp
        src/core/inline_markdown.cpp
        src/core/markdown_parser.cpp
        src/core/footnotes.cpp
        src/core/highlight.cpp
        src/core/contents.cpp
        src/core/book.cpp
        src/core/svg.cpp
        src/core/tagged.cpp
        src/core/math_parser.cpp
        src/core/math_layout.cpp
        src/common/text_utils.cpp
        src/common/batch_report.cpp
        src/common/file_io.cpp
        src/win32/main_win32.cpp
        src/win32/rayomd.rc
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/misc/cpp/imgui_stdlib.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_sources(rayomd PRIVATE
            src/win32/delay_load.cpp
            src/win32/delay_import_pointers.cpp
            src/win32/delay_image_load.cpp
            src/win32/delay_image_import_pointers.cpp)
        target_link_options(rayomd PRIVATE
            -Wl,--wrap=GetOpenFileNameW
            -Wl,--wrap=GetSaveFileNameW
            -Wl,--wrap=D3D11CreateDeviceAndSwapChain
            -Wl,--wrap=D3DCompile
            -Wl,--wrap=DwmEnableBlurBehindWindow
            -Wl,--wrap=DwmGetColorizationColor
            -Wl,--wrap=DwmIsCompositionEnabled
            -Wl,--wrap=DwmSetWindowAttribute
            -Wl,--wrap=DragAcceptFiles
            -Wl,--wrap=DragFinish
            -Wl,--wrap=DragQueryFileW
            -Wl,--wrap=SHGetFolderPathW
            -Wl,--wrap=ShellExecuteW
            -Wl,--wrap=CommandLineToArgvW)
    endif()
    target_compile_definitions(rayomd PRIVATE UNICODE _UNICODE)
    target_include_directories(rayomd PRIVATE include . third_party third_party/imgui)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_libraries(rayomd PRIVATE
            dxgi windowscodecs Threads::Threads)
    else()
        target_link_libraries(rayomd PRIVATE
            d3d11 dxgi d3dcompiler_47 comdlg32 shell32 ole32 dwmapi
            winhttp windowscodecs ws2_32 Threads::Threads)
    endif()
    rayomd_simdutf(rayomd)
    rayomd_common_definitions(rayomd)
    rayomd_release_flags(rayomd)
    rayomd_experiments(rayomd)
else()
    if(RAYOMD_USE_CURL)
        find_package(CURL REQUIRED)
    endif()
    add_executable(rayomd
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
        src/core/contents.cpp
        src/core/book.cpp
        src/core/svg.cpp
        src/core/tagged.cpp
        src/core/math_parser.cpp
        src/core/math_layout.cpp
        src/common/text_utils.cpp
        src/common/batch_report.cpp
        src/common/file_io.cpp
        src/core/rayomd_pdf_source.cpp
        src/cli/main_cli.cpp
    )
    target_include_directories(rayomd PRIVATE include)
    target_link_libraries(rayomd PRIVATE Threads::Threads)
    if(RAYOMD_USE_CURL)
        target_link_libraries(rayomd PRIVATE CURL::libcurl)
        target_compile_definitions(rayomd PRIVATE RAYOMD_USE_CURL=1)
    endif()
    rayomd_simdutf(rayomd)
    rayomd_common_definitions(rayomd)
    rayomd_release_flags(rayomd)
    rayomd_experiments(rayomd)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_executable(rayomd-core-tests
        tests/core_tests.cpp
        src/core/tiny_pdf.cpp
        src/core/theme.cpp
        src/core/pdfa.cpp
        src/core/png_filter.cpp
        src/core/flate.cpp
        src/core/image_cache.cpp
        src/core/image_encoder.cpp
        src/core/inflate.cpp
        src/core/rayomd_pdf_source.cpp
        src/core/export_options.cpp
        src/core/inline_markdown.cpp
        src/core/markdown_parser.cpp
        src/core/footnotes.cpp
        src/core/highlight.cpp
        src/core/contents.cpp
        src/core/book.cpp
        src/core/svg.cpp
        src/core/tagged.cpp
        src/core/math_parser.cpp
        src/core/math_layout.cpp
        src/common/text_utils.cpp
        src/common/batch_report.cpp
        src/common/file_io.cpp)
    target_include_directories(rayomd-core-tests PRIVATE include)
    target_compile_definitions(rayomd-core-tests PRIVATE RAYOMD_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rayomd-core-tests PRIVATE rayomd_inflate Threads::Threads)
    if(WIN32)
        target_link_libraries(rayomd-core-tests PRIVATE ole32 winhttp windowscodecs ws2_32)
    endif()
    rayomd_release_flags(rayomd-core-tests)
    rayomd_experiments(rayomd-core-tests)
    add_test(NAME rayomd-core COMMAND rayomd-core-tests)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME rayomd-cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_cli.py"
                --binary "$<TARGET_FILE:rayomd>")
    endif()
endif()