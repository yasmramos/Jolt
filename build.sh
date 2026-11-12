#!/usr/bin/env sh
# Builds the Oskel interpreter into build/oskel.
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
# The development engine includes the AOT backend (PHASE 7d wires `oskel build`
# into the CLI) and the JIT (PHASE 8), so `src/native/*.cpp` and `src/jit/*.cpp`
# are compiled in too, the way CMake's GLOB_RECURSE over src/ picks them up.
# shellcheck disable=SC2086
"$CXX" -std=c++17 $CXXFLAGS -Wall -Wextra -Iinclude -Isrc -o build/oskel src/*.cpp src/native/*.cpp src/jit/*.cpp
echo "Built build/oskel"
