# Builds and runs the AST -> bytecode compiler tests.
# Usage: powershell -ExecutionPolicy Bypass -File tests\run_compiler.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$root = Split-Path -Parent $testDir
$exe = Join-Path $root "build\compiler_test.exe"

Push-Location $root
try {
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }

    g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o $exe `
        tests\compiler_test.cpp `
        src\value.cpp `
        src\lexer.cpp `
        src\parser.cpp `
        src\bytecode.cpp `
        src\compiler.cpp
    if ($LASTEXITCODE -ne 0) {
        Write-Host "compiler test build failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    & $exe
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host "[OK]   compiler_test.cpp" -ForegroundColor Green
    }
    exit $code
} finally {
    if (Test-Path $exe) {
        Remove-Item -Force $exe
    }
    Pop-Location
}
