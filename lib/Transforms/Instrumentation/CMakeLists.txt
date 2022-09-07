add_library(TagUtils OBJECT
  TagUtils.cpp
)

add_library(UseSite SHARED
  UseSite.cpp
)
target_link_libraries(UseSite PUBLIC
  UseSiteIdentify
)
install(TARGETS UseSite LIBRARY DESTINATION lib)

add_library(TracerUtils SHARED
  TracerUtils.cpp
)
install(TARGETS TracerUtils LIBRARY DESTINATION lib)

add_library(GlobalVariableTag SHARED
  GlobalVariableTag.cpp
)
target_link_libraries(GlobalVariableTag PUBLIC
  DefSiteIdentify
  TagUtils
  TracerUtils
)
install(TARGETS GlobalVariableTag LIBRARY DESTINATION lib)

add_library(HeapTag SHARED
  HeapTag.cpp
)
target_link_libraries(HeapTag PUBLIC
  MemFuncIdentify
  TagUtils
  TracerUtils
)
install(TARGETS HeapTag LIBRARY DESTINATION lib)

add_library(LocalVariableTag SHARED
  LocalVariableTag.cpp
)
target_link_libraries(LocalVariableTag PUBLIC
  DefSiteIdentify
  TagUtils
  TracerUtils
)
install(TARGETS LocalVariableTag LIBRARY DESTINATION lib)
