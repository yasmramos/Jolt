#!/usr/bin/env sh
# Runs every .jolt test.
# Positive tests live in tests/ and must exit with code 0.
# Error tests live in tests/errors/ and declare their expected exit code with
# a first-line comment such as: // expect-exit: 70
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

if [ -x "$root/build/jolt" ]; then
    exe="$root/build/jolt"
elif [ -x "$root/build/jolt.exe" ]; then
    exe="$root/build/jolt.exe"
else
    echo "error: build/jolt was not found. Run ./build.sh first." >&2
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

failed=0
total=0
skipped=0

for test in "$test_dir"/*.jolt; do
    [ -e "$test" ] || continue
    name=$(basename "$test")

    need=$(required_capability "$test")
    if [ -n "$need" ] && ! has_capability "$need"; then
        printf '[SKIP] %s (needs %s)\n' "$name" "$need"
        skipped=$((skipped + 1))
        continue
    fi

    total=$((total + 1))
    if output=$("$exe" "$test" 2>&1); then
        printf '[OK]   %s\n' "$name"
    else
        status=$?
        printf '[FAIL] %s (exit %s)\n' "$name" "$status"
        printf '%s\n' "$output" | sed 's/^/       /'
        failed=$((failed + 1))
    fi
done

error_dir="$test_dir/errors"
for test in "$error_dir"/*.jolt; do
    [ -e "$test" ] || continue
    name="errors/$(basename "$test")"

    need=$(required_capability "$test")
    if [ -n "$need" ] && ! has_capability "$need"; then
        printf '[SKIP] %s (needs %s)\n' "$name" "$need"
        skipped=$((skipped + 1))
        continue
    fi

    total=$((total + 1))
    expected=$(sed -n '1s/.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$test")
    [ -n "$expected" ] || expected=70

    output=$("$exe" "$test" 2>&1)
    actual=$?
    if [ "$actual" -eq "$expected" ]; then
        printf '[OK]   %s\n' "$name"
    else
        printf '[FAIL] %s (expected exit %s, got %s)\n' "$name" "$expected" "$actual"
        printf '%s\n' "$output" | sed 's/^/       /'
        failed=$((failed + 1))
    fi
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
