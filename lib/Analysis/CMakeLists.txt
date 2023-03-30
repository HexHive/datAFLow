add_library(CollectStats SHARED
  CollectStats.cpp
)
target_link_libraries(CollectStats PUBLIC
  VariableRecovery
)
install(TARGETS CollectStats LIBRARY DESTINATION lib)

add_library(DefSiteIdentify SHARED
  DefSiteIdentify.cpp
)
target_link_libraries(DefSiteIdentify PUBLIC
  MemFuncIdentify
  VariableRecovery
)
install(TARGETS DefSiteIdentify LIBRARY DESTINATION lib)

if(USE_SVF)
  add_library(DefUseChain SHARED
    DefUseChain.cpp
  )
  target_link_libraries(DefUseChain PUBLIC
    MemFuncIdentify
    VariableRecovery
    Svf
    absl::flat_hash_map
    absl::flat_hash_set
  )
  install(TARGETS DefUseChain LIBRARY DESTINATION lib)
endif(USE_SVF)

add_library(MemFuncIdentify SHARED
  MemFuncIdentify.cpp
)
install(TARGETS MemFuncIdentify LIBRARY DESTINATION lib)

add_library(UseSiteIdentify SHARED
  UseSiteIdentify.cpp
)
target_link_libraries(UseSiteIdentify PUBLIC
  DefSiteIdentify
)
install(TARGETS UseSiteIdentify LIBRARY DESTINATION lib)

add_library(VariableRecovery SHARED
  VariableRecovery.cpp
)
install(TARGETS VariableRecovery LIBRARY DESTINATION lib)
