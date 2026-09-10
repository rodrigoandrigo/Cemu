void D3D11Renderer::streamout_setupXfbBuffer(uint32 index, sint32 ringBufferOffset, uint32, uint32 rangeSize)
{
	if (index >= LATTE_NUM_STREAMOUT_BUFFER)
		return;
	const uint64 offset = static_cast<uint64>((std::max)(ringBufferOffset, 0));
	const bool validRange = ringBufferOffset >= 0 && rangeSize != 0 &&
		offset + rangeSize <= static_cast<uint32>(LatteStreamout_GetRingBufferSize());
	m_streamoutEnabled[index] = validRange;
	m_streamoutOffsets[index] = validRange ? static_cast<UINT>(offset) : 0;
	m_streamoutRangeSizes[index] = validRange ? rangeSize : 0;
	if (rangeSize != 0 && !validRange)
		cemuLog_log(LogType::Force,
			"D3D11 stream-output buffer {} rejected invalid range offset={} size={}",
			index, ringBufferOffset, rangeSize);
}

void D3D11Renderer::streamout_begin()
{
	m_streamoutUsesStorage = false;
	m_streamoutUsesPixelCapture = false;
	m_streamoutNativeRasterized = false;
	m_streamoutDataAvailable = false;
	m_streamoutPixelCaptureShader = nullptr;
	auto* gsContext = LatteSHRC_GetActiveGeometryShader();
	auto* vsContext = LatteSHRC_GetActiveVertexShader();
	auto* shader = gsContext ? static_cast<D3D11Shader*>(gsContext->shader) :
		(vsContext ? static_cast<D3D11Shader*>(vsContext->shader) : nullptr);
	if (shader)
		shader->PreponeCompilation(true);
	const bool hasOutputBuffer = std::any_of(m_streamoutEnabled.begin(),
		m_streamoutEnabled.end(), [](bool enabled) { return enabled; });
	// A real title GS must remain the stage that emits the stream. Feature Level
	// 11.0 supports native geometry stream output, and the scalar declaration built
	// from reflection preserves the exact GX2 buffer strides.
	if (gsContext && shader && shader->StreamoutGeometry() && hasOutputBuffer &&
		EnsureNativeStreamoutBuffers())
	{
		const bool rasterizerKilled =
			LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() &&
			LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
		const bool bothFacesCulled =
			LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_FRONT() &&
			LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_BACK();
		const bool wantsRaster = !rasterizerKilled && !bothFacesCulled;
		ID3D11GeometryShader* streamoutGeometry = shader->StreamoutGeometry(wantsRaster);
		std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> buffers{};
		bool hasNativeOutputBuffer = false;
		for (UINT i = 0; i < buffers.size(); ++i)
		{
			buffers[i] = m_streamoutEnabled[i] ? m_streamoutBuffers[i].Get() : nullptr;
			hasNativeOutputBuffer |= buffers[i] != nullptr;
		}
		if (hasNativeOutputBuffer)
		{
			m_streamoutNativeRasterized = wantsRaster &&
				shader->HasRasterizedStreamoutGeometry();
			m_context->GSSetShader(streamoutGeometry, nullptr, 0);
			m_context->SOSetTargets(static_cast<UINT>(buffers.size()), buffers.data(),
				m_streamoutOffsets.data());
			m_streamoutDataAvailable = true;
			m_streamoutActive = true;
			return;
		}
	}
	// Prefer native stream output for a real geometry stage. Besides capturing the
	// actual emitted vertices, it avoids the all-stage UAV slot used by the shader
	// storage path, which is not portable at Feature Level 11.0.
	if (!gsContext && shader && shader->UsesStreamoutStorage() && hasOutputBuffer &&
		m_streamoutStorageUav)
	{
		ID3D11UnorderedAccessView* uav = m_streamoutStorageUav.Get();
		m_context->OMSetRenderTargetsAndUnorderedAccessViews(
			D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
			StreamoutUavSlot, 1, &uav, nullptr);
		m_streamoutUsesStorage = true;
		m_streamoutDataAvailable = true;
		m_streamoutActive = true;
		return;
	}
#if defined(CEMU_UWP)
	if (!gsContext && shader && shader->HasPixelStreamoutCapture() && hasOutputBuffer &&
		m_streamoutStorageBuffer && m_streamoutStorageUav && m_streamoutCaptureTargetView)
	{
		// FL 11.0 has no vertex/geometry UAV stores. The replay runs after the
		// title draw, uses the original VS with point topology and writes its
		// reflected XFB scalars from a pixel shader into this raw ring buffer.
		m_streamoutUsesPixelCapture = true;
		m_streamoutPixelCaptureShader = shader;
		m_streamoutActive = true;
		return;
	}
	if (gsContext)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 Xbox could not create the reflected Feature Level 11.0 geometry stream-output shader");
	}
	else if (!shader || !shader->HasPixelStreamoutCapture())
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 Xbox stream-output capture shader unavailable; native stream output remains disabled");
	}
	m_streamoutEnabled.fill(false);
	m_streamoutRangeSizes.fill(0);
	m_streamoutActive = false;
	return;
#endif
	if (!shader || !shader->StreamoutGeometry())
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 stream output was requested but its shader signature is unavailable");
		// FinishDrawcall still runs in the common path. Clear these bindings so it
		// cannot copy bytes left by an older transform-feedback draw into the new
		// vertex range.
		m_streamoutEnabled.fill(false);
		m_streamoutActive = false;
		return;
	}
	std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> buffers{};
	bool hasNativeOutputBuffer = false;
	for (UINT i = 0; i < buffers.size(); ++i)
	{
		buffers[i] = m_streamoutEnabled[i] ? m_streamoutBuffers[i].Get() : nullptr;
		hasNativeOutputBuffer |= buffers[i] != nullptr;
	}
	if (!hasNativeOutputBuffer)
	{
		m_streamoutEnabled.fill(false);
		m_streamoutActive = false;
		return;
	}
	m_context->GSSetShader(shader->StreamoutGeometry(), nullptr, 0);
	m_context->SOSetTargets(static_cast<UINT>(buffers.size()), buffers.data(), m_streamoutOffsets.data());
	m_streamoutActive = true;
}

void D3D11Renderer::streamout_rendererFinishDrawcall()
{
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
	m_streamoutUsesStorage = false;
	m_streamoutUsesPixelCapture = false;
	m_streamoutNativeRasterized = false;
	m_streamoutDataAvailable = false;
	m_streamoutPixelCaptureShader = nullptr;
	m_streamoutEnabled.fill(false);
	m_streamoutRangeSizes.fill(0);
	// Restore the title's GS or the RECT emulation GS. Merely restoring the
	// title GS loses rectangle emulation on subsequent draws in the sequence.
	BindActiveShaders();
}

bool D3D11Renderer::ExecutePixelStreamoutCapture(uint32 baseVertex, uint32 baseInstance,
	uint32 instanceCount, uint32 vertexCount, uint32 indexCount, Renderer::INDEX_TYPE indexType,
	ID3D11Buffer* indexBuffer, UINT indexOffset, const uint8* decodedIndices, UINT decodedIndexBytes)
{
#if !defined(CEMU_UWP)
	return false;
#else
	const bool indexed = indexType != INDEX_TYPE::NONE;
	const UINT indexStride = indexType == INDEX_TYPE::U16 ? sizeof(uint16) :
		indexType == INDEX_TYPE::U32 ? sizeof(uint32) : 0;
	auto* captureShader = static_cast<D3D11Shader*>(m_streamoutPixelCaptureShader);
	if (!captureShader || !m_streamoutStorageUav ||
		!m_streamoutCaptureTargetView || !m_streamoutCaptureDepthState ||
		!m_streamoutCaptureBlendState || !captureShader->PixelStreamoutCaptureVertex() ||
		!vertexCount || !instanceCount ||
		(indexed && (!indexBuffer || !decodedIndices || !indexStride ||
			indexCount > (std::numeric_limits<UINT>::max)() / indexStride ||
			decodedIndexBytes < indexCount * indexStride)))
		return false;

	struct CaptureConstants
	{
		uint32 recordState[4]{};
		uint32 ringBase[4]{};
		uint32 bufferLimit[4]{};
	};
	CaptureConstants constants{};
	uint32 recordLimit{};
	for (UINT buffer = 0; buffer < LATTE_NUM_STREAMOUT_BUFFER; ++buffer)
	{
		if (!m_streamoutEnabled[buffer])
			continue;
		const UINT stride = captureShader->PixelStreamoutCaptureStride(buffer);
		if (!stride)
		{
			cemuLog_logOnce(LogType::Force,
				"D3D11 Xbox pixel stream-output replay has no reflected stride for buffer {}", buffer);
			return false;
		}
		constants.ringBase[buffer] = m_streamoutOffsets[buffer] / sizeof(uint32);
		constants.bufferLimit[buffer] = (std::min)(m_streamoutRangeSizes[buffer] / stride,
			StreamoutPixelCaptureCapacity);
		recordLimit = (std::max)(recordLimit, constants.bufferLimit[buffer]);
	}
	if (!recordLimit)
		return true;

	ID3D11ShaderResourceView* recordMap{};
	if (indexed)
	{
		if (!m_streamoutCaptureRecordMap || indexCount > m_streamoutCaptureRecordMapCapacity)
		{
			UINT newCapacity = m_streamoutCaptureRecordMapCapacity ?
				m_streamoutCaptureRecordMapCapacity : 256u;
			while (newCapacity < indexCount &&
				newCapacity <= (std::numeric_limits<UINT>::max)() / 2u)
				newCapacity *= 2u;
			newCapacity = (std::max)(newCapacity, indexCount);
			if (newCapacity > (std::numeric_limits<UINT>::max)() / sizeof(uint32))
				return false;

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = newCapacity * sizeof(uint32);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(uint32);
			ComPtr<ID3D11Buffer> buffer;
			if (FAILED(m_device->CreateBuffer(&desc, nullptr, &buffer)))
				return false;

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			viewDesc.Format = DXGI_FORMAT_UNKNOWN;
			viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			viewDesc.Buffer.FirstElement = 0;
			viewDesc.Buffer.NumElements = newCapacity;
			ComPtr<ID3D11ShaderResourceView> view;
			if (FAILED(m_device->CreateShaderResourceView(buffer.Get(), &viewDesc, &view)))
				return false;
			m_streamoutCaptureRecordMap = std::move(buffer);
			m_streamoutCaptureRecordMapView = std::move(view);
			m_streamoutCaptureRecordMapCapacity = newCapacity;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(m_context->Map(m_streamoutCaptureRecordMap.Get(), 0,
			D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		auto* destination = static_cast<uint32*>(mapped.pData);
		for (UINT index = 0; index < indexCount; ++index)
		{
			uint32 value{};
			std::memcpy(&value, decodedIndices + static_cast<size_t>(index) * indexStride,
				indexStride);
			if (indexType == INDEX_TYPE::U16)
				value &= 0xFFFFu;
			destination[index] = value <= (std::numeric_limits<uint32>::max)() - baseVertex ?
				value + baseVertex : (std::numeric_limits<uint32>::max)();
		}
		m_context->Unmap(m_streamoutCaptureRecordMap.Get(), 0);
		recordMap = m_streamoutCaptureRecordMapView.Get();
	}

	ID3D11RenderTargetView* target = m_streamoutCaptureTargetView.Get();
	ID3D11UnorderedAccessView* streamoutUav = m_streamoutStorageUav.Get();
	if (m_context1 && target)
		m_context1->DiscardView(target);
	m_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr,
		StreamoutPixelCaptureUavSlot, 1, &streamoutUav, nullptr);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f,
		static_cast<float>(StreamoutPixelCaptureWidth), static_cast<float>(StreamoutPixelCaptureHeight), 0.0f, 1.0f };
	const D3D11_RECT scissor{ 0, 0, static_cast<LONG>(StreamoutPixelCaptureWidth),
		static_cast<LONG>(StreamoutPixelCaptureHeight) };
	m_context->RSSetViewports(1, &viewport);
	m_context->RSSetScissorRects(1, &scissor);
	const float blendFactor[4]{};
	m_context->OMSetBlendState(m_streamoutCaptureBlendState.Get(), blendFactor, 0xFFFFFFFF);
	m_context->OMSetDepthStencilState(m_streamoutCaptureDepthState.Get(), 0);
	m_context->VSSetShader(captureShader->PixelStreamoutCaptureVertex(), nullptr, 0);
	m_context->IASetInputLayout(m_inputLayout.Get());
	m_context->GSSetShaderResources(0, 1, &recordMap);
	if (indexed)
		m_context->IASetIndexBuffer(indexBuffer,
			indexType == INDEX_TYPE::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
			indexOffset);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

	bool captured = true;
	const UINT passCount = captureShader->PixelStreamoutCapturePassCount();
	if (m_context1)
	{
		// A D3D11 constant buffer is limited to 4096 16-byte constants. Each
		// instance consumes three. Upload a whole batch once, then select the
		// appropriate three-constant range for every draw with the D3D11.1 API.
		// D3D11.1 requires subranges to start on a 16-constant (256-byte)
		// boundary when the runtime has to emulate range binding.
		constexpr UINT constantsPerInstance = 16;
		struct CaptureConstantSlot
		{
			CaptureConstants value{};
			std::array<uint8, constantsPerInstance * 16 - sizeof(CaptureConstants)> padding{};
		};
		static_assert(sizeof(CaptureConstantSlot) == constantsPerInstance * 16);
		constexpr UINT instancesPerBatch = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT /
			constantsPerInstance;
		std::vector<CaptureConstantSlot> instanceConstants;
		instanceConstants.reserve((std::min)(instanceCount, instancesPerBatch));
		for (UINT batchStart = 0; captured && batchStart < instanceCount;
			batchStart += instancesPerBatch)
		{
			const UINT batchCount = (std::min)(instanceCount - batchStart, instancesPerBatch);
			instanceConstants.clear();
			for (UINT local = 0; local < batchCount; ++local)
			{
				const uint64 recordBase = static_cast<uint64>(batchStart + local) * vertexCount +
					(indexed ? 0u : baseVertex);
				if (recordBase >= recordLimit || recordBase >= StreamoutPixelCaptureCapacity)
					break;
				CaptureConstantSlot current{};
				current.value = constants;
				current.value.recordState[0] = static_cast<uint32>(recordBase);
				current.value.recordState[1] = recordLimit;
				current.value.recordState[2] = indexed ? 1u : 0u;
				instanceConstants.emplace_back(current);
			}
			if (instanceConstants.empty())
				break;
			if (!UpdateDynamicConstantBuffer(m_streamoutCaptureConstants,
				m_streamoutCaptureConstantsCapacity, instanceConstants.data(),
				static_cast<UINT>(instanceConstants.size() * sizeof(CaptureConstantSlot))))
			{
				captured = false;
				break;
			}
			ID3D11Buffer* constantsBuffer = m_streamoutCaptureConstants.Get();
			for (UINT pass = 0; captured && pass < passCount; ++pass)
			{
				auto* geometry = captureShader->PixelStreamoutCaptureGeometry(pass);
				auto* pixel = captureShader->PixelStreamoutCapturePixel(pass);
				if (!geometry || !pixel)
				{
					captured = false;
					break;
				}
				m_context->GSSetShader(geometry, nullptr, 0);
				m_context->PSSetShader(pixel, nullptr, 0);
				for (UINT local = 0; local < instanceConstants.size(); ++local)
				{
					const UINT firstConstant = local * constantsPerInstance;
					const UINT constantCount = constantsPerInstance;
					m_context1->GSSetConstantBuffers1(0, 1, &constantsBuffer,
						&firstConstant, &constantCount);
					m_context1->PSSetConstantBuffers1(0, 1, &constantsBuffer,
						&firstConstant, &constantCount);
					const UINT instance = batchStart + local;
					if (!indexed)
						m_context->DrawInstanced(vertexCount, 1, baseVertex, baseInstance + instance);
					else
						m_context->DrawIndexedInstanced(indexCount, 1, 0, baseVertex,
							baseInstance + instance);
				}
			}
		}
	}
	else
	{
		// Desktop systems without ID3D11DeviceContext1 retain the portable path.
		for (UINT pass = 0; captured && pass < passCount; ++pass)
		{
			auto* geometry = captureShader->PixelStreamoutCaptureGeometry(pass);
			auto* pixel = captureShader->PixelStreamoutCapturePixel(pass);
			if (!geometry || !pixel)
			{
				captured = false;
				break;
			}
			m_context->GSSetShader(geometry, nullptr, 0);
			m_context->PSSetShader(pixel, nullptr, 0);
			for (uint32 instance = 0; instance < instanceCount; ++instance)
			{
				const uint64 recordBase = static_cast<uint64>(instance) * vertexCount +
					(indexed ? 0u : baseVertex);
				if (recordBase >= recordLimit || recordBase >= StreamoutPixelCaptureCapacity)
					break;
				constants.recordState[0] = static_cast<uint32>(recordBase);
				constants.recordState[1] = recordLimit;
				constants.recordState[2] = indexed ? 1u : 0u;
				if (!UpdateDynamicConstantBuffer(m_streamoutCaptureConstants,
					m_streamoutCaptureConstantsCapacity, &constants, sizeof(constants)))
				{
					captured = false;
					break;
				}
				ID3D11Buffer* constantsBuffer = m_streamoutCaptureConstants.Get();
				m_context->GSSetConstantBuffers(0, 1, &constantsBuffer);
				m_context->PSSetConstantBuffers(0, 1, &constantsBuffer);
				if (!indexed)
					m_context->DrawInstanced(vertexCount, 1, baseVertex, baseInstance + instance);
				else
					m_context->DrawIndexedInstanced(indexCount, 1, 0, baseVertex,
						baseInstance + instance);
			}
		}
	}
	ID3D11UnorderedAccessView* emptyUav{};
	m_context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
		nullptr, nullptr, StreamoutPixelCaptureUavSlot, 1, &emptyUav, nullptr);
	ID3D11ShaderResourceView* emptyResource{};
	m_context->GSSetShaderResources(0, 1, &emptyResource);
	m_context->GSSetShader(nullptr, nullptr, 0);
	m_context->PSSetShader(nullptr, nullptr, 0);
	// The temporary GS resource is not tracked by the ordinary texture binder.
	// Clear its cache as well, so the original GS resources are re-applied instead
	// of inheriting an implicitly removed binding on the next GX2 draw.
	ClearShaderResources();
	// The replay deliberately changes every graphics-stage binding. The next GX2
	// draw rebuilds the ordinary FBO/pipeline rather than inheriting capture state.
	InvalidateNativePipelineState();
	m_graphicsStateInvalid = true;
	m_streamoutDataAvailable = captured;
	return captured;
#endif
}
