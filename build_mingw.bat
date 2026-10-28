@echo off
rem ---------------------------------------------------------------------------
rem  Build com MinGW-w64 (MSYS2 UCRT64, w64devkit, WinLibs...).
rem  Precisa de gcc.exe e windres.exe no PATH.
rem    build_mingw.bat          -> versao normal
rem    build_mingw.bat debug    -> grava log em %%TEMP%%\videowallpaper_debug.log
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [erro] gcc.exe nao encontrado no PATH.
    exit /b 1
)

set EXTRA=
if /i "%1"=="debug" set EXTRA=-DVW_DEBUG

if not exist build mkdir build

windres -O coff -i res\app.rc -o build\app.res.o || exit /b 1

gcc -std=c11 -O2 -Wall -Wextra -municode %EXTRA% ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    src\main.c src\gui.c src\engine.c src\wallpaper.c src\config.c build\app.res.o ^
    -o VideoWallpaper.exe -municode -mwindows -s -static ^
    -lmfplat -lmfuuid -ld3d11 -ldxgi -ldxguid -lole32 -loleaut32 -luuid ^
    -lcomctl32 -lcomdlg32 -lshell32 -ladvapi32 -lgdi32 -luser32 || exit /b 1

echo.
echo Pronto: VideoWallpaper.exe
endlocal
