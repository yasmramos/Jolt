#!/usr/bin/env sh
# Builds the Jolt interpreter into build/jolt.
# Usage: ./build.sh
#   CXX=clang++ ./build.sh
#   CXXFLAGS="-O0 -g" ./build.sh

set -eu

cd "$(dirname "$0")"

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

CXXFLAGS=${CXXFLAGS:--O2}

mkdir -p build
# shellcheck disable=SC2086
"$CXX" -std=c++17 $CXXFLAGS -Wall -Wextra -Iinclude -Isrc -o build/jolt src/*.cpp
echo "Built build/jolt"
