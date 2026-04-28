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

set(HFT_INCLUDE_DIRS
  include
  tests
)

set(LIB_SOURCES
  src/lib/AppConfig.cpp
  src/lib/RankingEngine.cpp
  src/lib/IBKRClient.cpp
  src/lib/LiveExecutionEngine.cpp
  src/lib/RealIBKRTransport.cpp
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
  tests/module/TestBrokerIntegration.cpp
)
target_link_libraries(hft_tests PRIVATE hft_lib)

enable_testing()
add_test(NAME hft_tests COMMAND hft_tests)

# ---------------------------------------------------------------------------
# IBKR / TWS API integration (mandatory).
# Provides protobuf::libprotobuf, the Intel decimal runtime, and the vendored
# TWS API client compiled into twsapi_vendor. RealIBKRTransport.cpp (already
# in LIB_SOURCES above) is the single consumer inside hft_lib.
# ---------------------------------------------------------------------------
find_package(Protobuf CONFIG REQUIRED)

if(NOT TARGET protobuf::libprotobuf)
  message(FATAL_ERROR
    "protobuf::libprotobuf not found. Install libprotobuf-dev / set "
    "CMAKE_PREFIX_PATH to a prefix that provides the protobuf CMake package."
  )
endif()

set(TWSAPI_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client")

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

# Prebuilt libtwsapi_vendor.a from scripts/rebuild_linux_deps_ci.sh (linux-deps
# tarball). Search only CMAKE_PREFIX_PATH so we never pick up an unrelated system lib.
set(_HFT_TWS_PREBUILT_HINTS)
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _HFT_TWS_PREBUILT_HINTS "${_prefix}/lib")
endforeach()
find_library(HFT_TWSAPI_VENDOR_PREBUILT
  NAMES twsapi_vendor libtwsapi_vendor
  PATHS ${_HFT_TWS_PREBUILT_HINTS}
  NO_DEFAULT_PATH
)

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

if(HFT_TWSAPI_VENDOR_PREBUILT)
  message(STATUS "Using prebuilt twsapi_vendor static library: ${HFT_TWSAPI_VENDOR_PREBUILT}")
  add_library(twsapi_vendor STATIC IMPORTED GLOBAL)
  set_target_properties(twsapi_vendor PROPERTIES
    IMPORTED_LOCATION "${HFT_TWSAPI_VENDOR_PREBUILT}"
    INTERFACE_INCLUDE_DIRECTORIES "${TWSAPI_CLIENT_DIR};${TWSAPI_CLIENT_DIR}/protobuf"
  )
  target_link_libraries(twsapi_vendor INTERFACE hft_ibkr_link_deps)
else()
  message(STATUS "Building twsapi_vendor from vendored sources under ${TWSAPI_CLIENT_DIR}")
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
  add_library(twsapi_vendor STATIC ${TWSAPI_VENDOR_SOURCES})
  target_include_directories(twsapi_vendor PUBLIC
    "${TWSAPI_CLIENT_DIR}"
    "${TWSAPI_CLIENT_DIR}/protobuf"
  )
  target_link_libraries(twsapi_vendor PUBLIC hft_ibkr_link_deps)
endif()

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

# ---------------------------------------------------------------------------
# State-centric logging module (always built; spdlog is a hard dependency).
# Resolved via find_package(spdlog) first, falling back to vendored
# third_party/spdlog. Both UCRT staging and the Linux CI deps bundle install
# spdlog into CMAKE_PREFIX_PATH.
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG QUIET)
if(TARGET spdlog::spdlog)
  message(STATUS "Using spdlog from find_package: spdlog::spdlog")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt")
  set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
  message(STATUS "Using spdlog from vendored third_party/spdlog")
else()
  message(FATAL_ERROR
    "spdlog not found. Install libspdlog-dev, set CMAKE_PREFIX_PATH to a "
    "build prefix that provides spdlog (e.g. dependencies/ucrt64/install), "
    "or stage the source via scripts/stage_third_party_sources_ucrt.sh."
  )
endif()

target_sources(hft_lib PRIVATE
  src/lib/LoggingService.cpp
  src/lib/LoggingState.cpp
)
target_link_libraries(hft_lib PUBLIC spdlog::spdlog)

# ---------------------------------------------------------------------------
# GoogleTest + GoogleMock for the unit/module test split.
# Resolved via find_package(GTest) first, falling back to vendored
# third_party/googletest. Mirrors the spdlog pattern. UCRT staging and the
# Linux CI deps bundle both install GTest into CMAKE_PREFIX_PATH.
# ---------------------------------------------------------------------------
find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest)
  # Some Ubuntu apt packages provide GTest via the FindGTest module rather
  # than a CONFIG-mode package; fall back to MODULE-mode lookup.
  find_package(GTest QUIET)
endif()

if(TARGET GTest::gtest AND TARGET GTest::gmock)
  message(STATUS "Using GTest from find_package: GTest::gtest / GTest::gmock")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  if(TARGET gtest AND NOT TARGET GTest::gtest)
    add_library(GTest::gtest ALIAS gtest)
  endif()
  if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
  endif()
  if(TARGET gmock AND NOT TARGET GTest::gmock)
    add_library(GTest::gmock ALIAS gmock)
  endif()
  if(TARGET gmock_main AND NOT TARGET GTest::gmock_main)
    add_library(GTest::gmock_main ALIAS gmock_main)
  endif()
  message(STATUS "Using GTest from vendored third_party/googletest")
else()
  message(FATAL_ERROR
    "GTest not found. Install libgtest-dev/libgmock-dev, set CMAKE_PREFIX_PATH "
    "to a build prefix that provides GTest (e.g. dependencies/ucrt64/install), "
    "or stage the source via scripts/stage_third_party_sources_ucrt.sh."
  )
endif()

# New unit/module test binary. Lives alongside the legacy hft_tests target;
# both are registered with CTest. Phase 0 ships only a smoke test that
# exercises the gtest+gmock wiring; real unit/module tests land in Phase 1+.
add_executable(hft_gtests
  tests/common/GTestSmoke.cpp
  tests/unit/SpscRingTest.cpp
  tests/unit/StateRegistryTest.cpp
  tests/unit/LoggingStateTest.cpp
  tests/unit/AppConfigTest.cpp
  tests/unit/IBKRClientTest.cpp
  tests/unit/LiveExecutionEngineTest.cpp
)
target_link_libraries(hft_gtests PRIVATE
  hft_lib
  GTest::gtest
  GTest::gmock
  GTest::gtest_main
)
# Same link-order constraint as hft_tests: TWS API + decimal must follow
# hft_lib on the final link line for MinGW static archive resolution.
target_link_libraries(hft_gtests PRIVATE twsapi_vendor hft_ibkr_link_deps)
add_test(NAME hft_gtests COMMAND hft_gtests)
