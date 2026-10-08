cmake_minimum_required(VERSION 3.16)
project(zerosum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(zerosum-headers INTERFACE)
target_include_directories(zerosum-headers INTERFACE include)
# -ffp-contract=off keeps floating-point results identical on x86 and ARM.
target_compile_options(zerosum-headers INTERFACE -ffp-contract=off -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(zerosum src/main.cpp)
target_link_libraries(zerosum PRIVATE zerosum-headers)
add_executable(core-tests tests/test_core.cpp)
target_link_libraries(core-tests PRIVATE zerosum-headers)

enable_testing()
add_test(NAME core COMMAND core-tests)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_cli.py ${CMAKE_BINARY_DIR})
endif()
