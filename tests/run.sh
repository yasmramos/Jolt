#!/usr/bin/env sh
# Runs every .jolt test.
# Positive tests live in tests/ and must exit with code 0.
# Error tests live in tests/errors/ and declare their expected exit code with
# a first-line comment such as: // expect-exit: 70
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

failed=0
total=0

for test in "$test_dir"/*.jolt; do
    [ -e "$test" ] || continue
    total=$((total + 1))
    name=$(basename "$test")

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
    total=$((total + 1))
    name="errors/$(basename "$test")"
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
    printf 'all %s tests passed\n' "$total"
    exit 0
fi

printf '%s of %s tests failed\n' "$failed" "$total"
exit 1
