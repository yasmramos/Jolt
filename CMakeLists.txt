cmake_minimum_required(VERSION 3.15)

project(jolt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  set(JOLT_WARNINGS /W4)
else()
  set(JOLT_WARNINGS -Wall -Wextra)
endif()

# Testing aid: run the cycle collector at every safe point instead of only when
# enough objects were allocated. Slow; meant for sanitizer runs.
option(JOLT_GC_STRESS "Collect cycles at every safe point (testing only)" OFF)
if(JOLT_GC_STRESS)
  add_compile_definitions(JOLT_GC_STRESS)
endif()

# --------------------------------------------------------------------------- sources
# Auto-detect every .cpp under src/ and src/native/. No file is listed by hand,
# so adding a new source never requires touching this file.
file(GLOB_RECURSE JOLT_ALL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# The main entry point is not part of the runtime library.
set(JOLT_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
list(REMOVE_ITEM JOLT_ALL_SOURCES "${JOLT_MAIN}")

# --------------------------------------------------------------------------- library
# One static library with the whole runtime. Every executable links against it,
# so no test repeats the source list.
add_library(jolt_lib STATIC ${JOLT_ALL_SOURCES})
target_include_directories(jolt_lib PUBLIC src include)
target_compile_options(jolt_lib PRIVATE ${JOLT_WARNINGS})

# --------------------------------------------------------------------------- main binary
add_executable(jolt "${JOLT_MAIN}")
target_link_libraries(jolt PRIVATE jolt_lib)
target_compile_options(jolt PRIVATE ${JOLT_WARNINGS})

install(TARGETS jolt RUNTIME DESTINATION bin)

# The standard library ships next to the binary: `standardLibraryDir` looks for
# `../share/jolt/std` relative to the executable, so `import std.math` resolves
# after `make install` and not only when running from the source tree.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/std" DESTINATION share/jolt
  FILES_MATCHING PATTERN "*.jolt")

# --------------------------------------------------------------------------- tests
include(CTest)

if(BUILD_TESTING)

  # Registers one script in CTest; the expected exit code comes from the
  # first-line `// expect-exit: N` comment, defaulting to 0.
  function(jolt_add_script_test name script)
    set(expected 0)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    string(REGEX MATCH "expect-exit:[ \t]*([0-9]+)" declared_exit "${first_line}")
    if(declared_exit)
      set(expected "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME "${name}"
      COMMAND ${CMAKE_COMMAND}
        "-DJOLT_EXE=$<TARGET_FILE:jolt>"
        "-DJOLT_SCRIPT=${script}"
        "-DEXPECTED_EXIT=${expected}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_jolt_case.cmake")
  endfunction()

  # Registers one C++ test. The test file is `tests/<name>.cpp`; it links
  # against `jolt_lib` so it does not repeat the source list.
  # Run from the repository root so a test can read fixtures by their path
  # relative to the root (e.g. `tests/modules/util.jolt`).
  function(jolt_add_cpp_test name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")
    target_link_libraries(${name} PRIVATE jolt_lib)
    target_compile_options(${name} PRIVATE ${JOLT_WARNINGS})
    add_test(NAME ${name} COMMAND ${name}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endfunction()

  # --- C++ tests (one line each) ---
  jolt_add_cpp_test(compiler_test)
  jolt_add_cpp_test(module_test)
  jolt_add_cpp_test(vm_test)
  jolt_add_cpp_test(repl_test)
  jolt_add_cpp_test(gc_test)
  jolt_add_cpp_test(codegen_test)
  jolt_add_cpp_test(elf_test)
  jolt_add_cpp_test(pe_test)
  jolt_add_cpp_test(lower_test)

  # --- REPL scripted test (clear) ---
  add_test(NAME repl.clear
    COMMAND ${CMAKE_COMMAND}
      "-DJOLT_EXE=$<TARGET_FILE:jolt>"
      "-DREPL_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/repl/clear.in"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_repl_case.cmake")

  # --- Compiler boundary check (AGENT.md toolchain boundary) ---
  add_test(NAME compiler_boundary
    COMMAND ${CMAKE_COMMAND}
      -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_compiler_boundary.cmake")

  # --- Installed layout: `std/` ships with the binary and resolves ---
  add_test(NAME install_stdlib
    COMMAND ${CMAKE_COMMAND}
      "-DJOLT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DJOLT_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DJOLT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/21_std_math.jolt"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_stdlib.cmake")

  # --- .jolt script tests (auto-detected) ---
  file(GLOB jolt_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.jolt"
  )
  foreach(script IN LISTS jolt_tests)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("jolt.${name}" "${script}")
  endforeach()

  # --- examples/*.jolt (auto-detected) ---
  file(GLOB jolt_examples CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.jolt"
  )
  foreach(script IN LISTS jolt_examples)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("examples.${name}" "${script}")
  endforeach()

  # --- tests/errors/*.jolt (auto-detected) ---
  file(GLOB jolt_error_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/*.jolt"
  )
  foreach(script IN LISTS jolt_error_tests)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("jolt.errors.${name}" "${script}")
  endforeach()

endif()