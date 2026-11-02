# Fails when the bytecode compiler or the native backend pulls in the
# interpreter or the VM.
#
# AGENT.md requires this boundary: `src/compiler.*` and `src/native/*` are
# libraries any entry point may link (today the single `jolt` binary, later a
# separate `joltc`), and the AOT runtime must never be able to reach the
# compiler or the interpreter. Keeping the include graph clean is what makes
# that split a second `main` instead of a refactor.
#
# Usage: cmake -DROOT=<repository root> -P tests/check_compiler_boundary.cmake

if(NOT DEFINED ROOT)
  get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

file(GLOB boundary_files
  "${ROOT}/src/compiler.cpp"
  "${ROOT}/src/compiler.h"
  "${ROOT}/include/compiler.h"
  "${ROOT}/src/native/*.cpp"
  "${ROOT}/src/native/*.h"
  "${ROOT}/include/native/*.h"
)

# `[.]` keeps the regex readable: no escaping of the dot inside CMake strings.
set(forbidden "interpreter[.]h|vm[.]h")

set(violations "")
foreach(file IN LISTS boundary_files)
  file(STRINGS "${file}" source_lines)
  foreach(line IN LISTS source_lines)
    # A commented-out include documents the rule, it does not violate it.
    string(STRIP "${line}" candidate)
    if(candidate MATCHES "^//" OR candidate MATCHES "^/[*]" OR candidate MATCHES "^[*]")
      continue()
    endif()
    if(line MATCHES "#[ \t]*include[ \t]*[<\"](${forbidden})[>\"]")
      file(RELATIVE_PATH shown "${ROOT}" "${file}")
      string(STRIP "${line}" shown_line)
      list(APPEND violations "${shown}: ${shown_line}")
    endif()
  endforeach()
endforeach()

if(violations)
  list(LENGTH violations count)
  foreach(violation IN LISTS violations)
    message(SEND_ERROR "forbidden include: ${violation}")
  endforeach()
  message(FATAL_ERROR
    "${count} forbidden include(s): the compiler and the native backend must not "
    "include interpreter.h or vm.h (see AGENT.md, toolchain boundary).")
endif()

list(LENGTH boundary_files checked)
message(STATUS "compiler boundary intact (${checked} file(s) checked)")
