void D3D11Renderer::bufferCache_init(const sint32 size)
{
	m_bufferCacheShadow.resize((std::max)(size, 16));
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(m_bufferCacheShadow.size());
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
	ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &m_bufferCache), "Create buffer cache");
	desc.ByteWidth = LatteStreamout_GetRingBufferSize();
	// Xbox FL 11.0 uses the same raw ring buffer as the normal shader-storage
	// path, but populates it from a pixel-UAV replay rather than VS/GS UAV stores.
	// Keep this resource independent from UseTFViaSSBO(): the latter deliberately
	// remains false on FL 11.0 so the decompiler emits the reflected XFB values
	// consumed by the replay geometry shader.
	const bool needsStreamoutStorageBuffer =
#if defined(CEMU_UWP)
		true;
#else
		UseTFViaSSBO();
#endif
	if (needsStreamoutStorageBuffer)
	{
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &m_streamoutStorageBuffer),
			"Create shader stream-output ring buffer");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = desc.ByteWidth / sizeof(uint32);
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		ThrowIfFailed(m_device->CreateUnorderedAccessView(m_streamoutStorageBuffer.Get(),
			&uavDesc, &m_streamoutStorageUav), "Create shader stream-output UAV");
	}
	else
	{
		desc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
		desc.MiscFlags = 0;
		for (auto& streamoutBuffer : m_streamoutBuffers)
			ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &streamoutBuffer),
				"Create stream-output ring buffer");
	}
#if defined(CEMU_UWP)
	D3D11_TEXTURE2D_DESC captureTargetDesc{};
	captureTargetDesc.Width = StreamoutPixelCaptureWidth;
	captureTargetDesc.Height = StreamoutPixelCaptureHeight;
	captureTargetDesc.MipLevels = 1;
	captureTargetDesc.ArraySize = 1;
	captureTargetDesc.Format = DXGI_FORMAT_R8_UNORM;
	captureTargetDesc.SampleDesc.Count = 1;
	captureTargetDesc.Usage = D3D11_USAGE_DEFAULT;
	captureTargetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ThrowIfFailed(m_device->CreateTexture2D(&captureTargetDesc, nullptr, &m_streamoutCaptureTarget),
		"Create Xbox stream-output capture target");
	ThrowIfFailed(m_device->CreateRenderTargetView(m_streamoutCaptureTarget.Get(), nullptr,
		&m_streamoutCaptureTargetView), "Create Xbox stream-output capture target view");
	D3D11_DEPTH_STENCIL_DESC captureDepthDesc{};
	captureDepthDesc.DepthEnable = FALSE;
	captureDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	captureDepthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	captureDepthDesc.StencilEnable = FALSE;
	ThrowIfFailed(m_device->CreateDepthStencilState(&captureDepthDesc, &m_streamoutCaptureDepthState),
		"Create Xbox stream-output capture depth state");
	D3D11_BLEND_DESC captureBlendDesc{};
	captureBlendDesc.RenderTarget[0].BlendEnable = FALSE;
	captureBlendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
	ThrowIfFailed(m_device->CreateBlendState(&captureBlendDesc, &m_streamoutCaptureBlendState),
		"Create Xbox stream-output capture blend state");
#endif
	for (auto& buffer : m_vertexBuffers)
		buffer = m_bufferCache;
}

void D3D11Renderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 offset)
{
	if (!m_bufferCache || !buffer || size <= 0 ||
		static_cast<uint64>(offset) + static_cast<uint32>(size) > m_bufferCacheShadow.size())
		return;
	std::memcpy(m_bufferCacheShadow.data() + offset, buffer, size);
	D3D11_BOX box{ offset, 0, 0, offset + static_cast<UINT>(size), 1, 1 };
	{
		D3D11_DRIVER_TRACE(fmt::format(
			"UpdateSubresource buffer-cache offset={} size={} source={}",
			offset, size, static_cast<const void*>(buffer)));
		m_context->UpdateSubresource(m_bufferCache.Get(), 0, &box, buffer, 0, 0);
	}
}

void D3D11Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	if (!m_bufferCache || !size ||
		static_cast<uint64>(srcOffset) + size > m_bufferCacheShadow.size() ||
		static_cast<uint64>(dstOffset) + size > m_bufferCacheShadow.size())
		return;
	if (!m_bufferCopyScratch || m_bufferCopyScratchCapacity < size)
	{
		UINT newCapacity = m_bufferCopyScratchCapacity ? m_bufferCopyScratchCapacity : 256u * 1024u;
		while (newCapacity < size && newCapacity <= (std::numeric_limits<UINT>::max)() / 2)
			newCapacity *= 2;
		newCapacity = (std::max)(newCapacity, size);
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = newCapacity;
		desc.Usage = D3D11_USAGE_DEFAULT;
		ComPtr<ID3D11Buffer> replacement;
		const HRESULT hr = m_device->CreateBuffer(&desc, nullptr, &replacement);
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force,
				"D3D11 buffer-cache GPU copy skipped: scratch allocation {} bytes failed (0x{:08X})",
				newCapacity, static_cast<uint32>(hr));
			return;
		}
		m_bufferCopyScratch = std::move(replacement);
		m_bufferCopyScratchCapacity = newCapacity;
	}

	// A cache node may contain transform-feedback data that exists only on the
	// GPU. The old CPU-shadow UpdateSubresource path silently replaced those
	// bytes with stale emulated RAM while ranges were merged. D3D11 buffers have
	// one subresource, so use a distinct reusable scratch resource for the two
	// asynchronous GPU copies required by the API.
	D3D11_BOX sourceBox{ srcOffset, 0, 0, srcOffset + size, 1, 1 };
	m_context->CopySubresourceRegion(m_bufferCopyScratch.Get(), 0, 0, 0, 0,
		m_bufferCache.Get(), 0, &sourceBox);
	D3D11_BOX scratchBox{ 0, 0, 0, size, 1, 1 };
	m_context->CopySubresourceRegion(m_bufferCache.Get(), 0, dstOffset, 0, 0,
		m_bufferCopyScratch.Get(), 0, &scratchBox);

	// Keep the CPU mirror coherent for ordinary RAM-backed pages and uniform
	// ranges. Stream-output-only bytes remain authoritative on the GPU, but are
	// no longer uploaded back over the correct destination.
	std::memmove(m_bufferCacheShadow.data() + dstOffset, m_bufferCacheShadow.data() + srcOffset, size);
	D3D11_DRIVER_TRACE(fmt::format(
		"GPU buffer-copy src={} dst={} size={}", srcOffset, dstOffset, size));
}
void D3D11Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 src, uint32 dst, uint32 size)
{
	if (!m_bufferCache || !size ||
		static_cast<uint64>(src) + size > static_cast<uint32>(LatteStreamout_GetRingBufferSize()) ||
		static_cast<uint64>(dst) + size > m_bufferCacheShadow.size())
		return;
	if (m_streamoutActive)
	{
		if (m_streamoutUsesStorage)
		{
			ID3D11UnorderedAccessView* empty{};
			m_context->OMSetRenderTargetsAndUnorderedAccessViews(
				D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
				StreamoutUavSlot, 1, &empty, nullptr);
		}
		else if (!m_streamoutUsesPixelCapture)
		{
			std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> empty{};
			std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> offsets{};
			m_context->SOSetTargets(static_cast<UINT>(empty.size()), empty.data(), offsets.data());
		}
		m_streamoutActive = false;
	}
	ID3D11Buffer* sourceBuffer = m_streamoutDataAvailable &&
		(m_streamoutUsesStorage || m_streamoutUsesPixelCapture) ? m_streamoutStorageBuffer.Get() : nullptr;
	for (UINT i = 0; !sourceBuffer && i < m_streamoutBuffers.size(); ++i)
	{
		if (m_streamoutEnabled[i] && m_streamoutOffsets[i] == src)
		{
			sourceBuffer = m_streamoutBuffers[i].Get();
			break;
		}
	}
	if (!sourceBuffer)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 stream-output source unavailable; clearing destination offset {} size {}",
			src, size);
		// Never leave an older transform-feedback result in the vertex cache. A
		// later draw interprets those stale bytes as positions and produces the
		// characteristic screen-spanning triangles. Zeroing is a deterministic
		// degradation if the Xbox driver rejects an individual SO signature.
		static constexpr size_t zeroChunkSize = 64 * 1024;
		static const std::array<uint8, zeroChunkSize> zeroChunk{};
		uint32 cleared{};
		while (cleared < size)
		{
			const uint32 chunk = (std::min)(size - cleared,
				static_cast<uint32>(zeroChunk.size()));
			D3D11_BOX destinationBox{ dst + cleared, 0, 0, dst + cleared + chunk, 1, 1 };
			m_context->UpdateSubresource(m_bufferCache.Get(), 0, &destinationBox,
				zeroChunk.data(), 0, 0);
			cleared += chunk;
		}
		std::memset(m_bufferCacheShadow.data() + dst, 0, size);
		return;
	}
	D3D11_BOX box{ src, 0, 0, src + size, 1, 1 };
	m_context->CopySubresourceRegion(m_bufferCache.Get(), 0, dst, 0, 0,
		sourceBuffer, 0, &box);
}

void D3D11Renderer::buffer_bindVertexBuffer(uint32 index, uint32 offset, uint32 size)
{
	if (index >= m_vertexBuffers.size())
		return;
	const bool validRange = size != 0 &&
		static_cast<uint64>(offset) + size <= m_bufferCacheShadow.size();
	m_vertexBuffers[index] = validRange ? m_bufferCache : nullptr;
	m_vertexOffsets[index] = offset;
	const uint32* regs = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START + index * 7;
	m_vertexStrides[index] = (regs[2] >> 11) & 0xFFFF;
	ID3D11Buffer* buffer = m_vertexBuffers[index].Get();
	m_context->IASetVertexBuffers(index, 1, &buffer, &m_vertexStrides[index], &m_vertexOffsets[index]);
}

void D3D11Renderer::buffer_bindUniformBuffer(LatteConst::ShaderType stage, uint32 index, uint32 offset, uint32 size)
{
	if (index >= LATTE_NUM_MAX_UNIFORM_BUFFERS)
		return;
	const uint32 stageIndex = static_cast<uint32>(stage);
	ID3D11Buffer* native{};
	if (size && static_cast<uint64>(offset) + size <= m_bufferCacheShadow.size())
	{
		const UINT requiredSize = Align16(size);
		auto& buffer = m_uniformBuffers[stageIndex][index];
		auto& capacity = m_uniformBufferCapacity[stageIndex][index];
		if (!buffer || capacity < requiredSize)
		{
			// Grow geometrically and reuse the allocation. Recreating a DEFAULT
			// buffer on every bind leaves many driver allocations in flight and
			// quickly exhausts the shared Xbox memory budget.
			UINT newCapacity = 256;
			while (newCapacity < requiredSize &&
				newCapacity <= (std::numeric_limits<UINT>::max() / 2))
				newCapacity *= 2;
			newCapacity = (std::max)(newCapacity, requiredSize);

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = newCapacity;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			ComPtr<ID3D11Buffer> replacement;
			HRESULT hr = m_device->CreateBuffer(&desc, nullptr, &replacement);
			if (IsMemoryPressureResult(hr))
			{
				// Do not evict the decoded index selected for this same draw: the
				// local IndexAllocation still references it until DrawIndexed below.
				buffer.Reset();
				capacity = 0;
				RecoverFromMemoryPressure("uniform buffer", false);
				if (m_deviceLost.load(std::memory_order_relaxed))
					return;
				hr = m_device->CreateBuffer(&desc, nullptr, &replacement);
			}
			if (SUCCEEDED(hr))
			{
				buffer = std::move(replacement);
				capacity = newCapacity;
			}
		}

		if (buffer && capacity >= requiredSize)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(m_context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				std::memcpy(mapped.pData, m_bufferCacheShadow.data() + offset, size);
				if (capacity > size)
					std::memset(static_cast<uint8*>(mapped.pData) + size, 0, capacity - size);
				m_context->Unmap(buffer.Get(), 0);
				native = buffer.Get();
			}
		}
	}
	// Keep an unused slot allocated. The next bind can reuse it without asking
	// the Xbox driver for another resource; native remains null for this bind.
	LatteDecompilerShader* shader{};
	if (stage == LatteConst::ShaderType::Vertex) shader = LatteSHRC_GetActiveVertexShader();
	else if (stage == LatteConst::ShaderType::Pixel) shader = LatteSHRC_GetActivePixelShader();
	else if (stage == LatteConst::ShaderType::Geometry) shader = LatteSHRC_GetActiveGeometryShader();
	auto* nativeShader = shader ? static_cast<D3D11Shader*>(shader->shader) : nullptr;
	if (!nativeShader)
		return;
	const sint32 originalBinding = shader->resourceMapping.uniformBuffersBindingPoint[index];
	const UINT binding = originalBinding >= 0 ?
		nativeShader->UniformSlot(static_cast<UINT>(originalBinding)) : D3D11Shader::InvalidSlot;
	if (binding == D3D11Shader::InvalidSlot)
		return;
	if (stage == LatteConst::ShaderType::Vertex) m_context->VSSetConstantBuffers(binding, 1, &native);
	else if (stage == LatteConst::ShaderType::Pixel) m_context->PSSetConstantBuffers(binding, 1, &native);
	else if (stage == LatteConst::ShaderType::Geometry) m_context->GSSetConstantBuffers(binding, 1, &native);
}

RendererShader* D3D11Renderer::shader_create(RendererShader::ShaderType type, uint64 baseHash,
	uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackSource)
{
	// Only entries restored from the transferable cache can be deferred. A
	// shader discovered while the title is running must be available now; using
	// the isGameShader argument for this distinction would also defer normal
	// runtime game shaders.
	const bool compileAsync = isGameShader && LatteShaderCache_IsLoading();
	if (m_deviceLost.load(std::memory_order_relaxed))
		return nullptr;
	const uint64 failureKey = ShaderFailureKey(type, baseHash, auxHash);
	{
		std::lock_guard lock(m_failedShaderMutex);
		if (m_failedShaderKeys.contains(failureKey))
			return nullptr;
	}
#if defined(CEMU_UWP)
	// The desktop cache benefits from parallel compilation, but Xbox compiler and
	// translation workers share one strict process budget. Serialize the complete
	// pipeline so glslang, SPIRV-Cross and xbsc_xs.dll cannot overlap across jobs.
	// Cached shaders are admitted to the low-priority queue immediately; the
	// queue takes the same lock when it performs the actual translation.
	if (!compileAsync)
	{
		if (m_deviceLost.load(std::memory_order_relaxed))
			return nullptr;
		constexpr uint64 resumeCompileMB = 3840;
		constexpr uint64 stopCompileMB = 4096;
		uint64 commitMB = QueryProcessCommitBytes() / (1024 * 1024);
		if (m_shaderCompilationBlocked.load(std::memory_order_relaxed))
		{
			if (commitMB < resumeCompileMB)
				m_shaderCompilationBlocked.store(false, std::memory_order_relaxed);
			else
				return nullptr;
		}
		if (commitMB >= stopCompileMB)
		{
			cemuLog_log(LogType::Force,
				"D3D11 Series S shader compilation paused at {} MB process commit "
				"after {} compiled shaders",
				commitMB, m_compiledShaderCount.load(std::memory_order_relaxed));
			m_shaderCompilationBlocked.store(true, std::memory_order_relaxed);
			return nullptr;
		}
	}
#endif
	try
	{
		auto shader = std::make_unique<D3D11Shader>(m_device.Get(), type, baseHash,
			auxHash, isGameShader, isGfxPackSource, source, compileAsync);
		if (compileAsync)
			return shader.release();
		// Xbox may report a shader creation failure as a native xbsc exception and
		// remove the underlying D3D12 device before the D3D11 call returns. Latch
		// that state immediately on the compilation thread so no other worker keeps
		// feeding the removed device until the next Present/GPU-idle check.
		if (!CheckDeviceHealth("shader creation"))
		{
			std::lock_guard lock(m_failedShaderMutex);
			m_failedShaderKeys.insert(failureKey);
			return nullptr;
		}
		if (!shader->IsCompiled())
		{
			std::lock_guard lock(m_failedShaderMutex);
			m_failedShaderKeys.insert(failureKey);
			return nullptr;
		}
		m_compiledShaderCount.fetch_add(1, std::memory_order_relaxed);
		return shader.release();
	}
	catch (const std::bad_alloc&)
	{
#if defined(CEMU_UWP)
		HeapCompact(GetProcessHeap(), 0);
		m_shaderCompilationBlocked.store(true, std::memory_order_relaxed);
#endif
		OutputDebugStringA("[Cemu/D3D11] Shader creation skipped: out of memory\n");
		return nullptr;
	}
	catch (const std::exception& ex)
	{
		{
			std::lock_guard lock(m_failedShaderMutex);
			m_failedShaderKeys.insert(failureKey);
		}
		cemuLog_log(LogType::Force,
			"D3D11 shader {:016x}_{:016x} quarantined after creation failure: {}",
			baseHash, auxHash, ex.what());
		CheckDeviceHealth("shader exception");
		return nullptr;
	}
	catch (...)
	{
		{
			std::lock_guard lock(m_failedShaderMutex);
			m_failedShaderKeys.insert(failureKey);
		}
		OutputDebugStringA("[Cemu/D3D11] Shader quarantined after Xbox compiler exception\n");
		CheckDeviceHealth("Xbox shader compiler exception");
		return nullptr;
	}
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
