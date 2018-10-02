add_library(fuzzalloc-rewrite-news
            SHARED
            RewriteNews.cpp)
set_target_properties(fuzzalloc-rewrite-news PROPERTIES LINK_FLAGS "-Wl,-znodelete")
