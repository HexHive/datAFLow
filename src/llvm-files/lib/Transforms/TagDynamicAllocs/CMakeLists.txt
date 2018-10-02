add_library(fuzzalloc-tag-dyn-allocs
            SHARED
            TagDynamicAllocs.cpp)
target_link_libraries(fuzzalloc-tag-dyn-allocs PRIVATE fuzzalloc-utils)
set_target_properties(fuzzalloc-tag-dyn-allocs PROPERTIES LINK_FLAGS "-Wl,-znodelete")
