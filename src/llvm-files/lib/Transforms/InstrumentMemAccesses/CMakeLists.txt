add_library(fuzzalloc-inst-mem-accesses
            SHARED
            InstrumentMemAccesses.cpp)
target_link_libraries(fuzzalloc-inst-mem-accesses
                      PRIVATE
                      fuzzalloc-utils)
set_target_properties(fuzzalloc-inst-mem-accesses PROPERTIES LINK_FLAGS "-Wl,-znodelete")
