add_library(FuzzallocRuntime STATIC
  PAMD.cpp
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)