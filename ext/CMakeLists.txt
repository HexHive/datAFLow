add_library(AFLRuntime STATIC
  aflplusplus/instrumentation/afl-compiler-rt.o.c
)
target_include_directories(AFLRuntime PRIVATE
  aflplusplus/include
)
install(TARGETS AFLRuntime LIBRARY DESTINATION lib)
