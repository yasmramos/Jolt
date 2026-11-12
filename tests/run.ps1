# Runs every .jolt test in this directory.
# Positive tests live in tests/ and examples/ and must exit with code 0.
# Error tests live in tests/errors/ and declare their expected exit code with
# a first-line comment such as: // expect-exit: 70
# Check tests live in tests/check/ and run under `-c` (compile only): the
# declared exit code must match and nothing may reach stdout.
#
# A test may declare a capability it needs on its first line, such as:
#   // requires: libffi
# The runner skips the test (instead of failing it) when the built engine does
# not have that capability, mirroring the CMake gating in CMakeLists.txt so a
# tree that is green under CTest is also green here.
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

# Whether the built engine has a capability, probed from the binary itself.
# `libffi` is linked only when the build found it (CMake); an unknown capability
# reports "present", so a real failure is never hidden behind a skip.
function Test-Capability([string]$capability) {
    if ($capability -ne "libffi") {
        return $true
    }
    # Dumpbin is not guaranteed to be on PATH; when the probe is unavailable the
    # capability reports "present" rather than skipping a real test.
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($null -eq $dumpbin) {
        return $true
    }
    $imports = & $dumpbin /imports $exe 2>$null | Out-String
    return $imports -match "(?i)libffi"
}

# Prints the capability a test needs (from the leading comment block), or
# nothing. Scans a few lines so it can coexist with `expect-exit:` on line 1.
function Get-RequiredCapability([string]$path) {
    $lines = Get-Content -Path $path -TotalCount 5 -ErrorAction SilentlyContinue
    foreach ($line in $lines) {
        if ($line -match "requires:\s*([A-Za-z0-9_-]+)") {
            return $Matches[1]
        }
    }
    return $null
}

# Prints the exit code a test declares with a first-line `expect-exit: N`
# comment, or nothing when it declares none.
function Get-DeclaredExit([string]$path) {
    $firstLine = Get-Content -Path $path -TotalCount 1
    if ($firstLine -match "expect-exit:\s*(\d+)") {
        return [int]$Matches[1]
    }
    return $null
}

$failed = 0
$total = 0
$skipped = 0

# Runs one test and records the result. `mode` is `run` (execute the script) or
# `check` (compile only with `-c`). A check test must additionally leave stdout
# empty, because `-c` must never start the VM (see tests/run_check_case.cmake).
function Invoke-One {
    param(
        [string]$name,
        [string]$path,
        [string]$mode,
        [int]$expected
    )

    $need = Get-RequiredCapability $path
    if ($need -and -not (Test-Capability $need)) {
        Write-Host ("[SKIP] " + $name + " (needs " + $need + ")") -ForegroundColor Yellow
        $script:skipped += 1
        return
    }

    $script:total += 1

    if ($mode -eq "check") {
        $stdout = & $exe -c $path 2>$null | Out-String
        $actual = $LASTEXITCODE
        if (($actual -eq $expected) -and [string]::IsNullOrEmpty($stdout)) {
            Write-Host ("[OK]   " + $name) -ForegroundColor Green
            return
        }
        if ($actual -ne $expected) {
            Write-Host ("[FAIL] " + $name + " (expected exit " + $expected + ", got " + $actual + ")") -ForegroundColor Red
        } else {
            Write-Host ("[FAIL] " + $name + " (-c produced output, so the program ran)") -ForegroundColor Red
        }
        $script:failed += 1
        return
    }

    $output = & $exe $path 2>&1
    $actual = $LASTEXITCODE
    if ($actual -eq $expected) {
        Write-Host ("[OK]   " + $name) -ForegroundColor Green
    } else {
        Write-Host ("[FAIL] " + $name + " (expected exit " + $expected + ", got " + $actual + ")") -ForegroundColor Red
        ($output | Out-String).TrimEnd() -split "`r?`n" | ForEach-Object { Write-Host ("       " + $_) }
        $script:failed += 1
    }
}

# tests/*.jolt: run the script, expected exit 0 unless declared otherwise.
$tests = @(Get-ChildItem -Path $testDir -Filter "*.jolt" | Sort-Object Name)
foreach ($test in $tests) {
    $expected = Get-DeclaredExit $test.FullName
    if ($null -eq $expected) { $expected = 0 }
    Invoke-One $test.Name $test.FullName "run" $expected
}

# examples/*.jolt: run the script, expected exit 0 unless declared otherwise.
$examplesDir = Join-Path $root "examples"
if (Test-Path $examplesDir) {
    $examples = @(Get-ChildItem -Path $examplesDir -Filter "*.jolt" | Sort-Object Name)
    foreach ($test in $examples) {
        $expected = Get-DeclaredExit $test.FullName
        if ($null -eq $expected) { $expected = 0 }
        Invoke-One ("examples/" + $test.Name) $test.FullName "run" $expected
    }
}

# tests/errors/*.jolt: run the script, expected exit 70 unless declared.
$errorDir = Join-Path $testDir "errors"
if (Test-Path $errorDir) {
    $errorTests = @(Get-ChildItem -Path $errorDir -Filter "*.jolt" | Sort-Object Name)
    foreach ($test in $errorTests) {
        $expected = Get-DeclaredExit $test.FullName
        if ($null -eq $expected) { $expected = 70 }
        Invoke-One ("errors/" + $test.Name) $test.FullName "run" $expected
    }
}

# tests/check/*.jolt: compile only with `-c`, expected exit 0 unless declared.
$checkDir = Join-Path $testDir "check"
if (Test-Path $checkDir) {
    $checkTests = @(Get-ChildItem -Path $checkDir -Filter "*.jolt" | Sort-Object Name)
    foreach ($test in $checkTests) {
        $expected = Get-DeclaredExit $test.FullName
        if ($null -eq $expected) { $expected = 0 }
        Invoke-One ("check/" + $test.Name) $test.FullName "check" $expected
    }
}

if ($total -eq 0) {
    Write-Host "no tests found in $testDir"
    exit 1
}

Write-Host ""
if ($failed -eq 0) {
    if ($skipped -gt 0) {
        Write-Host ("all " + $total + " tests passed (" + $skipped + " skipped)") -ForegroundColor Green
    } else {
        Write-Host ("all " + $total + " tests passed") -ForegroundColor Green
    }
    exit 0
}

if ($skipped -gt 0) {
    Write-Host ($failed.ToString() + " of " + $total + " tests failed (" + $skipped + " skipped)") -ForegroundColor Red
} else {
    Write-Host ($failed.ToString() + " of " + $total + " tests failed") -ForegroundColor Red
}
exit 1
