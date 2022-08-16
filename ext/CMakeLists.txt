#
# AFL++
#

add_library(AFLRuntime STATIC
  aflplusplus/instrumentation/afl-compiler-rt.o.c
)
install(TARGETS AFLRuntime LIBRARY DESTINATION lib)

#
# SVF
#

add_subdirectory(svf)