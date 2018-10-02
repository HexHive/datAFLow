if(NOT DEFINED AFL_DIR AND DEFINED ENV{AFL_DIR})
  set(AFL_DIR "$ENV{AFL_DIR}")
endif()

if(NOT DEFINED AFL_DIR)
  message(FATAL_ERROR "AFL++ source code path not specified. "
                      "Set AFL_DIR appropriately.")
endif()

get_filename_component(AFL_DIR ${AFL_DIR} ABSOLUTE)

if(EXISTS "${AFL_DIR}/src/afl-fuzz.c")
  message(STATUS "AFL++ source code found at ${AFL_DIR}")
else()
  message(FATAL_ERROR "afl-fuzz.c not found in ${AFL_DIR}. "
                      "Are you sure that this is the AFL++ source directory? "
                      "Set AFL_DIR appropriately.")
endif()

include_directories("${AFL_DIR}/include")
include_directories(include)

add_subdirectory(llvm-files)
add_subdirectory(runtime)
add_subdirectory(tools)
