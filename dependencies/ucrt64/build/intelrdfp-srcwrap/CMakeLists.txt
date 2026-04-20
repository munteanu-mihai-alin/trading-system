cmake_minimum_required(VERSION 3.20)
project(intelrdfpmath_c_wrapper C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(RDFP_SRC_ROOT "D:/trading-system/third_party/IntelRDFPMathLib20U4/src")
file(TO_CMAKE_PATH "${RDFP_SRC_ROOT}" RDFP_SRC_ROOT_NORM)
file(GLOB_RECURSE RDFP_SOURCES "${RDFP_SRC_ROOT_NORM}/*.c")
message(STATUS "RDFP_SRC_ROOT_NORM=${RDFP_SRC_ROOT_NORM}")
list(LENGTH RDFP_SOURCES RDFP_SOURCES_COUNT)
message(STATUS "RDFP_SOURCES_COUNT=${RDFP_SOURCES_COUNT}")
if(NOT RDFP_SOURCES)
  message(FATAL_ERROR "No .c sources found under detected Intel RDFP source root: ${RDFP_SRC_ROOT_NORM}")
endif()
add_library(intelrdfpmath STATIC ${RDFP_SOURCES})
target_include_directories(intelrdfpmath PUBLIC "${RDFP_SRC_ROOT_NORM}")
target_compile_definitions(intelrdfpmath PRIVATE
  CALL_BY_REF=0
  GLOBAL_RND=0
  GLOBAL_FLAGS=0
  UNCHANGED_BINARY_FLAGS=0
)
set_target_properties(intelrdfpmath PROPERTIES OUTPUT_NAME "intelrdfpmath")
install(TARGETS intelrdfpmath
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(DIRECTORY "${RDFP_SRC_ROOT_NORM}/" DESTINATION include FILES_MATCHING PATTERN "bid*.h")
