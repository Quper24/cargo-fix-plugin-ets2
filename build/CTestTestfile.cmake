# CMake generated Testfile for 
# Source directory: E:/codex/cargo fix plugin ets2
# Build directory: E:/codex/cargo fix plugin ets2/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[generation_limit]=] "E:/codex/cargo fix plugin ets2/build/Debug/generation_limit_check.exe")
  set_tests_properties([=[generation_limit]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/codex/cargo fix plugin ets2/CMakeLists.txt;41;add_test;E:/codex/cargo fix plugin ets2/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[generation_limit]=] "E:/codex/cargo fix plugin ets2/build/Release/generation_limit_check.exe")
  set_tests_properties([=[generation_limit]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/codex/cargo fix plugin ets2/CMakeLists.txt;41;add_test;E:/codex/cargo fix plugin ets2/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[generation_limit]=] "E:/codex/cargo fix plugin ets2/build/MinSizeRel/generation_limit_check.exe")
  set_tests_properties([=[generation_limit]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/codex/cargo fix plugin ets2/CMakeLists.txt;41;add_test;E:/codex/cargo fix plugin ets2/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[generation_limit]=] "E:/codex/cargo fix plugin ets2/build/RelWithDebInfo/generation_limit_check.exe")
  set_tests_properties([=[generation_limit]=] PROPERTIES  _BACKTRACE_TRIPLES "E:/codex/cargo fix plugin ets2/CMakeLists.txt;41;add_test;E:/codex/cargo fix plugin ets2/CMakeLists.txt;0;")
else()
  add_test([=[generation_limit]=] NOT_AVAILABLE)
endif()
