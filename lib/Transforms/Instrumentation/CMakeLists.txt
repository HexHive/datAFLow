add_library(AFLUseSite SHARED
  AFLUseSite.cpp
)
target_link_libraries(AFLUseSite PUBLIC
  UseSiteIdentify
)
install(TARGETS AFLUseSite LIBRARY DESTINATION lib)

add_library(MemFuncTag SHARED
  MemFuncTag.cpp
)
target_link_libraries(MemFuncTag PUBLIC
  MemFuncIdentify
)
install(TARGETS MemFuncTag LIBRARY DESTINATION lib)
