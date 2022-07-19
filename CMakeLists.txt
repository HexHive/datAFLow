# Same version required by LLVM
cmake_minimum_required(VERSION 3.13.4)

project(datAFLow
  LANGUAGES C CXX
  DESCRIPTION "Data-flow-based fuzzing"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror")

if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif(NOT LLVM_ENABLE_RTTI)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

#
# LLVM
#

find_package(LLVM 12 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

#
# AFL++
#

set(AFL_DIR "${CMAKE_SOURCE_DIR}/ext/aflplusplus")

add_custom_target(aflplusplus ALL
  COMMAND
    LLVM_CONFIG="${LLVM_TOOLS_BINARY_DIR}/llvm-config"
    PREFIX=${CMAKE_INSTALL_PREFIX}
    make -j
  WORKING_DIRECTORY ${AFL_DIR}
)

set(AFL_INSTALL_CMD "make -C ${AFL_DIR} install PREFIX=${CMAKE_INSTALL_PREFIX}")
install(CODE
    "execute_process(COMMAND ${AFL_INSTALL_CMD})"
)

#
# SVF
#

option(USE_SVF "Use the SVF static analysis framework (requires Z3)" FALSE)
if(USE_SVF)
  add_subdirectory(ext/svf)
  set(SVF_DIR "${CMAKE_SOURCE_DIR}/ext/svf")
endif(USE_SVF)

#
# datAFLow
#

add_subdirectory(src)
