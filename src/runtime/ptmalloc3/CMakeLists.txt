add_library(ptmalloc3 SHARED ptmalloc3.c malloc.c)

set_property(SOURCE ptmalloc3.c
             PROPERTY COMPILE_DEFINITIONS
             USE_TSD_DATA_HACK MSPACES=1)
if(FUZZALLOC_USE_LOCKS)
  set_property(SOURCE malloc.c
               PROPERTY COMPILE_DEFINITIONS
               USE_TSD_DATA_HACK ONLY_MSPACES USE_LOCKS=1 MALLOC_ALIGNMENT=16)
else()
  set_property(SOURCE malloc.c
               PROPERTY COMPILE_DEFINITIONS
               USE_TSD_DATA_HACK ONLY_MSPACES MALLOC_ALIGNMENT=16)
endif()
if(CMAKE_BUILD_TYPE MATCHES DEBUG)
  set_property(SOURCE malloc.c
               PROPERTY COMPILE_DEFINITIONS
               DEBUG=1)
  set_property(SOURCE ptmalloc3.c
               PROPERTY COMPILE_DEFINITIONS
               MALLOC_DEBUG=1)
endif()

# Need to remove -Werror
string(REPLACE " -Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

target_include_directories(ptmalloc3 PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/sysdeps/pthread"
                           "${CMAKE_CURRENT_SOURCE_DIR}/sysdeps/generic")
target_link_libraries(ptmalloc3 PRIVATE pthread)
install(TARGETS ptmalloc3 LIBRARY DESTINATION lib)
