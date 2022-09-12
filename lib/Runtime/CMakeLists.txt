llvm_map_components_to_libnames(LLVM_LIBS
  support
)

add_library(FuzzallocRuntime STATIC
  BaggyBounds.c
  Hash.c
)
install(TARGETS FuzzallocRuntime LIBRARY DESTINATION lib)

add_library(LLVMCovRuntime STATIC
  LLVMCov.c
)
install(TARGETS LLVMCovRuntime LIBRARY DESTINATION lib)

add_library(TracerRuntime STATIC
  BaggyBounds.c
  Tracer.cpp
)
target_link_libraries(TracerRuntime PUBLIC
  ${LLVM_LIBS}
)
install(TARGETS TracerRuntime LIBRARY DESTINATION lib)
