#
# AFL++
#

message(STATUS "Patching AFL++")
execute_process(
  COMMAND git apply "${CMAKE_SOURCE_DIR}/patches/afl-bitmap.patch"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/aflplusplus"
)
add_library(AFLRuntime STATIC
  aflplusplus/instrumentation/afl-compiler-rt.o.c
)
install(TARGETS AFLRuntime LIBRARY DESTINATION lib)

#
# SVF
#

add_subdirectory(svf)
