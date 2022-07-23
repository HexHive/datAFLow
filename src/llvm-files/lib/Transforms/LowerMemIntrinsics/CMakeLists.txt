add_library(fuzzalloc-lower-mem-intrinsics
  SHARED
  LowerMemIntrinsics.cpp
)
set_target_properties(fuzzalloc-lower-mem-intrinsics
  PROPERTIES LINK_FLAGS "-Wl,-znodelete"
)
