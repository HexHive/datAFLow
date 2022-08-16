add_library(FuzzallocRuntime STATIC
  BaggyBounds.c
  Hash.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)