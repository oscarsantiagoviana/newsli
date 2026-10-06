# RunCrashTest.cmake — runs t_crash (dies by design through the crash
# handler) and verifies the EFFECT: the minidump the filter must leave
# beside its log. A wrapper because ctest cannot express "a segfault is
# the expected result" honestly (WILL_FAIL only inverts exit codes and
# used to mask real assertion failures — R82l).
execute_process(COMMAND ${PROG} WORKING_DIRECTORY ${LOGDIR}
                RESULT_VARIABLE rv TIMEOUT 90
                OUTPUT_QUIET ERROR_QUIET)
# rv: nonzero / exception — expected and NOT the evidence. The dump is.
file(GLOB dumps ${LOGDIR}/logtest/sli_crash_*.dmp)
list(LENGTH dumps n)
if(n GREATER 0)
  message(STATUS "crash handler: ${n} minidump(s) written -> PASS")
else()
  message(FATAL_ERROR
          "crash handler: process died WITHOUT writing a minidump -> FAIL")
endif()
