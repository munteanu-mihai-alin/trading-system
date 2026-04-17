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

option(HFT_ENABLE_IBKR "Enable real IBKR integration using vendored TWS API" OFF)

set(HFT_INCLUDE_DIRS
    include
    tests
)

set(LIB_SOURCES
    src/lib/AppConfig.cpp
    src/lib/RankingEngine.cpp
    src/lib/IBKRClient.cpp
    src/lib/LiveExecutionEngine.cpp
)

add_library(hft_lib ${LIB_SOURCES})
target_include_directories(hft_lib PUBLIC ${HFT_INCLUDE_DIRS})


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
    tests/integration/TestBrokerIntegration.cpp
)
target_link_libraries(hft_tests PRIVATE hft_lib)

enable_testing()
add_test(NAME hft_tests COMMAND hft_tests)

# TWS vendor linkage fix
if(HFT_ENABLE_IBKR)
  set(TWSAPI_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client")
  if(EXISTS "${TWSAPI_CLIENT_DIR}/client/EClient.cpp")
    set(TWSAPI_CLIENT_DIR "${TWSAPI_CLIENT_DIR}/client")
  endif()

  file(GLOB TWSAPI_VENDOR_SOURCES
    "${TWSAPI_CLIENT_DIR}/*.cpp"
    "${TWSAPI_CLIENT_DIR}/protobuf/*.cc"
  )

  if(NOT TWSAPI_VENDOR_SOURCES)
    message(FATAL_ERROR
      "No TWS API sources found under ${TWSAPI_CLIENT_DIR}. "
      "Check third_party/twsapi/client layout."
    )
  endif()

  add_library(twsapi_vendor STATIC ${TWSAPI_VENDOR_SOURCES})
  target_include_directories(twsapi_vendor PUBLIC
    ${TWSAPI_CLIENT_DIR}
    ${TWSAPI_CLIENT_DIR}/protobuf
    ${PROTOBUF_INCLUDE_DIR}
  )
  target_link_libraries(twsapi_vendor PUBLIC ${PROTOBUF_LIBRARY})

  target_sources(hft_lib PRIVATE
    src/lib/IBKRClient.cpp
    src/lib/LiveExecutionEngine.cpp
  )
  target_include_directories(hft_lib PUBLIC ${TWSAPI_CLIENT_DIR} ${TWSAPI_CLIENT_DIR}/protobuf ${PROTOBUF_INCLUDE_DIR})
  target_link_libraries(hft_lib PUBLIC twsapi_vendor ${PROTOBUF_LIBRARY})
endif()


# protobuf shared-library note:
# For HFT_ENABLE_IBKR=ON in CI, protobuf 29.3 is expected to be installed as a shared library.
