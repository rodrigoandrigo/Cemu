#include "interface/WindowSystem.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11Renderer.h"
#if defined(ENABLE_D3D12)
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/VulkanD3D12API.h"
#endif
#include "config/ActiveSettings.h"

namespace
{
WindowSystem::WindowInfo s_windowInfo{};

void SetSurface(WindowSystem::WindowHandleInfo& window, WindowSystem::WindowHandleInfo& canvas, void* hostWindow, void* hostCanvas)
{
	window.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
	window.surface = hostWindow;
	canvas.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
	canvas.surface = hostCanvas ? hostCanvas : hostWindow;
}
}

void WindowSystem::Create()
{
	// The embedded host owns the SwapChainPanel and the D3D11 presentation
	// objects. The renderer takes COM references to those objects.
	if (g_renderer)
		return;
#if defined(ENABLE_D3D12)
	if (ActiveSettings::GetGraphicsAPI() == GraphicAPI::kD3D12)
	{
		VulkanD3D12::SelectInternalDriver(true);
		if (!InitializeGlobalVulkan())
			throw std::runtime_error("Unable to initialize the internal Vulkan-to-D3D12 driver");
		g_renderer = std::make_unique<D3D12Renderer>();
	}
	else
#endif
		g_renderer = std::make_unique<D3D11Renderer>();
}

void WindowSystem::SetEmbeddedSurface(void* window, void* canvas, int width, int height, double dpiScale)
{
	SetSurface(s_windowInfo.window_main, s_windowInfo.canvas_main, window, canvas);
	ResizeEmbeddedSurface(width, height, dpiScale);
	s_windowInfo.app_active = true;
}

void WindowSystem::ResizeEmbeddedSurface(int width, int height, double dpiScale)
{
	s_windowInfo.width = std::max(width, 1);
	s_windowInfo.height = std::max(height, 1);
	s_windowInfo.phys_width = std::max(width, 1);
	s_windowInfo.phys_height = std::max(height, 1);
	s_windowInfo.dpi_scale = dpiScale > 0.0 ? dpiScale : 1.0;
}

void WindowSystem::SetEmbeddedPadSurface(void* window, void* canvas, int width, int height, double dpiScale)
{
	SetSurface(s_windowInfo.window_pad, s_windowInfo.canvas_pad, window, canvas);
	s_windowInfo.pad_width = std::max(width, 1);
	s_windowInfo.pad_height = std::max(height, 1);
	s_windowInfo.phys_pad_width = std::max(width, 1);
	s_windowInfo.phys_pad_height = std::max(height, 1);
	s_windowInfo.pad_dpi_scale = dpiScale > 0.0 ? dpiScale : 1.0;
	s_windowInfo.pad_open = window != nullptr;
}

void WindowSystem::ShowErrorDialog(std::string_view, std::string_view, std::optional<ErrorCategory>) {}
WindowSystem::WindowInfo& WindowSystem::GetWindowInfo() { return s_windowInfo; }
void WindowSystem::UpdateWindowTitles(bool, bool, double) {}
void WindowSystem::GetWindowSize(int& width, int& height) { width = s_windowInfo.width; height = s_windowInfo.height; }
void WindowSystem::GetPadWindowSize(int& width, int& height) { width = s_windowInfo.pad_open ? s_windowInfo.pad_width.load() : 0; height = s_windowInfo.pad_open ? s_windowInfo.pad_height.load() : 0; }
void WindowSystem::GetWindowPhysSize(int& width, int& height) { width = s_windowInfo.phys_width; height = s_windowInfo.phys_height; }
void WindowSystem::GetPadWindowPhysSize(int& width, int& height) { width = s_windowInfo.pad_open ? s_windowInfo.phys_pad_width.load() : 0; height = s_windowInfo.pad_open ? s_windowInfo.phys_pad_height.load() : 0; }
double WindowSystem::GetWindowDPIScale() { return s_windowInfo.dpi_scale; }
double WindowSystem::GetPadDPIScale() { return s_windowInfo.pad_open ? s_windowInfo.pad_dpi_scale.load() : 1.0; }
bool WindowSystem::IsPadWindowOpen() { return s_windowInfo.pad_open; }
bool WindowSystem::IsKeyDown(uint32 key) { return s_windowInfo.get_keystate(key); }
bool WindowSystem::IsKeyDown(PlatformKeyCodes) { return false; }
std::string WindowSystem::GetKeyCodeName(uint32 key) { return fmt::format("key_{}", key); }
bool WindowSystem::InputConfigWindowHasFocus() { return false; }
void WindowSystem::NotifyGameLoaded() {}
void WindowSystem::NotifyGameExited() {}
void WindowSystem::RefreshGameList() {}
void WindowSystem::CaptureInput(const ControllerState&, const ControllerState&) {}
bool WindowSystem::IsFullScreen() { return s_windowInfo.is_fullscreen; }
