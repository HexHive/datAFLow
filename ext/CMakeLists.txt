#
# Abseil
#

add_subdirectory(abseil)

#
# AFL++
#

message(STATUS "Patching AFL++")
execute_process(
  COMMAND git apply "${CMAKE_SOURCE_DIR}/patches/afl-bitmap.patch"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aflplusplus"
)

add_library(AFLRuntime OBJECT
  aflplusplus/instrumentation/afl-compiler-rt.o.c
)

add_custom_target(aflplusplus
  ALL
  ${CMAKE_COMMAND} -E env LLVM_CONFIG="${LLVM_TOOLS_BINARY_DIR}/llvm-config" make
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aflplusplus"
)
install(PROGRAMS
  aflplusplus/afl-fuzz
  aflplusplus/afl-showmap
  DESTINATION bin
)

#
# SVF
#

add_subdirectory(svf)
