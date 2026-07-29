add_library(CemuEmbeddedGui STATIC
  EmbeddedWindowSystem.cpp
)

cemu_use_precompiled_header(CemuEmbeddedGui)
target_include_directories(CemuEmbeddedGui PUBLIC "../")
target_link_libraries(CemuEmbeddedGui PRIVATE CemuCommon)
