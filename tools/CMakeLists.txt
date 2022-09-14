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

install(PROGRAMS ddg-ratio.py
  RENAME ddg-ratio
  TYPE BIN
)

add_executable(inst-stats inst-stats.cpp)
target_link_libraries(inst-stats PRIVATE
  CollectStats
  ${LLVM_LIBS}
)
install(TARGETS inst-stats RUNTIME DESTINATION bin)

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

add_executable(dua-subsumption dua-subsumption.cpp)
target_link_libraries(dua-subsumption PRIVATE
  DefUseChain
  Svf
  ${LLVM_LIBS}
)

install(PROGRAMS timestamp-angora-queue.py
  RENAME timestamp-angora-queue
  TYPE BIN
)
