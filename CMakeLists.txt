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

set(JOLT_CORE_SOURCES
  src/value.cpp
  src/lexer.cpp
  src/parser.cpp
  src/bytecode.cpp
  src/diagnostic.cpp
)

add_executable(jolt
  ${JOLT_CORE_SOURCES}
  src/compiler.cpp
  src/vm.cpp
  src/interpreter.cpp
  src/builtins.cpp
  src/main.cpp
)

target_include_directories(jolt PRIVATE src include)
target_compile_options(jolt PRIVATE ${JOLT_WARNINGS})

install(TARGETS jolt RUNTIME DESTINATION bin)

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

  add_executable(compiler_test
    ${JOLT_CORE_SOURCES}
    src/compiler.cpp
    tests/compiler_test.cpp
  )
  target_include_directories(compiler_test PRIVATE src include)
  target_compile_options(compiler_test PRIVATE ${JOLT_WARNINGS})
  add_test(NAME compiler_test COMMAND compiler_test)

  add_executable(vm_test
    ${JOLT_CORE_SOURCES}
    src/compiler.cpp
    src/vm.cpp
    tests/vm_test.cpp
  )
  target_include_directories(vm_test PRIVATE src include)
  target_compile_options(vm_test PRIVATE ${JOLT_WARNINGS})
  add_test(NAME vm_test COMMAND vm_test)

  file(GLOB jolt_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.jolt"
  )
  foreach(script IN LISTS jolt_tests)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("jolt.${name}" "${script}")
  endforeach()

  file(GLOB jolt_examples CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.jolt"
  )
  foreach(script IN LISTS jolt_examples)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("examples.${name}" "${script}")
  endforeach()

  file(GLOB jolt_error_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/errors/*.jolt"
  )
  foreach(script IN LISTS jolt_error_tests)
    get_filename_component(name "${script}" NAME_WE)
    jolt_add_script_test("jolt.errors.${name}" "${script}")
  endforeach()
endif()
