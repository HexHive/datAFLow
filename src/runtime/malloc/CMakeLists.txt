add_library(fuzzalloc SHARED malloc.c
                             mem_access.c
                             debug.c)
add_dependencies(fuzzalloc ptmalloc3)

if(FUZZALLOC_USE_LOCKS)
  add_definitions(-DFUZZALLOC_USE_LOCKS)
endif()

add_definitions(-DMSPACES)

target_include_directories(fuzzalloc PRIVATE
                           "${CMAKE_SOURCE_DIR}/src/runtime/ptmalloc3")
target_link_libraries(fuzzalloc PRIVATE
                      "${CMAKE_BINARY_DIR}/src/runtime/ptmalloc3/libptmalloc3.so")
install(TARGETS fuzzalloc LIBRARY DESTINATION lib)
