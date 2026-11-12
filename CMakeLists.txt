cmake_minimum_required(VERSION 3.15)

project(oskel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  set(OSKEL_WARNINGS /W4)
  # Match the 8 MiB main-thread stack Linux and macOS give by default. The
  # recursive-descent parser and the compiler's AST walk each hold a DepthGuard
  # (see src/parser.cpp and src/compiler.cpp) sized for that budget; Windows'
  # 1 MiB default would let a deeply nested source overflow the stack before
  # the guard fires, turning a clean compile error into a crash.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")
else()
  set(OSKEL_WARNINGS -Wall -Wextra)
endif()

# Testing aid: run the cycle collector at every safe point instead of only when
# enough objects were allocated. Slow; meant for sanitizer runs.
option(OSKEL_GC_STRESS "Collect cycles at every safe point (testing only)" OFF)
if(OSKEL_GC_STRESS)
  add_compile_definitions(OSKEL_GC_STRESS)
endif()

# --------------------------------------------------------------------------- sources
# Auto-detect every .cpp under src/ and src/native/. No file is listed by hand,
# so adding a new source never requires touching this file.
file(GLOB_RECURSE OSKEL_ALL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# The main entry point is not part of the runtime library.
set(OSKEL_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
list(REMOVE_ITEM OSKEL_ALL_SOURCES "${OSKEL_MAIN}")

# --------------------------------------------------------------------------- library
# One static library with the whole runtime. Every executable links against it,
# so no test repeats the source list.
add_library(oskel_lib STATIC ${OSKEL_ALL_SOURCES})
target_include_directories(oskel_lib PUBLIC src include)
target_compile_options(oskel_lib PRIVATE ${OSKEL_WARNINGS})
# `Platform::lookupSymbol` resolves `extern "C"` declarations with `dlsym`
# (PHASE 6.6). Empty on platforms that fold libdl into libc (glibc 2.34+,
# Windows), so linking it unconditionally is safe and portable.
target_link_libraries(oskel_lib PUBLIC ${CMAKE_DL_LIBS})

# --------------------------------------------------------------------------- libffi (optional)
# FFI callbacks (PHASE 6.6) need a trampoline that C can call back into the VM.
# The development engine uses libffi for that; the AOT backend lowers callbacks
# to compile-time stubs and never links libffi (AGENT.md §5.1/§6.6). The build
# stays green without libffi: `OSKEL_HAVE_LIBFFI` is only defined when found, and
# the callback path is compiled out otherwise (a clear runtime error instead).
#
# There is no FindLibFFI module and pkg-config may be absent, so probe the
# library and the header directly. The header lives under the multiarch dir on
# Debian/Ubuntu (/usr/include/<triplet>/ffi.h), so that path is hinted too.
option(OSKEL_ENABLE_LIBFFI "Use libffi for FFI callbacks in the dev engine" ON)
if(OSKEL_ENABLE_LIBFFI)
  find_library(FFI_LIBRARY NAMES ffi libffi)
  find_path(FFI_INCLUDE_DIR NAMES ffi.h
    PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE}
    PATHS /usr/include /usr/local/include)
  if(FFI_LIBRARY AND FFI_INCLUDE_DIR)
    set(OSKEL_LIBFFI_FOUND TRUE)
    target_include_directories(oskel_lib PRIVATE ${FFI_INCLUDE_DIR})
    target_link_libraries(oskel_lib PUBLIC ${FFI_LIBRARY})
    target_compile_definitions(oskel_lib PUBLIC OSKEL_HAVE_LIBFFI)
    message(STATUS "libffi found: FFI callbacks enabled in the dev engine")
  else()
    set(OSKEL_LIBFFI_FOUND FALSE)
    message(STATUS "libffi not found: FFI callbacks disabled (set OSKEL_ENABLE_LIBFFI=OFF to silence)")
  endif()
endif()

# Struct-by-value FFI is written for the System V AMD64 ABI only; the Microsoft
# x64 ABI passes a struct larger than 8 bytes through a hidden pointer, a shape
# the register thunks cannot express. The compiler rejects such a declaration
# with a clear error on other targets, and the test that exercises it is skipped.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND NOT WIN32)
  set(OSKEL_FFI_STRUCT_BY_VALUE TRUE)
else()
  set(OSKEL_FFI_STRUCT_BY_VALUE FALSE)
endif()

# --------------------------------------------------------------------------- main binary
add_executable(oskel "${OSKEL_MAIN}")
target_link_libraries(oskel PRIVATE oskel_lib)
target_compile_options(oskel PRIVATE ${OSKEL_WARNINGS})

install(TARGETS oskel RUNTIME DESTINATION bin)

# The standard library ships next to the binary: `standardLibraryDir` looks for
# `../share/oskel/std` relative to the executable, so `import std.math` resolves
# after `make install` and not only when running from the source tree.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/std" DESTINATION share/oskel
  FILES_MATCHING PATTERN "*.oskl")

# --------------------------------------------------------------------------- tests
include(CTest)

if(BUILD_TESTING)

  # Registers one script in CTest; the expected exit code comes from the
  # first-line `// expect-exit: N` comment, defaulting to 0.
  function(oskel_add_script_test name script)
    set(expected 0)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    string(REGEX MATCH "expect-exit:[ \t]*([0-9]+)" declared_exit "${first_line}")
    if(declared_exit)
      set(expected "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME "${name}"
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${script}"
        "-DEXPECTED_EXIT=${expected}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_oskel_case.cmake")
  endfunction()

  # Registers one C++ test. The test file is `tests/<name>.cpp`; it links
  # against `oskel_lib` so it does not repeat the source list.
  # Run from the repository root so a test can read fixtures by their path
  # relative to the root (e.g. `tests/modules/util.oskl`).
  function(oskel_add_cpp_test name)
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")
    target_link_libraries(${name} PRIVATE oskel_lib)
    target_compile_options(${name} PRIVATE ${OSKEL_WARNINGS})
    add_test(NAME ${name} COMMAND ${name}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endfunction()

  # --- C++ tests (one line each) ---
  oskel_add_cpp_test(compiler_test)
  oskel_add_cpp_test(module_test)
  oskel_add_cpp_test(vm_test)
  oskel_add_cpp_test(jit_test)
  oskel_add_cpp_test(value_test)
  oskel_add_cpp_test(repl_test)
  oskel_add_cpp_test(gc_test)
  oskel_add_cpp_test(codegen_test)
  oskel_add_cpp_test(dtoa_test)
  oskel_add_cpp_test(elf_test)
  oskel_add_cpp_test(pe_test)
  oskel_add_cpp_test(lower_test)
  oskel_add_cpp_test(freestanding_test)

  # --- REPL scripted test (clear) ---
  add_test(NAME repl.clear
    COMMAND ${CMAKE_COMMAND}
      "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
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
      "-DOSKEL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOSKEL_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/21_std_math.oskl"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_stdlib.cmake")

  # --- `oskel build`: compile to a native binary and run it (PHASE 7d) ---
  # The AOT backend targets x86_64-linux-sysv, so this runs on Linux x86-64
  # only; elsewhere the generated image would not execute.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME oskel.build_binary
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_build_hello.oskl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_binary.cmake")
  endif()

  # --- .oskl script tests (auto-detected) ---
  file(GLOB oskel_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.oskl"
  )
  foreach(script IN LISTS oskel_tests)
    get_filename_component(name "${script}" NAME_WE)
    # FFI callbacks need libffi in the dev engine; skip the callback test when
    # the build has no libffi (the feature reports a clear runtime error then).
    if(name STREQUAL "32_ffi_callback" AND NOT OSKEL_LIBFFI_FOUND)
      continue()
    endif()
    # Struct-by-value FFI needs the System V AMD64 ABI; see the capability above.
    if(name STREQUAL "31_ffi_struct" AND NOT OSKEL_FFI_STRUCT_BY_VALUE)
      continue()
    endif()
    oskel_add_script_test("oskel.${name}" "${script}")
  endforeach()

  # --- examples/*.oskl (auto-detected) ---
  file(GLOB oskel_examples CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.oskl"
  )
  foreach(script IN LISTS oskel_examples)
    get_filename_component(name "${script}" NAME_WE)
    oskel_add_script_test("examples.${name}" "${script}")
  endforeach()

  # --- tests/errors/*.oskl (auto-detected) ---
  file(GLOB oskel_error_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/*.oskl"
  )
  foreach(script IN LISTS oskel_error_tests)
    get_filename_component(name "${script}" NAME_WE)
    oskel_add_script_test("oskel.errors.${name}" "${script}")
  endforeach()

  # --- tests/check/*.oskl: run under `-c` (compile only, never execute) ---
  # `-c` must run the whole compiler (types and imports included) and print
  # nothing; the runner checks the exit code and that stdout is empty.
  file(GLOB oskel_check_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check/*.oskl"
  )
  foreach(script IN LISTS oskel_check_tests)
    get_filename_component(name "${script}" NAME_WE)
    set(expected 0)
    file(STRINGS "${script}" first_line LIMIT_COUNT 1)
    string(REGEX MATCH "expect-exit:[ \t]*([0-9]+)" declared_exit "${first_line}")
    if(declared_exit)
      set(expected "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME "oskel.check.${name}"
      COMMAND ${CMAKE_COMMAND}
        "-DOSKEL_EXE=$<TARGET_FILE:oskel>"
        "-DOSKEL_SCRIPT=${script}"
        "-DEXPECTED_EXIT=${expected}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_check_case.cmake")
  endforeach()

endif()