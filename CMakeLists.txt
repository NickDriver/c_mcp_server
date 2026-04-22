cmake_minimum_required(VERSION 3.25)
project(c_mcp_server
    VERSION 0.1.0
    DESCRIPTION "MCP server SDK in pure C23"
    LANGUAGES C)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "This project is developed against clang; got ${CMAKE_C_COMPILER_ID}. "
        "Configure with: cmake -DCMAKE_C_COMPILER=clang ..")
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/IntelliSense picks up the real
# include paths and flags — otherwise IDEs flag cJSON.h as missing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Keep asserts enabled even in Release — user explicitly wants fail-fast.
string(REGEX REPLACE "[-/]DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
string(REGEX REPLACE "[-/]DNDEBUG" "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
string(REGEX REPLACE "[-/]DNDEBUG" "" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")

set(MCP_WARNINGS
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes
    -Wimplicit-fallthrough -Wformat=2
    -Wnull-dereference -Wdouble-promotion
    -Werror)

set(MCP_HARDENING
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2)

# ----- vendored cJSON -----
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)
# cJSON is third-party; don't apply our strict warnings.
target_compile_options(cjson PRIVATE -w)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ----- mcp library -----
add_library(mcp STATIC
    src/mcp_server.c
    src/mcp_jsonrpc.c
    src/mcp_stdio.c
    src/mcp_http.c
    src/mcp_log.c)
target_include_directories(mcp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(mcp PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})

find_package(Threads REQUIRED)
target_link_libraries(mcp PUBLIC cjson Threads::Threads)
set_target_properties(mcp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ----- examples -----
add_executable(echo_server examples/echo_server/main.c)
target_compile_options(echo_server PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})
target_link_libraries(echo_server PRIVATE mcp)

add_executable(demo_server examples/demo_server/main.c)
target_compile_options(demo_server PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})
target_link_libraries(demo_server PRIVATE mcp)

add_executable(http_demo examples/http_demo/main.c)
target_compile_options(http_demo PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})
target_link_libraries(http_demo PRIVATE mcp)

# ----- tests -----
enable_testing()
add_executable(test_basic tests/test_basic.c)
target_compile_options(test_basic PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})
target_link_libraries(test_basic PRIVATE mcp)
add_test(NAME basic COMMAND test_basic)

add_executable(test_http tests/test_http.c)
target_compile_options(test_http PRIVATE ${MCP_WARNINGS} ${MCP_HARDENING})
target_link_libraries(test_http PRIVATE mcp)
add_test(NAME http COMMAND test_http)
set_tests_properties(http PROPERTIES TIMEOUT 30)
