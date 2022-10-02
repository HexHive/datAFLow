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

install(PROGRAMS bootstrap-cov.py
  RENAME bootstrap-cov
  TYPE BIN
)

configure_file(dataflow-cc.py.in dataflow-cc @ONLY)
configure_file(dataflow-cc.py.in dataflow-c++ @ONLY)

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-cc" TYPE BIN)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-c++" TYPE BIN)

add_executable(dua-cov-json
  dua-cov-json.cpp
  CovJSONCommon.cpp
)
target_link_libraries(dua-cov-json PRIVATE
  ${LLVM_LIBS}
  absl::flat_hash_map
  absl::flat_hash_set
)
install(TARGETS dua-cov-json RUNTIME DESTINATION bin)

add_executable(dataflow-stats dataflow-stats.cpp)
target_link_libraries(dataflow-stats PRIVATE
  CollectStats
  ${LLVM_LIBS}
)
install(TARGETS dataflow-stats RUNTIME DESTINATION bin)

add_executable(llvm-cov-json
  llvm-cov-json.cpp
  CovJSONCommon.cpp
)
target_link_libraries(llvm-cov-json PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS llvm-cov-json RUNTIME DESTINATION bin)

add_executable(static-dua static-dua.cpp)
target_link_libraries(static-dua PRIVATE
  DefUseChain
  ${LLVM_LIBS}
)
install(TARGETS static-dua RUNTIME DESTINATION bin)

add_executable(static-llvm-cov static-llvm-cov.cpp)
target_link_libraries(static-llvm-cov PRIVATE
  ${LLVM_LIBS}
)
install(TARGETS static-llvm-cov RUNTIME DESTINATION bin)

install(PROGRAMS timestamp-angora-queue.py
  RENAME timestamp-angora-queue
  TYPE BIN
)
