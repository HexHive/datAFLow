add_library(FuzzallocRuntime SHARED
  PAMD.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)