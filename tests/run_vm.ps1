# Builds and runs the virtual machine tests.
# Usage: powershell -ExecutionPolicy Bypass -File tests\run_vm.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$root = Split-Path -Parent $testDir
$exe = Join-Path $root "build\vm_test.exe"

Push-Location $root
try {
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }

    g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o $exe `
        tests\vm_test.cpp `
        src\value.cpp `
        src\lexer.cpp `
        src\parser.cpp `
        src\bytecode.cpp `
        src\compiler.cpp `
        src\vm.cpp
    if ($LASTEXITCODE -ne 0) {
        Write-Host "vm test build failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    & $exe
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host "[OK]   vm_test.cpp" -ForegroundColor Green
    }
    exit $code
} finally {
    if (Test-Path $exe) {
        Remove-Item -Force $exe
    }
    Pop-Location
}
