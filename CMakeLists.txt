
cmake_minimum_required(VERSION 3.18)
project(HFTExecutionEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_COVERAGE "Enable coverage" ON)

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
endif()

include_directories(include)

file(GLOB SRC_FILES src/*.cpp)
add_executable(hft_engine ${SRC_FILES})

# Tests
enable_testing()

file(GLOB TEST_FILES tests/unit/*.cpp tests/math/*.cpp tests/sim/*.cpp)
add_executable(tests_runner ${TEST_FILES})

target_link_libraries(tests_runner pthread)

add_test(NAME AllTests COMMAND tests_runner)
