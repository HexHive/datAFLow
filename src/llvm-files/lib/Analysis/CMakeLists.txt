add_library(fuzzalloc-collect-stats
            SHARED
            CollectStats.cpp)
set_target_properties(fuzzalloc-collect-stats PROPERTIES LINK_FLAGS "-Wl,-znodelete")

add_library(fuzzalloc-collect-tag-sites
            SHARED
            CollectTagSites.cpp)
set_target_properties(fuzzalloc-collect-tag-sites PROPERTIES LINK_FLAGS "-Wl,-znodelete")
