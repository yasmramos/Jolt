# End-to-end test for `jolt build` (PHASE 7d): compile a .jolt program to a
# native binary and run it.
#
# Registered by CMakeLists.txt on Linux x86-64 only (the AOT backend targets
# x86_64-linux-sysv). Expects:
#   -DJOLT_EXE=<path to the jolt executable>
#   -DJOLT_SCRIPT=<the .jolt file to build>
#
# It asserts:
#   1. `jolt build` exits 0 and writes the requested output,
#   2. the generated binary runs and prints exactly the VM's output,
#   3. the binary is statically linked (no dynamic dependencies).

if(NOT DEFINED JOLT_EXE OR NOT DEFINED JOLT_SCRIPT)
  message(FATAL_ERROR "build_binary.cmake requires JOLT_EXE and JOLT_SCRIPT")
endif()

set(out "${CMAKE_CURRENT_BINARY_DIR}/jolt_build_output")
file(REMOVE "${out}")

execute_process(
  COMMAND "${JOLT_EXE}" build "${JOLT_SCRIPT}" -o "${out}"
  RESULT_VARIABLE build_exit
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_exit EQUAL 0)
  message(FATAL_ERROR
    "jolt build exited ${build_exit}\n--- stdout ---\n${build_stdout}\n"
    "--- stderr ---\n${build_stderr}")
endif()
if(NOT EXISTS "${out}")
  message(FATAL_ERROR "jolt build did not create '${out}'")
endif()

# The reference output is the development VM's: the AOT binary must agree.
execute_process(
  COMMAND "${JOLT_EXE}" "${JOLT_SCRIPT}"
  RESULT_VARIABLE vm_exit
  OUTPUT_VARIABLE vm_stdout
)
if(NOT vm_exit EQUAL 0)
  message(FATAL_ERROR "the VM failed to run the fixture (exit ${vm_exit})")
endif()

execute_process(
  COMMAND "${out}"
  RESULT_VARIABLE run_exit
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_exit EQUAL 0)
  message(FATAL_ERROR
    "the generated binary exited ${run_exit}\n--- stdout ---\n${run_stdout}\n"
    "--- stderr ---\n${run_stderr}")
endif()
if(NOT run_stdout STREQUAL vm_stdout)
  message(FATAL_ERROR
    "the generated binary disagrees with the VM\n"
    "--- vm ---\n${vm_stdout}\n--- binary ---\n${run_stdout}")
endif()

# A freestanding binary must not depend on a shared runtime.
execute_process(
  COMMAND ldd "${out}"
  RESULT_VARIABLE ldd_exit
  OUTPUT_VARIABLE ldd_out
  ERROR_VARIABLE ldd_err
)
set(ldd_text "${ldd_out}${ldd_err}")
if(NOT ldd_text MATCHES "not a dynamic executable|statically linked")
  message(FATAL_ERROR "the generated binary is dynamically linked:\n${ldd_text}")
endif()
