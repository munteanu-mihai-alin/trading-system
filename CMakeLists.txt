
cmake_minimum_required(VERSION 3.18)
project(HFTValidated)

set(CMAKE_CXX_STANDARD 20)
include_directories(include)

file(GLOB SRC src/*.cpp)
add_executable(hft ${SRC})

enable_testing()
file(GLOB TEST_SRC tests/math/*.cpp tests/sim/*.cpp)
add_executable(tests ${TEST_SRC})
add_test(NAME all_tests COMMAND tests)
