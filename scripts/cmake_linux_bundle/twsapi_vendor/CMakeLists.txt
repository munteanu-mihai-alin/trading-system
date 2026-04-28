# Standalone build of the vendored Interactive Brokers TWS API client into a
# single static archive, installed into CMAKE_INSTALL_PREFIX (used by
# scripts/rebuild_linux_deps_ci.sh for the linux-deps release tarball).
#
# Invoked with:
#   -DCMAKE_PREFIX_PATH=<deps install prefix>   (protobuf + Intel RDFP already installed)
#   -DTWSAPI_VENDOR_SOURCE_ROOT=<repo root>    (contains third_party/twsapi/client)

cmake_minimum_required(VERSION 3.18)

project(twsapi_vendor_bundle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED TWSAPI_VENDOR_SOURCE_ROOT)
  message(FATAL_ERROR
    "TWSAPI_VENDOR_SOURCE_ROOT must point to the repository root "
    "(directory containing third_party/twsapi/client)."
  )
endif()

find_package(Protobuf CONFIG REQUIRED)

set(TWSAPI_CLIENT_DIR "${TWSAPI_VENDOR_SOURCE_ROOT}/third_party/twsapi/client")

file(GLOB TWSAPI_VENDOR_SOURCES
  "${TWSAPI_CLIENT_DIR}/*.cpp"
  "${TWSAPI_CLIENT_DIR}/protobuf/*.cc"
)

if(NOT TWSAPI_VENDOR_SOURCES)
  message(FATAL_ERROR "No TWS API sources under ${TWSAPI_CLIENT_DIR}")
endif()

set(_lib_hints)
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _lib_hints "${_prefix}/lib")
endforeach()
list(APPEND _lib_hints /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu)

find_library(HFT_INTEL_RDFP_LIBRARY
  NAMES intelrdfpmath libintelrdfpmath bid bidgcc000 bidgcc000b
  PATHS ${_lib_hints}
)

if(NOT HFT_INTEL_RDFP_LIBRARY)
  message(FATAL_ERROR
    "Intel decimal runtime not found under CMAKE_PREFIX_PATH/lib "
    "(linux-deps bundle should install copied BID archives)."
  )
endif()

find_package(Threads REQUIRED)

add_library(twsapi_vendor STATIC ${TWSAPI_VENDOR_SOURCES})
set_target_properties(twsapi_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(twsapi_vendor PUBLIC
  "${TWSAPI_CLIENT_DIR}"
  "${TWSAPI_CLIENT_DIR}/protobuf"
)

target_link_libraries(twsapi_vendor PUBLIC
  protobuf::libprotobuf
  "${HFT_INTEL_RDFP_LIBRARY}"
  Threads::Threads
)

install(TARGETS twsapi_vendor ARCHIVE DESTINATION lib)
