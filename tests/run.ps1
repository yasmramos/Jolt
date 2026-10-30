# Runs every .jolt test in this directory.
# Positive tests live in tests/ and must exit with code 0.
# Error tests live in tests/errors/ and declare their expected exit code with
# a first-line comment such as: // expect-exit: 70
#
# Usage: powershell -ExecutionPolicy Bypass -File tests\run.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$root = Split-Path -Parent $testDir
$exe = Join-Path $root "build\jolt.exe"

if (-not (Test-Path $exe)) {
    Write-Host "error: $exe was not found. Run build.bat first." -ForegroundColor Red
    exit 1
}

$tests = @(Get-ChildItem -Path $testDir -Filter "*.jolt" | Sort-Object Name)
$errorDir = Join-Path $testDir "errors"
$errorTests = @()
if (Test-Path $errorDir) {
    $errorTests = @(Get-ChildItem -Path $errorDir -Filter "*.jolt" | Sort-Object Name)
}

if (($tests.Count + $errorTests.Count) -eq 0) {
    Write-Host "no tests found in $testDir"
    exit 1
}

$failed = 0
$total = 0

foreach ($test in $tests) {
    $total += 1
    $output = & $exe $test.FullName 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host ("[OK]   " + $test.Name) -ForegroundColor Green
    } else {
        Write-Host ("[FAIL] " + $test.Name + " (exit " + $LASTEXITCODE + ")") -ForegroundColor Red
        ($output | Out-String).TrimEnd() -split "`r?`n" | ForEach-Object { Write-Host ("       " + $_) }
        $failed += 1
    }
}

foreach ($test in $errorTests) {
    $total += 1
    $firstLine = Get-Content -Path $test.FullName -TotalCount 1
    $expected = 70
    if ($firstLine -match "expect-exit:\s*(\d+)") {
        $expected = [int]$Matches[1]
    }

    $output = & $exe $test.FullName 2>&1
    $actual = $LASTEXITCODE
    if ($actual -eq $expected) {
        Write-Host ("[OK]   errors/" + $test.Name) -ForegroundColor Green
    } else {
        Write-Host ("[FAIL] errors/" + $test.Name + " (expected exit " + $expected + ", got " + $actual + ")") -ForegroundColor Red
        ($output | Out-String).TrimEnd() -split "`r?`n" | ForEach-Object { Write-Host ("       " + $_) }
        $failed += 1
    }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host ("all " + $total + " tests passed") -ForegroundColor Green
    exit 0
}

Write-Host ($failed.ToString() + " of " + $total + " tests failed") -ForegroundColor Red
exit 1
