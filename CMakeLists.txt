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

if (HFT_ENABLE_IBKR)
  find_path(PROTOBUF_INCLUDE_DIR google/protobuf/message.h)
  find_library(PROTOBUF_LIBRARY protobuf)

  if (NOT PROTOBUF_INCLUDE_DIR OR NOT PROTOBUF_LIBRARY)
    message(FATAL_ERROR "HFT_ENABLE_IBKR=ON requires protobuf C++ development headers and library. Missing google/protobuf headers or libprotobuf.")
  endif()

  file(GLOB TWSAPI_CLIENT_SOURCES
      third_party/twsapi/client/*.cpp
      third_party/twsapi/client/protobuf/*.pb.cc
  )

  add_library(twsapi_vendor ${TWSAPI_CLIENT_SOURCES})
  target_include_directories(twsapi_vendor PUBLIC
      third_party/twsapi/client
      third_party/twsapi/client/protobuf
      ${PROTOBUF_INCLUDE_DIR}
  )
  target_link_libraries(twsapi_vendor PUBLIC ${PROTOBUF_LIBRARY})
  if (UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(twsapi_vendor PUBLIC Threads::Threads)
  endif()

  target_compile_definitions(hft_lib PUBLIC HFT_ENABLE_IBKR=1)
  target_include_directories(hft_lib PUBLIC
      third_party/twsapi/client
      third_party/twsapi/client/protobuf
      ${PROTOBUF_INCLUDE_DIR}
  )
  target_link_libraries(hft_lib PUBLIC twsapi_vendor)
endif()

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
  find_package(Protobuf REQUIRED)
  message(STATUS "Found Protobuf ${Protobuf_VERSION}")

  target_include_directories(hft_lib PUBLIC ${Protobuf_INCLUDE_DIRS})
  target_link_libraries(hft_lib PUBLIC ${Protobuf_LIBRARIES})

  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client)
    target_include_directories(hft_lib PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client/protobuf
    )
  endif()
endif()


# TWS vendor linkage fix
if(HFT_ENABLE_IBKR)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client)
    file(GLOB TWSAPI_VENDOR_SRC
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client/*.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client/protobuf/*.pb.cc
    )

    if(NOT TARGET twsapi_vendor)
      add_library(twsapi_vendor STATIC ${TWSAPI_VENDOR_SRC})
      target_include_directories(twsapi_vendor PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/twsapi/client/protobuf
      )
      target_compile_options(twsapi_vendor PRIVATE
        -Wno-stringop-overflow
        -Wno-array-bounds
      )
    endif()

    if(TARGET protobuf::libprotobuf)
      target_link_libraries(twsapi_vendor PUBLIC protobuf::libprotobuf)
    else()
      target_include_directories(twsapi_vendor PUBLIC ${Protobuf_INCLUDE_DIRS})
      target_link_libraries(twsapi_vendor PUBLIC ${Protobuf_LIBRARIES})
    endif()

    find_package(absl CONFIG REQUIRED)
    if(TARGET absl::base)
      target_link_libraries(twsapi_vendor PUBLIC
        absl::base
        absl::strings
        absl::log_initialize
        absl::log
        absl::log_internal_check_op
        absl::log_internal_message
        absl::log_internal_format
        absl::hash
        absl::raw_hash_set
        absl::city
        absl::low_level_hash
      )
    endif()

    find_library(BID_LIBRARY
      NAMES
        intelrdfpmath bid libbid libintelrdfpmath
        bidgcc000 bidgcc000b
        bidgcc001 bidgcc001b
        bidgcc010 bidgcc010b
        bidgcc011 bidgcc011b
        bidgcc100 bidgcc100b
        bidgcc101 bidgcc101b
        bidgcc110 bidgcc110b
        bidgcc111 bidgcc111b
      PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
      PATH_SUFFIXES lib lib64 x86_64-linux-gnu
    )
    if(NOT BID_LIBRARY)
      message(FATAL_ERROR "Intel decimal runtime not found; install libintelrdfpmath-dev or provide a BID-compatible library")
    endif()
    message(STATUS "Found Intel decimal runtime: ${BID_LIBRARY}")
    target_link_libraries(twsapi_vendor PUBLIC ${BID_LIBRARY})

    target_link_libraries(hft_lib PUBLIC twsapi_vendor)
  endif()
endif()


# protobuf shared-library note:
# For HFT_ENABLE_IBKR=ON in CI, protobuf 29.3 is expected to be installed as a shared library.
