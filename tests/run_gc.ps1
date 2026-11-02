# Builds and runs the cycle collector tests.
# Usage: powershell -ExecutionPolicy Bypass -File tests\run_gc.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$root = Split-Path -Parent $testDir
$exe = Join-Path $root "build\gc_test.exe"

Push-Location $root
try {
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }

    g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o $exe `
        tests\gc_test.cpp `
        src\gc.cpp `
        src\env.cpp `
        src\value.cpp
    if ($LASTEXITCODE -ne 0) {
        Write-Host "gc test build failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    & $exe
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host "[OK]   gc_test.cpp" -ForegroundColor Green
    }
    exit $code
} finally {
    if (Test-Path $exe) {
        Remove-Item -Force $exe
    }
    Pop-Location
}
