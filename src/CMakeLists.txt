include_directories(
  "${AFL_DIR}/include"
  include
)

add_subdirectory(llvm-files)
add_subdirectory(runtime)
add_subdirectory(tools)
