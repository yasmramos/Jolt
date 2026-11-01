# Runs one Jolt script under CTest and checks its exit code.
# Registered by CMakeLists.txt; expects:
#   -DJOLT_EXE=<path to the jolt executable>
#   -DJOLT_SCRIPT=<path to the .jolt file>
#   -DEXPECTED_EXIT=<exit code declared by the script, 0 by default>

if(NOT DEFINED JOLT_EXE OR NOT DEFINED JOLT_SCRIPT OR NOT DEFINED EXPECTED_EXIT)
  message(FATAL_ERROR "run_jolt_case.cmake requires JOLT_EXE, JOLT_SCRIPT and EXPECTED_EXIT")
endif()

execute_process(
  COMMAND "${JOLT_EXE}" "${JOLT_SCRIPT}"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE script_stdout
  ERROR_VARIABLE script_stderr
)

if(NOT actual_exit EQUAL EXPECTED_EXIT)
  message(FATAL_ERROR
    "${JOLT_SCRIPT}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}\n"
    "--- stdout ---\n${script_stdout}\n"
    "--- stderr ---\n${script_stderr}")
endif()
