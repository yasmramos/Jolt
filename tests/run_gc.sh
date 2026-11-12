#!/usr/bin/env sh
# Builds and runs the cycle collector tests.
# Usage: ./tests/run_gc.sh

set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$test_dir")

cd "$root"
mkdir -p build

if [ -z "${CXX:-}" ]; then
    if command -v g++ >/dev/null 2>&1; then
        CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    else
        echo "error: no C++ compiler found; set CXX or install g++/clang++" >&2
        exit 1
    fi
fi

# Link the whole runtime like CMake's `oskel_lib`: every src/**/*.cpp except the
# `oskel` entry point. The list is detected, never written by hand, so it cannot
# drift when a source is added or removed.
sources=""
for src in src/*.cpp src/native/*.cpp; do
    if [ "$src" != "src/main.cpp" ]; then
        sources="$sources $src"
    fi
done

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc \
    -o build/gc_test \
    tests/gc_test.cpp \
    $sources

./build/gc_test
