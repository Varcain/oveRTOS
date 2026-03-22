# OveTest.cmake - Helper for adding oveRTOS test suites
#
# Usage:
#   ove_add_test_suite(<target> <suite_source> ...)
#   Adds the given test suite source files to the target.

function(ove_add_test_suite TARGET)
    target_sources(${TARGET} PRIVATE ${ARGN})
endfunction()
