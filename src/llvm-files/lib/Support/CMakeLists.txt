add_library(fuzzalloc-utils
            SHARED
            FuzzallocUtils.cpp)
set_target_properties(fuzzalloc-utils PROPERTIES POSITION_INDEPENDENT_CODE ON)
