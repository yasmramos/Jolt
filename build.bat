@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

rem Auto-detect every .cpp under src\ (the development engine). src\native\ is
rem the AOT backend and is not part of this binary; build.sh and CMake use the
rem same rule, so no source is ever listed by hand and the list cannot drift.
set "SOURCES="
for %%f in (src\*.cpp) do set "SOURCES=!SOURCES! %%f"

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\jolt.exe !SOURCES!

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\jolt.exe
endlocal
