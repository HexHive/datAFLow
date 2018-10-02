add_library(fuzzalloc-lower-atomics
            SHARED
            LowerAtomics.cpp)
set_target_properties(fuzzalloc-lower-atomics PROPERTIES LINK_FLAGS "-Wl,-znodelete")
