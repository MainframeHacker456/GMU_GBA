@echo off
echo ========================================================
echo   Building gdkGBA Educational Emulator (gdkgba.exe)
echo ========================================================

if exist gdkgba del /f /q gdkgba

gcc -O3 -Wall -Wextra -I./c_src c_src/main.c c_src/arm.c c_src/arm_mem.c c_src/bios.c c_src/dma.c c_src/io.c c_src/sound.c c_src/timer.c c_src/video.c -lmingw32 -lSDL2 -lm -o gdkgba.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo  BUILD SUCCESSFUL: gdkgba.exe is ready!
    echo ========================================================
    echo.
    echo Usage:
    echo   gdkgba.exe "Pokemon.gba"
    echo   gdkgba.exe "Pokemon.gba" "gba_bios.bin"
    echo.
) else (
    echo.
    echo ========================================================
    echo  BUILD FAILED!
    echo ========================================================
    echo Make sure GCC and SDL2 are installed.
)
