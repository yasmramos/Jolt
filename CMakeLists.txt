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
# `Platform::lookupSymbol` resolves `extern "C"` declarations with `dlsym`
# (PHASE 6.6). Empty on platforms that fold libdl into libc (glibc 2.34+,
# Windows), so linking it unconditionally is safe and portable.
target_link_libraries(jolt_lib PUBLIC ${CMAKE_DL_LIBS})

# --------------------------------------------------------------------------- libffi (optional)
# FFI callbacks (PHASE 6.6) need a trampoline that C can call back into the VM.
# The development engine uses libffi for that; the AOT backend lowers callbacks
# to compile-time stubs and never links libffi (AGENT.md §5.1/§6.6). The build
# stays green without libffi: `JOLT_HAVE_LIBFFI` is only defined when found, and
# the callback path is compiled out otherwise (a clear runtime error instead).
#
# There is no FindLibFFI module and pkg-config may be absent, so probe the
# library and the header directly. The header lives under the multiarch dir on
# Debian/Ubuntu (/usr/include/<triplet>/ffi.h), so that path is hinted too.
option(JOLT_ENABLE_LIBFFI "Use libffi for FFI callbacks in the dev engine" ON)
if(JOLT_ENABLE_LIBFFI)
  find_library(FFI_LIBRARY NAMES ffi libffi)
  find_path(FFI_INCLUDE_DIR NAMES ffi.h
    PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE}
    PATHS /usr/include /usr/local/include)
  if(FFI_LIBRARY AND FFI_INCLUDE_DIR)
    set(JOLT_LIBFFI_FOUND TRUE)
    target_include_directories(jolt_lib PRIVATE ${FFI_INCLUDE_DIR})
    target_link_libraries(jolt_lib PUBLIC ${FFI_LIBRARY})
    target_compile_definitions(jolt_lib PUBLIC JOLT_HAVE_LIBFFI)
    message(STATUS "libffi found: FFI callbacks enabled in the dev engine")
  else()
    set(JOLT_LIBFFI_FOUND FALSE)
    message(STATUS "libffi not found: FFI callbacks disabled (set JOLT_ENABLE_LIBFFI=OFF to silence)")
  endif()
endif()

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
  jolt_add_cpp_test(value_test)
  jolt_add_cpp_test(repl_test)
  jolt_add_cpp_test(gc_test)
  jolt_add_cpp_test(codegen_test)
  jolt_add_cpp_test(elf_test)
  jolt_add_cpp_test(pe_test)
  jolt_add_cpp_test(lower_test)
  jolt_add_cpp_test(freestanding_test)

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
    # FFI callbacks need libffi in the dev engine; skip the callback test when
    # the build has no libffi (the feature reports a clear runtime error then).
    if(name STREQUAL "32_ffi_callback" AND NOT JOLT_LIBFFI_FOUND)
      continue()
    endif()
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