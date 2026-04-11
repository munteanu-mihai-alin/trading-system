
cmake_minimum_required(VERSION 3.18)
project(HFTReintegrated)

set(CMAKE_CXX_STANDARD 20)

include_directories(include)

file(GLOB LIB_SRC src/lib/*.cpp)
add_library(hft_lib ${LIB_SRC})

file(GLOB APP_SRC src/app/*.cpp)
add_executable(hft_app ${APP_SRC})
target_link_libraries(hft_app hft_lib)

enable_testing()
file(GLOB TEST_SRC tests/unit/*.cpp tests/integration/*.cpp)
add_executable(hft_tests ${TEST_SRC})
target_link_libraries(hft_tests hft_lib)
add_test(NAME all_tests COMMAND hft_tests)
