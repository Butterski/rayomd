@echo off
echo Building Fast Markdown (ImGui + DirectX11)...

set IMGUI=imgui

g++ -O3 -s -std=c++17 -DUNICODE -D_UNICODE ^
    main.cpp ^
    %IMGUI%/imgui.cpp ^
    %IMGUI%/imgui_draw.cpp ^
    %IMGUI%/imgui_tables.cpp ^
    %IMGUI%/imgui_widgets.cpp ^
    %IMGUI%/misc/cpp/imgui_stdlib.cpp ^
    %IMGUI%/backends/imgui_impl_win32.cpp ^
    %IMGUI%/backends/imgui_impl_dx11.cpp ^
    -o fast-markdown-imgui.exe ^
    -mwindows ^
    -ld3d11 -ldxgi -ld3dcompiler_47 -lcomdlg32 -lshell32 -lole32 -ldwmapi ^
    -I. -I%IMGUI% ^
    2>&1

if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    for %%A in (fast-markdown-imgui.exe) do echo Size: %%~zA bytes
) else (
    echo Build failed!
)

pause
