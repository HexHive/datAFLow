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
  coverage
)

configure_file(dataflow-cc.py.in dataflow-cc @ONLY)
configure_file(dataflow-cc.py.in dataflow-c++ @ONLY)

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-cc" TYPE BIN)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-c++" TYPE BIN)

add_executable(dataflow-sa dataflow-sa.cpp)
target_link_libraries(dataflow-sa PRIVATE
  VariableRecovery
  Svf
  ${LLVM_LIBS}
)
install(TARGETS dataflow-sa RUNTIME DESTINATION bin)

add_executable(dataflow-stats dataflow-stats.cpp)
target_link_libraries(dataflow-stats PRIVATE
  CollectStats
  ${LLVM_LIBS}
)
install(TARGETS dataflow-stats RUNTIME DESTINATION bin)

add_executable(region-cov region-cov.cpp)
target_link_libraries(region-cov PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS region-cov RUNTIME DESTINATION bin)
