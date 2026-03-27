# CMake generated Testfile for 
# Source directory: /home/varcain/projects/private/hIRoic/oveRTOS/tests
# Build directory: /home/varcain/projects/private/hIRoic/oveRTOS/build-test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(ove_stub_tests "/home/varcain/projects/private/hIRoic/oveRTOS/build-test/ove_test_stub")
set_tests_properties(ove_stub_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/varcain/projects/private/hIRoic/oveRTOS/tests/CMakeLists.txt;123;add_test;/home/varcain/projects/private/hIRoic/oveRTOS/tests/CMakeLists.txt;0;")
subdirs("_deps/cmocka-build")
