void D3D11Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool)
{
	const D3D11_VIEWPORT viewport{ x, y, (std::max)(width, 1.0f), (std::max)(height, 1.0f), nearZ, farZ };
	if (!m_appliedViewportValid || std::memcmp(&m_appliedViewport, &viewport, sizeof(viewport)) != 0)
	{
		m_context->RSSetViewports(1, &viewport);
		m_appliedViewport = viewport;
		m_appliedViewportValid = true;
	}
}

void D3D11Renderer::renderTarget_setScissor(sint32 x, sint32 y, sint32 width, sint32 height)
{
	const D3D11_RECT rect{ x, y, x + (std::max)(width, 1), y + (std::max)(height, 1) };
	if (!m_appliedScissorValid || std::memcmp(&m_appliedScissor, &rect, sizeof(rect)) != 0)
	{
		m_context->RSSetScissorRects(1, &rect);
		m_appliedScissor = rect;
		m_appliedScissorValid = true;
	}
}

LatteCachedFBO* D3D11Renderer::rendertarget_createCachedFBO(uint64 key) { return new D3D11CachedFBO(key); }
void D3D11Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO* fbo)
{
	// LatteMRT::DeleteCachedFBO owns the common object and deletes it after this
	// renderer hook returns. D3D11 has no separate framebuffer object to free;
	// deleting here as well caused a double-free during the post-Present texture
	// cleanup, commonly surfacing as a read from 0xFFFFFFFFFFFFFFFF.
	if (m_activeFbo == fbo)
	{
		m_activeFbo = nullptr;
		m_activeFeedbackLoop = false;
	}
}

void D3D11Renderer::UnbindTextureHazards()
{
	// Latte updates shader resources before ApplyCurrentState() binds the FBO for
	// the draw. Release the outputs from the previous draw first, otherwise D3D11
	// silently replaces an SRV with null when that resource is still an RTV/DSV.
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

void D3D11Renderer::ClearShaderResources()
{
	std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> empty{};
	const auto clearStage = [&](auto& stage, auto setResources)
	{
		UINT usedSlots{};
		for (UINT slot = 0; slot < stage.size(); ++slot)
			if (stage[slot])
				usedSlots = slot + 1;
		if (usedSlots)
			setResources(usedSlots, empty.data());
		stage.fill(nullptr);
	};
	clearStage(m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Vertex)],
		[this](UINT count, ID3D11ShaderResourceView** values) {
			m_context->VSSetShaderResources(0, count, values);
		});
	clearStage(m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Pixel)],
		[this](UINT count, ID3D11ShaderResourceView** values) {
			m_context->PSSetShaderResources(0, count, values);
		});
	clearStage(m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Geometry)],
		[this](UINT count, ID3D11ShaderResourceView** values) {
			m_context->GSSetShaderResources(0, count, values);
		});
	for (auto& stage : m_logicalShaderResources)
		stage.fill(nullptr);
	m_boundTextures.fill(nullptr);
	m_feedbackViews.clear();
	m_feedbackResources.clear();
}

void D3D11Renderer::InvalidateNativePipelineState()
{
	m_appliedRasterizerState.Reset();
	m_appliedBlendState.Reset();
	m_appliedDepthStencilState.Reset();
	m_appliedBlendStateValid = false;
	m_appliedDepthStencilStateValid = false;
	m_appliedPrimitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_appliedViewportValid = false;
	m_appliedScissorValid = false;
	// Internal presentation/copy passes bind their own samplers. Forget the
	// filtered application state so the next GX2 rebuild emits the real bindings.
	for (auto& stage : m_boundSamplers)
		stage.fill(nullptr);
	for (auto& stage : m_boundConstantBuffers)
		stage.fill(nullptr);
}

bool D3D11Renderer::ResolveTextureFeedbackLoops(
	const std::array<ID3D11RenderTargetView*, 8>& targets, ID3D11DepthStencilView* depth)
{
	// These holders only need to bridge the current feedback resolution. The
	// immediate context keeps references to views that remain bound. Retaining
	// every prior snapshot here caused full texture copies to accumulate until a
	// memory-pressure cleanup happened.
	m_feedbackViews.clear();
	m_feedbackResources.clear();

	std::vector<ComPtr<ID3D11Resource>> outputResources;
	for (auto* target : targets)
	{
		if (!target)
			continue;
		ComPtr<ID3D11Resource> resource;
		target->GetResource(&resource);
		outputResources.emplace_back(std::move(resource));
	}
	if (depth)
	{
		ComPtr<ID3D11Resource> resource;
		depth->GetResource(&resource);
		outputResources.emplace_back(std::move(resource));
	}
	if (outputResources.empty())
		return false;
#if defined(CEMU_UWP)
	// A feedback snapshot duplicates the complete native texture. Under pressure,
	// let D3D11 null the conflicting SRV when the output merger is rebound. A
	// localized missing sample is preferable to crossing the Series S title cap.
	constexpr uint64 feedbackSnapshotStopBytes = D3D11ProcessMemoryLimitMB * 1024 * 1024;
	if (QueryProcessPrivateCommitBytes() >= feedbackSnapshotStopBytes)
	{
		m_feedbackSnapshots.clear();
		return false;
	}
#endif
	struct SubresourceRange
	{
		UINT firstMip{};
		UINT mipCount{ 1 };
		UINT firstSlice{};
		UINT sliceCount{ 1 };
		bool volume{};
	};
	const auto rangesOverlap = [](const SubresourceRange& left, const SubresourceRange& right)
	{
		const bool mipOverlap = left.firstMip < right.firstMip + right.mipCount &&
			right.firstMip < left.firstMip + left.mipCount;
		if (!mipOverlap)
			return false;
		if (left.volume || right.volume)
			return true;
		return left.firstSlice < right.firstSlice + right.sliceCount &&
			right.firstSlice < left.firstSlice + left.sliceCount;
	};
	const auto srvRange = [](const D3D11_SHADER_RESOURCE_VIEW_DESC& desc)
	{
		SubresourceRange range{};
		switch (desc.ViewDimension)
		{
		case D3D11_SRV_DIMENSION_TEXTURE1D:
			range.firstMip = desc.Texture1D.MostDetailedMip; range.mipCount = desc.Texture1D.MipLevels; break;
		case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
			range.firstMip = desc.Texture1DArray.MostDetailedMip; range.mipCount = desc.Texture1DArray.MipLevels;
			range.firstSlice = desc.Texture1DArray.FirstArraySlice; range.sliceCount = desc.Texture1DArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE2D:
			range.firstMip = desc.Texture2D.MostDetailedMip; range.mipCount = desc.Texture2D.MipLevels; break;
		case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
			range.firstMip = desc.Texture2DArray.MostDetailedMip; range.mipCount = desc.Texture2DArray.MipLevels;
			range.firstSlice = desc.Texture2DArray.FirstArraySlice; range.sliceCount = desc.Texture2DArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
			range.firstSlice = desc.Texture2DMSArray.FirstArraySlice; range.sliceCount = desc.Texture2DMSArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE3D:
			range.firstMip = desc.Texture3D.MostDetailedMip; range.mipCount = desc.Texture3D.MipLevels; range.volume = true; break;
		case D3D11_SRV_DIMENSION_TEXTURECUBE:
			range.firstMip = desc.TextureCube.MostDetailedMip; range.mipCount = desc.TextureCube.MipLevels; range.sliceCount = 6; break;
		case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
			range.firstMip = desc.TextureCubeArray.MostDetailedMip; range.mipCount = desc.TextureCubeArray.MipLevels;
			range.firstSlice = desc.TextureCubeArray.First2DArrayFace; range.sliceCount = desc.TextureCubeArray.NumCubes * 6; break;
		default: break;
		}
		if (range.mipCount == UINT_MAX) range.mipCount = UINT_MAX - range.firstMip;
		return range;
	};
	const auto rtvRange = [](ID3D11View* view)
	{
		SubresourceRange range{};
		ComPtr<ID3D11RenderTargetView> rtv;
		if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv))))
		{
			D3D11_RENDER_TARGET_VIEW_DESC desc{}; rtv->GetDesc(&desc);
			switch (desc.ViewDimension)
			{
			case D3D11_RTV_DIMENSION_TEXTURE1D: range.firstMip = desc.Texture1D.MipSlice; break;
			case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: range.firstMip = desc.Texture1DArray.MipSlice; range.firstSlice = desc.Texture1DArray.FirstArraySlice; range.sliceCount = desc.Texture1DArray.ArraySize; break;
			case D3D11_RTV_DIMENSION_TEXTURE2D: range.firstMip = desc.Texture2D.MipSlice; break;
			case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: range.firstMip = desc.Texture2DArray.MipSlice; range.firstSlice = desc.Texture2DArray.FirstArraySlice; range.sliceCount = desc.Texture2DArray.ArraySize; break;
			case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: range.firstSlice = desc.Texture2DMSArray.FirstArraySlice; range.sliceCount = desc.Texture2DMSArray.ArraySize; break;
			case D3D11_RTV_DIMENSION_TEXTURE3D: range.firstMip = desc.Texture3D.MipSlice; range.firstSlice = desc.Texture3D.FirstWSlice; range.sliceCount = desc.Texture3D.WSize; range.volume = true; break;
			default: break;
			}
			return range;
		}
		ComPtr<ID3D11DepthStencilView> dsv;
		if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&dsv))))
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; dsv->GetDesc(&desc);
			switch (desc.ViewDimension)
			{
			case D3D11_DSV_DIMENSION_TEXTURE1D: range.firstMip = desc.Texture1D.MipSlice; break;
			case D3D11_DSV_DIMENSION_TEXTURE1DARRAY: range.firstMip = desc.Texture1DArray.MipSlice; range.firstSlice = desc.Texture1DArray.FirstArraySlice; range.sliceCount = desc.Texture1DArray.ArraySize; break;
			case D3D11_DSV_DIMENSION_TEXTURE2D: range.firstMip = desc.Texture2D.MipSlice; break;
			case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: range.firstMip = desc.Texture2DArray.MipSlice; range.firstSlice = desc.Texture2DArray.FirstArraySlice; range.sliceCount = desc.Texture2DArray.ArraySize; break;
			case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY: range.firstSlice = desc.Texture2DMSArray.FirstArraySlice; range.sliceCount = desc.Texture2DMSArray.ArraySize; break;
			default: break;
			}
		}
		return range;
	};
	struct OutputView { ID3D11Resource* resource{}; SubresourceRange range{}; };
	std::vector<OutputView> outputViews;
	for (auto* target : targets)
	{
		if (!target) continue;
		ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
		outputViews.push_back({ resource.Get(), rtvRange(target) });
	}
	if (depth)
	{
		ComPtr<ID3D11Resource> resource; depth->GetResource(&resource);
		outputViews.push_back({ resource.Get(), rtvRange(depth) });
	}
	bool feedbackResolved = false;
	bool outputsUnbound = false;
	auto replaceConflicts = [&](auto& logicalStage, auto& physicalStage, auto setResources)
	{
		for (UINT slot = 0; slot < logicalStage.size(); ++slot)
		{
			auto& logical = logicalStage[slot];
			if (!logical)
				continue;
			ComPtr<ID3D11Resource> source;
			logical->GetResource(&source);
			D3D11_SHADER_RESOURCE_VIEW_DESC logicalDesc{};
			logical->GetDesc(&logicalDesc);
			const auto inputRange = srvRange(logicalDesc);
			const bool conflict = std::any_of(outputViews.begin(), outputViews.end(),
				[&](const auto& output) { return output.resource == source.Get() && rangesOverlap(inputRange, output.range); });
			if (!conflict)
			{
				ID3D11ShaderResourceView* original = logical.Get();
				if (physicalStage[slot].Get() != original)
					setResources(slot, &original);
				physicalStage[slot] = logical;
				continue;
			}
			feedbackResolved = true;
			if (!outputsUnbound)
			{
				m_context->OMSetRenderTargets(0, nullptr, nullptr);
				outputsUnbound = true;
			}
			// From this point on the original SRV must not remain physically bound: if
			// snapshot allocation or view creation fails, rebinding the output would
			// make D3D11 silently null it while our binding cache still claimed it was
			// active. Keep the logical binding for a later retry, but make the physical
			// state explicit now.
			ID3D11ShaderResourceView* nullView{};
			setResources(slot, &nullView);
			physicalStage[slot].Reset();

			ComPtr<ID3D11Resource> snapshot;
			{
				D3D11_RESOURCE_DIMENSION dimension{};
				source->GetType(&dimension);
				uint64 descriptorKey = HashBytes(&dimension, sizeof(dimension));
				auto cached = m_feedbackSnapshots.end();
				if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
				{
					ComPtr<ID3D11Texture1D> sourceTexture;
					source.As(&sourceTexture);
					D3D11_TEXTURE1D_DESC desc{};
					sourceTexture->GetDesc(&desc);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;
					descriptorKey = HashBytes(&desc, sizeof(desc), descriptorKey);
					cached = std::find_if(m_feedbackSnapshots.begin(), m_feedbackSnapshots.end(), [&](const auto& item) { return item.source.Get() == source.Get() && item.descriptorKey == descriptorKey; });
					if (cached != m_feedbackSnapshots.end()) snapshot = cached->copy;
					else { ComPtr<ID3D11Texture1D> copy; if (FAILED(m_device->CreateTexture1D(&desc, nullptr, &copy))) continue; snapshot = copy; }
				}
				else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
				{
					ComPtr<ID3D11Texture2D> sourceTexture;
					source.As(&sourceTexture);
					D3D11_TEXTURE2D_DESC desc{};
					sourceTexture->GetDesc(&desc);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;
					descriptorKey = HashBytes(&desc, sizeof(desc), descriptorKey);
					cached = std::find_if(m_feedbackSnapshots.begin(), m_feedbackSnapshots.end(), [&](const auto& item) { return item.source.Get() == source.Get() && item.descriptorKey == descriptorKey; });
					if (cached != m_feedbackSnapshots.end()) snapshot = cached->copy;
					else { ComPtr<ID3D11Texture2D> copy; if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &copy))) continue; snapshot = copy; }
				}
				else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
				{
					ComPtr<ID3D11Texture3D> sourceTexture;
					source.As(&sourceTexture);
					D3D11_TEXTURE3D_DESC desc{};
					sourceTexture->GetDesc(&desc);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;
					descriptorKey = HashBytes(&desc, sizeof(desc), descriptorKey);
					cached = std::find_if(m_feedbackSnapshots.begin(), m_feedbackSnapshots.end(), [&](const auto& item) { return item.source.Get() == source.Get() && item.descriptorKey == descriptorKey; });
					if (cached != m_feedbackSnapshots.end()) snapshot = cached->copy;
					else { ComPtr<ID3D11Texture3D> copy; if (FAILED(m_device->CreateTexture3D(&desc, nullptr, &copy))) continue; snapshot = copy; }
				}
				if (!snapshot)
					continue;
				if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
				{
					ComPtr<ID3D11Texture3D> texture; source.As(&texture);
					D3D11_TEXTURE3D_DESC desc{}; texture->GetDesc(&desc);
					const UINT mipCount = (std::min)(inputRange.mipCount,
						desc.MipLevels - (std::min)(inputRange.firstMip, desc.MipLevels));
					for (UINT mip = 0; mip < mipCount; ++mip)
						m_context->CopySubresourceRegion(snapshot.Get(), inputRange.firstMip + mip, 0, 0, 0,
							source.Get(), inputRange.firstMip + mip, nullptr);
				}
				else
				{
					UINT resourceMipLevels = 1;
					if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) { ComPtr<ID3D11Texture1D> texture; source.As(&texture); D3D11_TEXTURE1D_DESC desc{}; texture->GetDesc(&desc); resourceMipLevels = desc.MipLevels; }
					else { ComPtr<ID3D11Texture2D> texture; source.As(&texture); D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); resourceMipLevels = desc.MipLevels; }
					const UINT mipCount = (std::min)(inputRange.mipCount, resourceMipLevels - (std::min)(inputRange.firstMip, resourceMipLevels));
					for (UINT slice = 0; slice < inputRange.sliceCount; ++slice)
						for (UINT mip = 0; mip < mipCount; ++mip)
						{
							const UINT subresource = D3D11CalcSubresource(inputRange.firstMip + mip, inputRange.firstSlice + slice, resourceMipLevels);
							m_context->CopySubresourceRegion(snapshot.Get(), subresource, 0, 0, 0, source.Get(), subresource, nullptr);
						}
				}
				if (cached == m_feedbackSnapshots.end())
				{
					if (m_feedbackSnapshots.size() >= 4)
						m_feedbackSnapshots.erase(std::min_element(m_feedbackSnapshots.begin(), m_feedbackSnapshots.end(), [](const auto& a, const auto& b) { return a.lastUsedFrame < b.lastUsedFrame; }));
					m_feedbackSnapshots.push_back({ source, snapshot, descriptorKey, LatteGPUState.frameCounter });
				}
				else
					cached->lastUsedFrame = LatteGPUState.frameCounter;
			}

			ComPtr<ID3D11ShaderResourceView> snapshotView;
			if (FAILED(m_device->CreateShaderResourceView(snapshot.Get(), &logicalDesc, &snapshotView)))
				continue;
			ID3D11ShaderResourceView* replacement = snapshotView.Get();
			setResources(slot, &replacement);
			physicalStage[slot] = snapshotView;
			m_feedbackViews.emplace_back(std::move(snapshotView));
		}
	};

	replaceConflicts(
		m_logicalShaderResources[static_cast<uint32>(LatteConst::ShaderType::Vertex)],
		m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Vertex)],
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->VSSetShaderResources(slot, 1, value);
		});
	replaceConflicts(
		m_logicalShaderResources[static_cast<uint32>(LatteConst::ShaderType::Pixel)],
		m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Pixel)],
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->PSSetShaderResources(slot, 1, value);
		});
	replaceConflicts(
		m_logicalShaderResources[static_cast<uint32>(LatteConst::ShaderType::Geometry)],
		m_boundShaderResources[static_cast<uint32>(LatteConst::ShaderType::Geometry)],
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->GSSetShaderResources(slot, 1, value);
		});
	return feedbackResolved;
}

void D3D11Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO* fbo)
{
	m_boundColorBlendable.fill(true);
	if (!fbo)
	{
		m_activeFbo = nullptr;
		m_activeFeedbackLoop = false;
		m_context->OMSetRenderTargets(0, nullptr, nullptr);
		return;
	}
	m_activeFbo = fbo;
	auto* nativeFbo = static_cast<D3D11CachedFBO*>(fbo);
	const UINT framebufferWidth = static_cast<UINT>((std::max)(fbo->m_size.x, 1));
	const UINT framebufferHeight = static_cast<UINT>((std::max)(fbo->m_size.y, 1));
	if (!nativeFbo->nativeViewsCached)
	{
		for (UINT i = 0; i < nativeFbo->targets.size(); ++i)
		{
			if (!fbo->colorBuffer[i].texture)
				continue;
			auto* textureView = static_cast<D3D11TextureView*>(fbo->colorBuffer[i].texture);
			// D3D11 requires every simultaneously bound MRT to have identical
			// dimensions. GX2/Vulkan permit an attachment whose allocation is wider
			// than the effective framebuffer area, so use a synchronized per-size
			// alias only for that attachment.
			nativeFbo->targets[i] = textureView->FramebufferRTV(
				framebufferWidth, framebufferHeight);
			if (nativeFbo->targets[i])
			{
				nativeFbo->targetCount = i + 1;
				UINT formatSupport{};
				// The view may reinterpret a typeless GX2 allocation. Pipeline
				// state must follow the bound RTV, not the base texture format.
				const auto format = textureView->RTVFormat();
				nativeFbo->blendable[i] =
					SUCCEEDED(m_device->CheckFormatSupport(format, &formatSupport)) &&
					(formatSupport & D3D11_FORMAT_SUPPORT_BLENDABLE) != 0;
			}
			else
				cemuLog_log(LogType::Force,
					"D3D11 color attachment {} has no render-target view (format 0x{:x})",
					i, static_cast<uint32>(fbo->colorBuffer[i].texture->format));
		}
		nativeFbo->depth = fbo->depthBuffer.texture ?
			static_cast<D3D11TextureView*>(fbo->depthBuffer.texture)->DSV() : nullptr;
		nativeFbo->nativeViewsCached = true;
	}
	for (UINT i = 0; i < nativeFbo->targets.size(); ++i)
		if (fbo->colorBuffer[i].texture && nativeFbo->targets[i])
			static_cast<D3D11TextureView*>(fbo->colorBuffer[i].texture)->PrepareForRenderTarget(
				framebufferWidth, framebufferHeight);
	if (fbo->depthBuffer.texture)
		static_cast<D3D11TextureView*>(fbo->depthBuffer.texture)->PrepareForRenderTarget();
	m_boundColorBlendable = nativeFbo->blendable;
	// Vulkan uses VK_EXT_attachment_feedback_loop_layout when a title samples
	// an attachment that it is also updating. D3D11 forbids simultaneous SRV
	// and RTV/DSV bindings, so snapshot only the conflicting inputs before the
	// output merger is rebound. This preserves the pre-draw contents instead
	// of letting the runtime silently replace the SRV with null.
	m_activeFeedbackLoop = ResolveTextureFeedbackLoops(nativeFbo->targets, nativeFbo->depth);
	m_context->OMSetRenderTargets(nativeFbo->targetCount, nativeFbo->targets.data(), nativeFbo->depth);
}
