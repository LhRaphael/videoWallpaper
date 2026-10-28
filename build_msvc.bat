@echo off
rem ---------------------------------------------------------------------------
rem  Build com Visual Studio (MSVC).
rem  Abra o "x64 Native Tools Command Prompt for VS" e rode:  build_msvc.bat
rem  Para o build de diagnostico:                          build_msvc.bat debug
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
    echo [erro] cl.exe nao encontrado. Use o "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

set EXTRA=
if /i "%1"=="debug" set EXTRA=/DVW_DEBUG

if not exist build mkdir build

rc /nologo /fo build\app.res res\app.rc || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT %EXTRA% ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_CRT_SECURE_NO_WARNINGS ^
   src\main.c src\gui.c src\engine.c src\wallpaper.c src\config.c build\app.res ^
   /Fo:build\ /Fe:VideoWallpaper.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   mfplat.lib mfuuid.lib d3d11.lib dxgi.lib dxguid.lib ole32.lib oleaut32.lib uuid.lib ^
   comctl32.lib comdlg32.lib shell32.lib advapi32.lib gdi32.lib user32.lib || exit /b 1

echo.
echo Pronto: VideoWallpaper.exe
endlocal
