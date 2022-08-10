add_library(AFLUseSiteInstrument SHARED
  AFLUseSiteInstrument.cpp
)
target_link_libraries(AFLUseSiteInstrument PUBLIC
  UseSiteIdentify
)
install(TARGETS AFLUseSiteInstrument LIBRARY DESTINATION lib)
