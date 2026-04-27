cmake_minimum_required(VERSION 3.18)

project(HFTMergedRecovered LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
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

if(HFT_ENABLE_IBKR)
  find_package(Protobuf CONFIG REQUIRED)

  if(NOT TARGET protobuf::libprotobuf)
    message(FATAL_ERROR
      "HFT_ENABLE_IBKR=ON requires protobuf::libprotobuf from the installed protobuf CMake package. "
      "Check CMAKE_PREFIX_PATH."
    )
  endif()

  set(TWSAPI_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client")

  file(GLOB TWSAPI_VENDOR_SOURCES
    "${TWSAPI_CLIENT_DIR}/*.cpp"
    "${TWSAPI_CLIENT_DIR}/protobuf/*.cc"
  )

  if(NOT TWSAPI_VENDOR_SOURCES)
    message(FATAL_ERROR
      "No TWS API sources found under ${TWSAPI_CLIENT_DIR}. "
      "Expected third_party/twsapi/client/*.cpp and protobuf/*.cc"
    )
  endif()

  set(_HFT_RDFP_HINTS)
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _HFT_RDFP_HINTS "${_prefix}/lib")
  endforeach()
  if(UNIX)
    list(APPEND _HFT_RDFP_HINTS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu)
  endif()

  find_library(HFT_INTEL_RDFP_LIBRARY
    NAMES intelrdfpmath libintelrdfpmath bid bidgcc000 bidgcc000b
    PATHS ${_HFT_RDFP_HINTS}
  )

  if(NOT HFT_INTEL_RDFP_LIBRARY)
    message(FATAL_ERROR
      "Intel decimal runtime not found; provide libintelrdfpmath or a BID-compatible library in CMAKE_PREFIX_PATH/lib."
    )
  endif()

  message(STATUS "Using protobuf target: protobuf::libprotobuf")
  message(STATUS "Using Intel decimal runtime: ${HFT_INTEL_RDFP_LIBRARY}")
  message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

  add_library(hft_ibkr_link_deps INTERFACE)
  target_link_libraries(hft_ibkr_link_deps INTERFACE
    protobuf::libprotobuf
    "${HFT_INTEL_RDFP_LIBRARY}"
  )

  if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(hft_ibkr_link_deps INTERFACE Threads::Threads)
  endif()

  if(WIN32)
    target_link_libraries(hft_ibkr_link_deps INTERFACE ws2_32)
  endif()

  add_library(twsapi_vendor STATIC ${TWSAPI_VENDOR_SOURCES})
  target_include_directories(twsapi_vendor PUBLIC
    "${TWSAPI_CLIENT_DIR}"
    "${TWSAPI_CLIENT_DIR}/protobuf"
  )

  # Keep protobuf/decimal/socket requirements transitive for library consumers.
  target_link_libraries(twsapi_vendor PUBLIC hft_ibkr_link_deps)

  target_compile_definitions(hft_lib PUBLIC HFT_ENABLE_IBKR=1)
  target_include_directories(hft_lib PUBLIC
    "${TWSAPI_CLIENT_DIR}"
    "${TWSAPI_CLIENT_DIR}/protobuf"
  )
  target_link_libraries(hft_lib PUBLIC twsapi_vendor hft_ibkr_link_deps)

  # MinGW static archive ordering can still leave BID symbols unresolved if the
  # decimal archive appears too early on the final executable link line. Attach
  # the dependency bundle directly after the app/test object and project library
  # as well, so libintelrdfpmath.a is seen after libtwsapi_vendor.a.
  target_link_libraries(hft_app PRIVATE twsapi_vendor hft_ibkr_link_deps)
  target_link_libraries(hft_tests PRIVATE twsapi_vendor hft_ibkr_link_deps)
endif()
