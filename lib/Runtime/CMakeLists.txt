llvm_map_components_to_libnames(LLVM_LIBS
  support
)

add_library(FuzzallocRuntime STATIC
  BaggyBounds.c
  Hash.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)

add_library(TracerRuntime STATIC
  BaggyBounds.c
  Tracer.cpp
)
target_link_libraries(TracerRuntime PUBLIC
  ${LLVM_LIBS}
  absl::flat_hash_map
  absl::synchronization
)
install(TARGETS TracerRuntime LIBRARY DESTINATION lib)
