#include "Cafe/HW/Latte/Renderer/D3D11/D3D11Renderer.h"
#include "Cemu/Logging/CemuLogging.h"

#include <chrono>
#include <immintrin.h>
#include <thread>

namespace
{
bool IsRemovedResult(HRESULT result)
{
	return result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_REMOVED ||
		result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
}

bool D3D11Renderer::WaitForGpuIdle()
{
	if (m_deviceLost.load(std::memory_order_relaxed))
		return false;
	if (!m_gpuIdleQuery)
	{
		D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		const HRESULT createResult = m_device->CreateQuery(&desc, &m_gpuIdleQuery);
		if (FAILED(createResult))
		{
			cemuLog_log(LogType::Force,
				"D3D11: GPU-idle wait unavailable (CreateQuery HRESULT 0x{:08X})",
				static_cast<uint32>(createResult));
			return false;
		}
	}
	m_context->End(m_gpuIdleQuery.Get());
	m_context->Flush();
	uint32 spinCount = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		const HRESULT status = m_context->GetData(m_gpuIdleQuery.Get(), nullptr, 0,
			D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (status == S_OK)
			return true;
		if (status != S_FALSE)
		{
			if (IsRemovedResult(status))
				RecordDeviceLost(status, "GPU-idle wait");
			else
				cemuLog_log(LogType::Force,
					"D3D11: GPU-idle wait failed with HRESULT 0x{:08X}",
					static_cast<uint32>(status));
			return false;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			cemuLog_log(LogType::Force,
				"D3D11: GPU-idle wait timed out after 5 seconds; abandoning the wait");
			CheckDeviceHealth("GPU-idle timeout");
			return false;
		}
		_mm_pause();
		if ((++spinCount & 0x3FF) == 0)
			std::this_thread::yield();
	}
}
