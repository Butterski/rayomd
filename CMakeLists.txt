cmake_minimum_required(VERSION 3.16)
project(fast_markdown LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(fast_markdown_release_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O3>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-s>)
    endif()
endfunction()

if(WIN32)
    add_executable(fast-markdown-imgui WIN32
        src/core/tiny_pdf.cpp
        src/win32/main_win32.cpp
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
        imgui/backends/imgui_impl_win32.cpp
        imgui/backends/imgui_impl_dx11.cpp
    )
    target_compile_definitions(fast-markdown-imgui PRIVATE UNICODE _UNICODE)
    target_include_directories(fast-markdown-imgui PRIVATE include . imgui)
    target_link_libraries(fast-markdown-imgui PRIVATE
        d3d11 dxgi d3dcompiler_47 comdlg32 shell32 ole32 dwmapi
    )
    fast_markdown_release_flags(fast-markdown-imgui)
else()
    add_executable(fast-markdown
        src/core/tiny_pdf.cpp
        src/cli/main_cli.cpp
    )
    target_include_directories(fast-markdown PRIVATE include)
    fast_markdown_release_flags(fast-markdown)
endif()
