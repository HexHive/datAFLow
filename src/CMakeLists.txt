set(AFL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/aflplusplus")

include_directories("${AFL_DIR}/include")
include_directories(include)

add_subdirectory(llvm-files)
add_subdirectory(runtime)
add_subdirectory(tools)
