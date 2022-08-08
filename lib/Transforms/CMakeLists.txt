add_library(Heapify SHARED
  Heapify.cpp
)
target_link_libraries(Heapify PUBLIC
  DefSiteIdentify
)
install(TARGETS Heapify LIBRARY DESTINATION lib)

add_subdirectory(Utils)
