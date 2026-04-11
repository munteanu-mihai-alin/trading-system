cmake_minimum_required(VERSION 3.18)
project(HFTMergedRecovered LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
  add_compile_options(/O2)
else()
  add_compile_options(-O3 -DNDEBUG)
endif()

include_directories(include tests)

set(LIB_SOURCES
    src/lib/AppConfig.cpp
    src/lib/RankingEngine.cpp
)

add_library(hft_lib ${LIB_SOURCES})

add_executable(hft_app
    src/app/main.cpp
)
target_link_libraries(hft_app PRIVATE hft_lib)

add_executable(hft_tests
    tests/common/TestMain.cpp
    tests/unit/TestCoreModels.cpp
    tests/math/TestMathModels.cpp
    tests/sim/TestSimulator.cpp
    tests/validation/TestValidation.cpp
)
target_link_libraries(hft_tests PRIVATE hft_lib)

enable_testing()
add_test(NAME hft_tests COMMAND hft_tests)
