llvm_map_components_to_libnames(LLVM_LIBS
  core
  support
)

add_library(FuzzallocRuntime STATIC
  BaggyBounds.c
  Hash.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)

add_library(DebugRuntime STATIC
  BaggyBounds.c
  Debug.cpp
)
target_link_libraries(DebugRuntime PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS DebugRuntime LIBRARY DESTINATION lib)
