# Verifies that `cmake --install` ships the standard library next to the
# binary and that the installed `oskel` can resolve `import std.math` from an
# unrelated working directory. Guards the CMakeLists install(DIRECTORY std)
# rule and the executable-path lookup in standardLibraryDir().
#
# Registered by CMakeLists.txt; expects:
#   -DOSKEL_SOURCE_DIR=<repository root>
#   -DOSKEL_BUILD_DIR=<cmake build directory>
#   -DOSKEL_SCRIPT=<a .oskl file that imports std.math and exits 0>

if(NOT DEFINED OSKEL_SOURCE_DIR OR NOT DEFINED OSKEL_BUILD_DIR OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "install_stdlib.cmake requires OSKEL_SOURCE_DIR, OSKEL_BUILD_DIR and OSKEL_SCRIPT")
endif()

set(prefix "${CMAKE_CURRENT_BINARY_DIR}/install-prefix")

# Start from a clean prefix: `cmake --install` does not remove files that a
# previous install left behind, so a stale `std/` would mask the regression.
file(REMOVE_RECURSE "${prefix}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${OSKEL_BUILD_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE install_exit
  OUTPUT_VARIABLE install_out
  ERROR_VARIABLE install_err
)
if(NOT install_exit EQUAL 0)
  message(FATAL_ERROR "install failed (${install_exit})\n${install_out}\n${install_err}")
endif()

if(NOT EXISTS "${prefix}/share/oskel/std/math.oskl")
  message(FATAL_ERROR
    "the installed tree is missing share/oskel/std/math.oskl; the standard "
    "library is not being installed")
endif()

# Run the installed binary from an unrelated directory, so the working
# directory cannot supply `std/`: only the shipped copy can.
execute_process(
  COMMAND "${prefix}/bin/oskel" "${OSKEL_SCRIPT}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_out
  ERROR_VARIABLE run_err
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the installed oskel could not import std.math (exit ${run_exit})\n"
    "--- stdout ---\n${run_out}\n--- stderr ---\n${run_err}")
endif()
