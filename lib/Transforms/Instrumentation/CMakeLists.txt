add_library(UseSite SHARED
  UseSite.cpp
)
target_link_libraries(UseSite PUBLIC
  UseSiteIdentify
)
install(TARGETS UseSite LIBRARY DESTINATION lib)

add_library(Utils SHARED
  Utils.cpp
)
install(TARGETS Utils LIBRARY DESTINATION lib)

add_library(GlobalVariableTag SHARED
  GlobalVariableTag.cpp
)
target_link_libraries(GlobalVariableTag PUBLIC
  DefSiteIdentify
  Utils
)
install(TARGETS GlobalVariableTag LIBRARY DESTINATION lib)

add_library(HeapTag SHARED
  HeapTag.cpp
)
target_link_libraries(HeapTag PUBLIC
  MemFuncIdentify
  Utils
)
install(TARGETS HeapTag LIBRARY DESTINATION lib)

add_library(LocalVariableTag SHARED
  LocalVariableTag.cpp
)
target_link_libraries(LocalVariableTag PUBLIC
  DefSiteIdentify
  Utils
)
install(TARGETS LocalVariableTag LIBRARY DESTINATION lib)
