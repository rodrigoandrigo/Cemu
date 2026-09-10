#pragma once

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace VulkanD3D12
{
	void SelectInternalDriver(bool selected);
	bool IsInternalDriverSelected();

	// These entry points install the internal vk* dispatch table. They are kept
	// separate by Vulkan dispatch scope so the existing renderer does not need
	// backend-specific branches in its render logic.
	bool InitializeGlobalDispatch();
	bool InitializeInstanceDispatch(VkInstance instance);
	bool InitializeDeviceDispatch(VkDevice device);
}
