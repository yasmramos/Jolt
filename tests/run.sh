#!/usr/bin/env sh
# Runs every .oskl test.
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
# Usage: ./tests/run.sh

set -u

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$test_dir")

if [ -x "$root/build/oskel" ]; then
    exe="$root/build/oskel"
elif [ -x "$root/build/oskel.exe" ]; then
    exe="$root/build/oskel.exe"
else
    echo "error: build/oskel was not found. Run ./build.sh first." >&2
    exit 1
fi

# Whether the built engine has a capability, probed from the binary itself.
# `libffi` is linked only when the build found it (CMake); `build.sh` never
# links it, so callbacks are compiled out there (a clear runtime error then).
# An unknown platform or capability reports "present", so a real failure is
# never hidden behind a skip.
has_capability() {
    case "$1" in
        libffi)
            if command -v ldd >/dev/null 2>&1; then
                ldd "$exe" 2>/dev/null | grep -qi 'libffi'
            elif command -v otool >/dev/null 2>&1; then
                otool -L "$exe" 2>/dev/null | grep -qi 'libffi'
            else
                return 0
            fi
            ;;
        *)
            return 0
            ;;
    esac
}

# Prints the capability a test needs (from the leading comment block), or
# nothing. Scans a few lines so it can coexist with `expect-exit:` on line 1.
required_capability() {
    sed -n '1,5s/.*requires:[[:space:]]*\([A-Za-z0-9_-][A-Za-z0-9_-]*\).*/\1/p' "$1" | head -1
}

# Prints the exit code a test declares with a first-line `expect-exit: N`
# comment, or nothing when it declares none.
declared_exit() {
    sed -n '1s/.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$1"
}

# Runs one test and records the result. `mode` is `run` (execute the script) or
# `check` (compile only with `-c`). A check test must additionally leave stdout
# empty, because `-c` must never start the VM (see tests/run_check_case.cmake).
run_one() {
    name=$1
    path=$2
    mode=$3
    expected=$4

    need=$(required_capability "$path")
    if [ -n "$need" ] && ! has_capability "$need"; then
        printf '[SKIP] %s (needs %s)\n' "$name" "$need"
        skipped=$((skipped + 1))
        return
    fi

    total=$((total + 1))

    if [ "$mode" = "check" ]; then
        stdout=$("$exe" -c "$path" 2>/dev/null)
        actual=$?
        if [ "$actual" -eq "$expected" ] && [ -z "$stdout" ]; then
            printf '[OK]   %s\n' "$name"
            return
        fi
        if [ "$actual" -ne "$expected" ]; then
            printf '[FAIL] %s (expected exit %s, got %s)\n' "$name" "$expected" "$actual"
        else
            printf '[FAIL] %s (-c produced output, so the program ran)\n' "$name"
        fi
        failed=$((failed + 1))
        return
    fi

    output=$("$exe" "$path" 2>&1)
    actual=$?
    if [ "$actual" -eq "$expected" ]; then
        printf '[OK]   %s\n' "$name"
    else
        printf '[FAIL] %s (expected exit %s, got %s)\n' "$name" "$expected" "$actual"
        printf '%s\n' "$output" | sed 's/^/       /'
        failed=$((failed + 1))
    fi
}

failed=0
total=0
skipped=0

# tests/*.oskl and examples/*.oskl: run the script, expected exit 0 unless the
# first line declares otherwise.
for test in "$test_dir"/*.oskl; do
    [ -e "$test" ] || continue
    expected=$(declared_exit "$test")
    [ -n "$expected" ] || expected=0
    run_one "$(basename "$test")" "$test" run "$expected"
done

for test in "$root/examples"/*.oskl; do
    [ -e "$test" ] || continue
    expected=$(declared_exit "$test")
    [ -n "$expected" ] || expected=0
    run_one "examples/$(basename "$test")" "$test" run "$expected"
done

# tests/errors/*.oskl: run the script, expected exit 70 unless declared.
for test in "$test_dir/errors"/*.oskl; do
    [ -e "$test" ] || continue
    expected=$(declared_exit "$test")
    [ -n "$expected" ] || expected=70
    run_one "errors/$(basename "$test")" "$test" run "$expected"
done

# tests/check/*.oskl: compile only with `-c`, expected exit 0 unless declared.
for test in "$test_dir/check"/*.oskl; do
    [ -e "$test" ] || continue
    expected=$(declared_exit "$test")
    [ -n "$expected" ] || expected=0
    run_one "check/$(basename "$test")" "$test" check "$expected"
done

if [ "$total" -eq 0 ]; then
    echo "no tests found in $test_dir" >&2
    exit 1
fi

echo
if [ "$failed" -eq 0 ]; then
    if [ "$skipped" -gt 0 ]; then
        printf 'all %s tests passed (%s skipped)\n' "$total" "$skipped"
    else
        printf 'all %s tests passed\n' "$total"
    fi
    exit 0
fi

if [ "$skipped" -gt 0 ]; then
    printf '%s of %s tests failed (%s skipped)\n' "$failed" "$total" "$skipped"
else
    printf '%s of %s tests failed\n' "$failed" "$total"
fi
exit 1
