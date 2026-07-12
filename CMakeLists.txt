cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOMD_VERSION LIMIT_COUNT 1)
project(rayomd VERSION ${RAYOMD_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RAYOMD_USE_SIMDUTF "Use vendored simdutf for UTF conversion" OFF)
option(RAYOMD_USE_CURL "Use libcurl for HTTP/HTTPS image URLs on non-Windows" OFF)

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

if(WIN32)
    enable_language(RC)
    add_executable(rayomd WIN32
        src/core/tiny_pdf.cpp
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
        d3d11 dxgi d3dcompiler_47 comdlg32 shell32 ole32 dwmapi winhttp windowscodecs ws2_32
    )
    rayomd_simdutf(rayomd)
    rayomd_common_definitions(rayomd)
    rayomd_release_flags(rayomd)
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
        src/cli/main_cli.cpp
    )
    target_include_directories(rayomd PRIVATE include)
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
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME rayomd-cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_cli.py"
                --binary "$<TARGET_FILE:rayomd>")
    endif()
endif()