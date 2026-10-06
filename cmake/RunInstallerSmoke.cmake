# RunInstallerSmoke.cmake — runs the installer's headless smoke gate.
# The --smoke mode itself does install -> uninstall inside one run (the
# exe prints its own PASS); this wrapper checks the exit code and that
# nothing was left behind. The scratch dir is deliberately NOT created
# first — the installer must handle a fresh path itself.
file(REMOVE_RECURSE "${DIR}")
execute_process(COMMAND "${PROG}" --smoke "${DIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "installer smoke FAILED (rc=${rc})\n${out}\n${err}")
endif()
file(GLOB LEFTOVERS "${DIR}/*")
if(LEFTOVERS)
  list(JOIN LEFTOVERS " " L)
  message(FATAL_ERROR "installer smoke left files behind: ${L}")
endif()
message(STATUS "installer smoke: PASS")
