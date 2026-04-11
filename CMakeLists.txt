
cmake_minimum_required(VERSION 3.18)
project(HFT_30_SHADOW)

set(CMAKE_CXX_STANDARD 20)

include_directories(include)

file(GLOB SRC src/*.cpp)
add_executable(hft_shadow ${SRC})
