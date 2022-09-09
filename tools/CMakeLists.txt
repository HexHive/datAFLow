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

install(PROGRAMS ddg-ratio.py RENAME ddg-ratio TYPE BIN)

add_executable(inst-stats inst-stats.cpp)
target_link_libraries(inst-stats PRIVATE
  CollectStats
  ${LLVM_LIBS}
)
install(TARGETS inst-stats RUNTIME DESTINATION bin)

add_executable(llvm-cov-json llvm-cov-json.cpp)
target_link_libraries(llvm-cov-json PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS llvm-cov-json RUNTIME DESTINATION bin)

add_executable(static-def-use-chains static-def-use-chains.cpp)
target_link_libraries(static-def-use-chains PRIVATE
  MemFuncIdentify
  VariableRecovery
  Svf
  ${LLVM_LIBS}
  absl::flat_hash_map
  absl::flat_hash_set
)
install(TARGETS static-def-use-chains RUNTIME DESTINATION bin)

add_executable(static-region-cov static-region-cov.cpp)
target_link_libraries(static-region-cov PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS static-region-cov RUNTIME DESTINATION bin)
