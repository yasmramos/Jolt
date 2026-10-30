#!/usr/bin/env sh
# Compila y ejecuta las pruebas de la maquina virtual.
# Uso: ./tests/run_vm.sh

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

"$CXX" -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc \
    -o build/vm_test \
    tests/vm_test.cpp \
    src/value.cpp \
    src/lexer.cpp \
    src/parser.cpp \
    src/bytecode.cpp \
    src/compiler.cpp \
    src/vm.cpp

./build/vm_test
