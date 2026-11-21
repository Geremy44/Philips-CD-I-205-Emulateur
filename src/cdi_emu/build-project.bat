@echo off
echo.
echo ========================================
echo CD-i 205 Emulator Build & Test Script
echo ========================================
echo.

REM Check if we have the required ROM files
if not exist "ROM\CDI205-00_PS-7211_REL.2.1.hex" (
    echo ERROR: Required ROM file not found!
    echo Please ensure ROM\CDI205-00_PS-7211_REL.2.1.hex exists
    exit /b 1
)

echo Building CD-i 205 Emulator with GCC...

REM Create output directory if it doesn't exist
if not exist bin mkdir bin

echo Compiling source files...
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/bus.c -o bin/bus.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/rom_loader.c -o bin/rom_loader.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/rtc_m48t08.c -o bin/rtc_m48t08.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/os9trace.c -o bin/os9trace.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CDIC/cdic.c -o bin/cdic.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CDIC/disc.c -o bin/disc.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CDIC/adpcm.c -o bin/adpcm.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/audio_out.c -o bin/audio_out.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/m68k.c -o bin/m68k.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/m68k_ea.c -o bin/m68k_ea.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/m68k_ops.c -o bin/m68k_ops.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/onchip68070.c -o bin/onchip68070.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/uart.c -o bin/uart.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/CPU/uart_console.c -o bin/uart_console.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/GPU/scc66470.c -o bin/scc66470.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/GPU/video_display.c -o bin/video_display.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/GPU/vsd.c -o bin/vsd.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/SLAVE/slave.c -o bin/slave.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/SLAVE/slave_hle.c -o bin/slave_hle.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    core/SLAVE/slave_lle.c -o bin/slave_lle.o
gcc -c -O2 -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC -std=c99 ^
    main.c -o bin/main.o

if %errorlevel% neq 0 (
    echo Error compiling source files
    exit /b 1
)

echo Linking executable...
gcc -o bin/cdi_emu.exe bin/*.o -lSDL2main -lSDL2 -lSDL2_ttf -lm

if %errorlevel% neq 0 (
    echo Error linking executable
    exit /b 1
)

echo.
echo Build completed successfully!
echo Executable is located at: bin/cdi_emu.exe
echo.

echo Testing the emulator...
echo Starting emulator for 5 seconds...
timeout 5
echo Test completed.

echo.
echo To run the emulator manually:
echo   cd bin
echo   cdi_emu.exe