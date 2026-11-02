# Feeds a session to the REPL under CTest and checks that it behaves.
# Registered by CMakeLists.txt; expects:
#   -DJOLT_EXE=<path to the jolt executable>
#   -DREPL_INPUT=<path to the file piped to the REPL>

if(NOT DEFINED JOLT_EXE OR NOT DEFINED REPL_INPUT)
  message(FATAL_ERROR "run_repl_case.cmake requires JOLT_EXE and REPL_INPUT")
endif()

execute_process(
  COMMAND "${JOLT_EXE}"
  INPUT_FILE "${REPL_INPUT}"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE repl_stdout
  ERROR_VARIABLE repl_stderr
)

if(NOT actual_exit EQUAL 0)
  message(FATAL_ERROR "REPL exited with ${actual_exit}\n${repl_stdout}\n${repl_stderr}")
endif()

# `:clear` forgets user definitions but must keep the standard library, so the
# `print` that follows still works and `y` is defined afterwards.
foreach(expected "all definitions cleared" "2\n" "y: int")
  string(FIND "${repl_stdout}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "REPL output is missing '${expected}'\n--- stdout ---\n${repl_stdout}\n--- stderr ---\n${repl_stderr}")
  endif()
endforeach()

string(FIND "${repl_stdout}${repl_stderr}" "undefined variable" unexpected)
if(NOT unexpected EQUAL -1)
  message(FATAL_ERROR "REPL reported an undefined variable after :clear\n${repl_stdout}\n${repl_stderr}")
endif()
