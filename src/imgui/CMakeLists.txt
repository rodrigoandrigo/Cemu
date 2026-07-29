add_library(imguiImpl
	imgui_extension.cpp
	imgui_extension.h
)

if (ENABLE_OPENGL)
	target_sources(imguiImpl PRIVATE
		imgui_impl_opengl3.cpp
		imgui_impl_opengl3.h
	)
endif()

if (ENABLE_VULKAN)
	target_sources(imguiImpl PRIVATE
		imgui_impl_vulkan.cpp
		imgui_impl_vulkan.h
	)
endif()

if (ENABLE_METAL)
    target_sources(imguiImpl PRIVATE
        imgui_impl_metal.mm
        imgui_impl_metal.h
    )

    target_compile_definitions(imguiImpl PRIVATE IMGUI_IMPL_METAL_CPP)
endif ()

if (ENABLE_D3D11)
	target_sources(imguiImpl PRIVATE
		../../dependencies/imgui/backends/imgui_impl_dx11.cpp
		../../dependencies/imgui/backends/imgui_impl_dx11.h
	)
	target_link_libraries(imguiImpl PRIVATE d3d11 d3dcompiler)
endif()

set_property(TARGET imguiImpl PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME_LIBRARY}")

cemu_use_precompiled_header(imguiImpl)

target_include_directories(imguiImpl PUBLIC "../")

# imgui source files
target_sources(imguiImpl PRIVATE
"../../dependencies/imgui/imgui.cpp"
"../../dependencies/imgui/imgui_draw.cpp"
"../../dependencies/imgui/imgui_tables.cpp"
"../../dependencies/imgui/imgui_widgets.cpp"
)

target_include_directories(imguiImpl PUBLIC "../../dependencies/imgui/")

target_link_libraries(imguiImpl PRIVATE
	CemuCommon
	CemuGui
)

# imgui_extension.cpp includes InputManager, whose SDL controller headers use
# SDL3 public types. CemuInput cannot be linked here without a cycle.
if(ENABLE_SDL)
	target_link_libraries(imguiImpl PRIVATE SDL3::SDL3)
endif()
