# CMake generated Testfile for 
# Source directory: /home/nickchel/webdev/c_mcp_server
# Build directory: /home/nickchel/webdev/c_mcp_server/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[basic]=] "/home/nickchel/webdev/c_mcp_server/build/test_basic")
set_tests_properties([=[basic]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/nickchel/webdev/c_mcp_server/CMakeLists.txt;83;add_test;/home/nickchel/webdev/c_mcp_server/CMakeLists.txt;0;")
add_test([=[http]=] "/home/nickchel/webdev/c_mcp_server/build/test_http")
set_tests_properties([=[http]=] PROPERTIES  TIMEOUT "30" _BACKTRACE_TRIPLES "/home/nickchel/webdev/c_mcp_server/CMakeLists.txt;88;add_test;/home/nickchel/webdev/c_mcp_server/CMakeLists.txt;0;")
