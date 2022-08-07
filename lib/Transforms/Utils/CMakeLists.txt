add_library(LowerAtomics SHARED
  LowerAtomics.cpp
)
install(TARGETS LowerAtomics LIBRARY DESTINATION lib)

add_library(LowerDbgDeclare SHARED
  LowerDbgDeclare.cpp
)
install(TARGETS LowerDbgDeclare LIBRARY DESTINATION lib)

add_Library(LowerMemIntrinsics SHARED
  LowerMemIntrinsics.cpp
)
install(TARGETS LowerMemIntrinsics LIBRARY DESTINATION lib)
