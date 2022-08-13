add_library(FuzzallocRuntime STATIC
  PAMD.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)