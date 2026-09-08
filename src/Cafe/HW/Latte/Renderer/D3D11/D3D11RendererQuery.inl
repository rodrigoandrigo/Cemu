Renderer::IndexAllocation D3D11Renderer::indexData_reserveIndexMemory(uint32 size)
{
	for (uint32 attempt = 0; attempt < 2; ++attempt)
	{
		try
		{
			auto allocation = std::make_unique<IndexBufferAllocation>();
			allocation->data.resize(size);
			void* memory = allocation->data.data();
			return { memory, allocation.release() };
		}
		catch (const std::bad_alloc&)
		{
			if (attempt == 0)
				RecoverFromMemoryPressure("index staging memory", true);
		}
	}
	cemuLog_log(LogType::Force,
		"D3D11 index allocation skipped: unable to reserve {} bytes", size);
	return {};
}

void D3D11Renderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	delete static_cast<IndexBufferAllocation*>(allocation.rendererInternal);
	allocation = {};
}

void D3D11Renderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	auto* data = static_cast<IndexBufferAllocation*>(allocation.rendererInternal);
	if (!data || data->data.empty())
		return;
	const UINT dataSize = static_cast<UINT>(data->data.size());
	const UINT alignedSize = (dataSize + 3u) & ~3u;
	constexpr UINT initialRingCapacity = 4u * 1024u * 1024u;
	const UINT requiredCapacity = (std::max)(initialRingCapacity, alignedSize);
	if (!m_indexRingBuffer || requiredCapacity > m_indexRingCapacity)
	{
		UINT newCapacity = m_indexRingCapacity ? m_indexRingCapacity : initialRingCapacity;
		while (newCapacity < requiredCapacity &&
			newCapacity <= (std::numeric_limits<UINT>::max)() / 2)
			newCapacity *= 2;
		newCapacity = (std::max)(newCapacity, requiredCapacity);

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = newCapacity;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ComPtr<ID3D11Buffer> replacement;
		HRESULT createResult = m_device->CreateBuffer(&desc, nullptr, &replacement);
		if (IsMemoryPressureResult(createResult))
		{
			RecoverFromMemoryPressure("index upload ring", true);
			if (m_deviceLost.load(std::memory_order_relaxed))
			{
				delete data;
				allocation = {};
				return;
			}
			createResult = m_device->CreateBuffer(&desc, nullptr, &replacement);
		}
		if (FAILED(createResult))
		{
			cemuLog_log(LogType::Force,
				"D3D11 index upload skipped: ring CreateBuffer failed with HRESULT 0x{:08X}",
				static_cast<uint32>(createResult));
			delete data;
			allocation = {};
			return;
		}

		m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		LatteIndices_invalidateAll();
		m_indexRingBuffer = std::move(replacement);
		m_indexRingCapacity = newCapacity;
		m_indexRingOffset = 0;
	}

	if (m_indexRingOffset > m_indexRingCapacity - alignedSize)
	{
		// D3D11On12 implements WRITE_DISCARD as RenameAndMapBuffer: preceding draws
		// retain the old backing allocation while this map receives fresh storage.
		// Invalidate Cemu's offset cache because its entries refer to the previous
		// generation, but never serialize the entire GPU merely to wrap the ring.
		m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		LatteIndices_invalidateAll();
		m_indexRingOffset = 0;
		++m_indexRingWrapCount;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	const D3D11_MAP mapMode = m_indexRingOffset == 0 ?
		D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
	const HRESULT mapResult = m_context->Map(m_indexRingBuffer.Get(), 0, mapMode, 0, &mapped);
	if (FAILED(mapResult))
	{
		if (IsDeviceLostResult(mapResult))
			RecordDeviceLost(mapResult, "Map index ring buffer");
		cemuLog_log(LogType::Force,
			"D3D11 index upload skipped: ring Map failed with HRESULT 0x{:08X} for {} bytes",
			static_cast<uint32>(mapResult), dataSize);
		delete data;
		allocation = {};
		return;
	}

	std::memcpy(static_cast<uint8*>(mapped.pData) + m_indexRingOffset,
		data->data.data(), dataSize);
	m_context->Unmap(m_indexRingBuffer.Get(), 0);
	data->buffer = m_indexRingBuffer;
	data->offset = m_indexRingOffset;
	m_indexRingOffset += alignedSize;
	++m_indexUploadCount;
	if (!m_keepIndexStagingForPixelStreamout)
		std::vector<uint8>().swap(data->data);
	allocation.mem = nullptr;
}

LatteQueryObject* D3D11Renderer::occlusionQuery_create() { return new D3D11Query(m_device.Get(), m_context.Get()); }
void D3D11Renderer::occlusionQuery_destroy(LatteQueryObject* query) { delete query; }
// Submission is sufficient: GetData polls the individual query. Waiting for
// every unrelated command defeats D3D11On12's batched query resolution.
void D3D11Renderer::occlusionQuery_flush() { Flush(false); }
void D3D11Renderer::occlusionQuery_updateState() {}
