cmake_minimum_required(VERSION 3.16)
project(VideoWallpaper VERSION 1.0.0 LANGUAGES C RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(VW_DEBUG "Grava log de diagnóstico em %TEMP%" OFF)

add_executable(VideoWallpaper WIN32
    src/main.c
    src/gui.c
    src/engine.c
    src/wallpaper.c
    src/config.c
    res/app.rc
)

target_compile_definitions(VideoWallpaper PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
if(VW_DEBUG)
    target_compile_definitions(VideoWallpaper PRIVATE VW_DEBUG)
endif()

if(MSVC)
    # /utf-8: os textos da interface têm acentos
    target_compile_options(VideoWallpaper PRIVATE /utf-8 /W3)
    # O manifesto já vai embutido pelo app.rc
    target_link_options(VideoWallpaper PRIVATE /MANIFEST:NO)
    set_property(TARGET VideoWallpaper PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(VideoWallpaper PRIVATE -municode -Wall -Wextra)
    target_link_options(VideoWallpaper PRIVATE -municode -static)
endif()

target_link_libraries(VideoWallpaper PRIVATE
    mfplat mfuuid d3d11 dxgi dxguid ole32 oleaut32 uuid
    comctl32 comdlg32 shell32 advapi32 gdi32 user32)
