find_program(CLANG_BINARY "clang" HINTS ${LLVM_TOOLS_BINARY_DIR})
find_program(CLANGXX_BINARY "clang++" HINTS ${LLVM_TOOLS_BINARY_DIR})

configure_file(dataflow-cc.sh.in dataflow-cc @ONLY)
configure_file(dataflow-c++.sh.in dataflow-c++ @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-cc" TYPE BIN)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-c++" TYPE BIN)
