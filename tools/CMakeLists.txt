configure_file(dataflow-cc.py.in dataflow-cc @ONLY)
configure_file(dataflow-cc.py.in dataflow-c++ @ONLY)

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-cc" TYPE BIN)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/dataflow-c++" TYPE BIN)
