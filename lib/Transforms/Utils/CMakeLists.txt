add_library(LowerAtomic SHARED
  LowerAtomic.cpp
)
install(TARGETS LowerAtomic LIBRARY DESTINATION lib)

add_library(LowerConstantExpr SHARED
  LowerConstantExpr.cpp
)
install(TARGETS LowerConstantExpr LIBRARY DESTINATION lib)

add_library(LowerDbgDeclare SHARED
  LowerDbgDeclare.cpp
)
install(TARGETS LowerDbgDeclare LIBRARY DESTINATION lib)

add_library(LowerNewDelete SHARED
  LowerNewDelete.cpp
)
target_link_libraries(LowerNewDelete PUBLIC
  MemFuncIdentify
)
install(TARGETS LowerNewDelete LIBRARY DESTINATION lib)

add_library(LowerMemIntrinsic SHARED
  LowerMemIntrinsic.cpp
)
install(TARGETS LowerMemIntrinsic LIBRARY DESTINATION lib)

add_library(Mem2Reg SHARED
  Mem2Reg.cpp
)
install(TARGETS Mem2Reg LIBRARY DESTINATION lib)
