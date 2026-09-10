#pragma once

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

// Keeps the mature Vulkan renderer's resource tracking, draw ordering and
// cache behavior. VulkanAPI redirects its vk* entry points to the in-process
// D3D12 translation layer before this object is constructed.
class D3D12Renderer final : public VulkanRenderer
{
public:
	D3D12Renderer() : VulkanRenderer(RendererAPI::D3D12) {}
};
