#include "Cafe/HW/Latte/Renderer/D3D11/D3D11Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/CemuRuntime.h"

#include <fmt/format.h>

bool D3D11Renderer::IsDeviceLostResult(HRESULT result)
{
	return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
		result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void D3D11Renderer::RecordDeviceLost(HRESULT result, const char* operation)
{
	bool expected = false;
	if (!m_deviceLost.compare_exchange_strong(expected, true, std::memory_order_relaxed))
		return;
	const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : result;
	cemuLog_log(LogType::Force,
		"D3D11 Series S device removed during {} (HRESULT 0x{:08X}, reason 0x{:08X}); "
		"stopping further driver calls; active shaders VS {:016x}_{:016x}, "
		"PS {:016x}_{:016x}, GS {:016x}_{:016x}",
		operation ? operation : "GPU operation", static_cast<uint32>(result),
		static_cast<uint32>(FAILED(reason) ? reason : result),
		m_lastVertexShaderBase, m_lastVertexShaderAux,
		m_lastPixelShaderBase, m_lastPixelShaderAux,
		m_lastGeometryShaderBase, m_lastGeometryShaderAux);
	CemuRuntime::RecordGraphicsDeviceLost(fmt::format(
		"Direct3D 11 device lost (0x{:08X}). The renderer was stopped so the host can recreate the device.",
		static_cast<uint32>(FAILED(reason) ? reason : result)));
	Latte_RequestStopFromGPU();
}

bool D3D11Renderer::CheckDeviceHealth(const char* operation)
{
	if (m_deviceLost.load(std::memory_order_relaxed))
		return false;
	const HRESULT reason = m_device->GetDeviceRemovedReason();
	if (SUCCEEDED(reason))
		return true;
	RecordDeviceLost(reason, operation);
	return false;
}
