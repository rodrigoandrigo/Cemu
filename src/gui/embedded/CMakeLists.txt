add_library(CemuEmbeddedGui STATIC
  EmbeddedWindowSystem.cpp
)

set_property(TARGET CemuEmbeddedGui PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME_LIBRARY}")
cemu_use_precompiled_header(CemuEmbeddedGui)
target_include_directories(CemuEmbeddedGui PUBLIC "../")
target_link_libraries(CemuEmbeddedGui PRIVATE CemuCommon)
