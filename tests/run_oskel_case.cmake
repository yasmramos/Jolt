# Runs one Jolt script under CTest and checks its exit code.
# Registered by CMakeLists.txt; expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<path to the .oskl file>
#   -DEXPECTED_EXIT=<exit code declared by the script, 0 by default>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED EXPECTED_EXIT)
  message(FATAL_ERROR "run_oskel_case.cmake requires OSKEL_EXE, OSKEL_SCRIPT and EXPECTED_EXIT")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE script_stdout
  ERROR_VARIABLE script_stderr
)

if(NOT actual_exit EQUAL EXPECTED_EXIT)
  message(FATAL_ERROR
    "${OSKEL_SCRIPT}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}\n"
    "--- stdout ---\n${script_stdout}\n"
    "--- stderr ---\n${script_stderr}")
endif()
