add_library(FuzzallocCommon OBJECT
  Common/BaggyBounds.c
  Common/Metadata.c
)
link_libraries(FuzzallocCommon)

add_subdirectory(Analysis)
add_subdirectory(Runtime)
add_subdirectory(Transforms)
