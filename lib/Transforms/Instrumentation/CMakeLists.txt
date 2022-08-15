add_library(AFLUseSite SHARED
  AFLUseSite.cpp
)
target_link_libraries(AFLUseSite PUBLIC
  UseSiteIdentify
)
install(TARGETS AFLUseSite LIBRARY DESTINATION lib)

add_library(DebugUseSite SHARED
  DebugUseSite.cpp
)
target_link_libraries(DebugUseSite PUBLIC
  UseSiteIdentify
)
install(TARGETS DebugUseSite LIBRARY DESTINATION lib)

add_library(GlobalVariableTag SHARED
  GlobalVariableTag.cpp
)
target_link_libraries(GlobalVariableTag PUBLIC
  DefSiteIdentify
)
install(TARGETS GlobalVariableTag LIBRARY DESTINATION lib)

add_library(HeapTag SHARED
  HeapTag.cpp
)
target_link_libraries(HeapTag PUBLIC
  MemFuncIdentify
)
install(TARGETS HeapTag LIBRARY DESTINATION lib)

add_library(LocalVariableTag SHARED
  LocalVariableTag.cpp
)
target_link_libraries(LocalVariableTag PUBLIC
  DefSiteIdentify
)
install(TARGETS LocalVariableTag LIBRARY DESTINATION lib)