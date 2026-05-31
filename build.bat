@echo off
g++ -std=c++11 -O2 -Wall -DNOMINMAX -DWIN32_LEAN_AND_MEAN -municode -o map.exe ^
    map.cpp scanner.cpp stats.cpp output.cpp tui.cpp mft.cpp ^
    -lkernel32 -static
if %errorlevel% equ 0 (
    echo Build successful: map.exe
) else (
    echo Build FAILED
)
