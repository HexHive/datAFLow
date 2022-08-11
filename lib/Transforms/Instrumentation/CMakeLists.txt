add_library(AFLUseSiteInstrument SHARED
  AFLUseSiteInstrument.cpp
)
target_link_libraries(AFLUseSiteInstrument PUBLIC
  UseSiteIdentify
)
install(TARGETS AFLUseSiteInstrument LIBRARY DESTINATION lib)

add_library(MemFuncInstrument SHARED
  MemFuncInstrument.cpp
)
target_link_libraries(MemFuncInstrument PUBLIC
  MemFuncIdentify
)
install(TARGETS MemFuncInstrument LIBRARY DESTINATION lib)
