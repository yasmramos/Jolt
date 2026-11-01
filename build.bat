@echo off
setlocal
cd /d "%~dp0"

if not exist build mkdir build

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\jolt.exe ^
    src\value.cpp ^
    src\lexer.cpp ^
    src\parser.cpp ^
    src\bytecode.cpp ^
    src\compiler.cpp ^
    src\vm.cpp ^
    src\ops.cpp ^
    src\interpreter.cpp ^
    src\builtins.cpp ^
    src\diagnostic.cpp ^
    src\main.cpp

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\jolt.exe
endlocal
