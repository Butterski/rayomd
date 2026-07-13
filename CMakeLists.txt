cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOMD_VERSION LIMIT_COUNT 1)
project(rayomd VERSION ${RAYOMD_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(RAYOMD_USE_SIMDUTF "Use vendored simdutf for UTF conversion" OFF)
option(RAYOMD_USE_CURL "Use libcurl for HTTP/HTTPS image URLs on non-Windows" OFF)
option(RAYOMD_ENABLE_LTO "Enable interprocedural optimization for measured release experiments" OFF)
option(RAYOMD_ENABLE_PROFILING "Enable phase timing and allocation counters" OFF)
option(RAYOMD_ENABLE_TSAN "Enable ThreadSanitizer on supported non-Windows builds" OFF)
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
        else()
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O3>)
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-s>)
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

# Recovery is cold, opt-in code. Optimize its translation unit for package size
# while leaving the performance-critical renderer on the normal release flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/core/rayomd_pdf_source.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release>:-Os>")
endif()

if(WIN32)
    enable_language(RC)
    add_executable(rayomd WIN32
        src/core/tiny_pdf.cpp
        src/core/rayomd_pdf_source.cpp
        src/core/export_options.cpp
        src/core/inline_markdown.cpp
        src/core/markdown_parser.cpp
        src/common/text_utils.cpp
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
    target_compile_definitions(rayomd PRIVATE UNICODE _UNICODE)
    target_include_directories(rayomd PRIVATE include . third_party third_party/imgui)
    target_link_libraries(rayomd PRIVATE
        d3d11 dxgi d3dcompiler_47 comdlg32 shell32 ole32 dwmapi winhttp windowscodecs ws2_32 Threads::Threads
    )
    rayomd_simdutf(rayomd)
    rayomd_common_definitions(rayomd)
    rayomd_release_flags(rayomd)
    rayomd_experiments(rayomd)
else()
    if(RAYOMD_USE_CURL)
        find_package(CURL REQUIRED)
    endif()
    find_package(ZLIB QUIET)

    add_executable(rayomd
        src/core/tiny_pdf.cpp
        src/core/export_options.cpp
        src/core/inline_markdown.cpp
        src/core/markdown_parser.cpp
        src/common/text_utils.cpp
        src/core/rayomd_pdf_source.cpp
        src/cli/main_cli.cpp
    )
    target_include_directories(rayomd PRIVATE include)
    target_link_libraries(rayomd PRIVATE Threads::Threads)
    if(RAYOMD_USE_CURL)
        target_link_libraries(rayomd PRIVATE CURL::libcurl)
        target_compile_definitions(rayomd PRIVATE RAYOMD_USE_CURL=1)
    endif()
    if(ZLIB_FOUND)
        target_link_libraries(rayomd PRIVATE ZLIB::ZLIB)
        target_compile_definitions(rayomd PRIVATE RAYOMD_USE_ZLIB=1)
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
        src/core/rayomd_pdf_source.cpp
        src/core/export_options.cpp
        src/core/inline_markdown.cpp
        src/core/markdown_parser.cpp
        src/common/text_utils.cpp)
    target_include_directories(rayomd-core-tests PRIVATE include)
    target_compile_definitions(rayomd-core-tests PRIVATE RAYOMD_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rayomd-core-tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(rayomd-core-tests PRIVATE ole32 winhttp windowscodecs ws2_32)
    elseif(ZLIB_FOUND)
        target_link_libraries(rayomd-core-tests PRIVATE ZLIB::ZLIB)
        target_compile_definitions(rayomd-core-tests PRIVATE RAYOMD_USE_ZLIB=1)
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