configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/Config.h
  @ONLY
)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

llvm_map_components_to_libnames(LLVM_LIBS
  bitwriter
  core
  ipo
  irreader
  instcombine
  instrumentation
  target
  linker
  analysis
  scalaropts
  support
  transformutils
)

# Need to remove -Werror when using AFL source code
string(REPLACE " -Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

add_executable(dataflow-cc dataflow-cc.c common.c)
add_executable(dataflow-cc++ dataflow-cc.c common.c)
add_executable(dataflow-pp dataflow-pp.c common.c)
add_executable(dataflow-pp++ dataflow-pp.c common.c)

install(TARGETS dataflow-cc RUNTIME DESTINATION bin)
install(TARGETS dataflow-cc++ RUNTIME DESTINATION bin)
install(TARGETS dataflow-pp RUNTIME DESTINATION bin)
install(TARGETS dataflow-pp++ RUNTIME DESTINATION bin)

#
# Statistics
#

add_executable(dataflow-stats dataflow-stats.cpp)
target_link_libraries(dataflow-stats PRIVATE
  fuzzalloc-collect-stats
  ${LLVM_LIBS}
)
install(TARGETS dataflow-stats RUNTIME DESTINATION bin)

#
# SVF static analysis
#

if(USE_SVF)
include_directories(
  "${SVF_DIR}/include"
)

add_executable(dataflow-sa dataflow-sa.cpp)
target_link_libraries(dataflow-sa PRIVATE
  fuzzalloc-variable-recovery
  Svf
  ${LLVM_LIBS}
)
install(TARGETS dataflow-sa RUNTIME DESTINATION bin)
endif(USE_SVF)
