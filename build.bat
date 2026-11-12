@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

rem Auto-detect every .cpp under src\ and src\native\ (the development engine
rem plus the AOT backend, which PHASE 7d wires into `oskel build`). build.sh and
rem CMake use the same rule, so no source is ever listed by hand and the list
rem cannot drift.
set "SOURCES="
for %%f in (src\*.cpp src\native\*.cpp src\jit\*.cpp) do set "SOURCES=!SOURCES! %%f"

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\oskel.exe !SOURCES!

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\oskel.exe
endlocal
