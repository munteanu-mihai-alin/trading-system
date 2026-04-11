cmake_minimum_required(VERSION 3.18)
project(HFTExecutionEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_compile_options(-O3 -march=native -pthread -DNDEBUG)

include_directories(include)

file(GLOB SRC_FILES src/*.cpp)

add_executable(hft_engine ${SRC_FILES})
