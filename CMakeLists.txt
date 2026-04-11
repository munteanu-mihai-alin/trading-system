
cmake_minimum_required(VERSION 3.18)
project(HFT_FIFO_QUEUE)

set(CMAKE_CXX_STANDARD 20)
include_directories(include)

file(GLOB SRC src/*.cpp)
add_executable(hft ${SRC})

enable_testing()
file(GLOB TEST_SRC tests/unit/*.cpp tests/math/*.cpp tests/sim/*.cpp)
add_executable(tests ${TEST_SRC})
add_test(NAME all_tests COMMAND tests)
