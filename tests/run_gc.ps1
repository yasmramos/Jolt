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

    # Link the whole runtime like CMake's `jolt_lib`: every src/**/*.cpp except
    # the `jolt` entry point. The list is detected, never written by hand, so it
    # cannot drift when a source is added or removed.
    $srcRoot = (Resolve-Path "src").Path
    $sources = Get-ChildItem -Path "src" -Recurse -Filter "*.cpp" |
        Where-Object { $_.Name -ne "main.cpp" } |
        ForEach-Object { "src\" + $_.FullName.Substring($srcRoot.Length + 1) }

    g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o $exe `
        tests\gc_test.cpp $sources
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
