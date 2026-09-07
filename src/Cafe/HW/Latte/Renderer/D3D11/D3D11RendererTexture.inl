void* D3D11Renderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	m_uploadBuffer.resize(size);
	return m_uploadBuffer.data();
}
void D3D11Renderer::texture_releaseTextureUploadBuffer(uint8*)
{
#if defined(CEMU_UWP)
	// Reuse ordinary upload storage, but do not let one unusually large decoded
	// texture permanently define the CPU-side high-water mark on Series S.
	constexpr size_t maxRetainedUploadCapacity = 16 * 1024 * 1024;
	if (m_uploadBuffer.capacity() > maxRetainedUploadCapacity)
		std::vector<uint8>().swap(m_uploadBuffer);
#endif
}

TextureDecoder* D3D11Renderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth,
	Latte::E_DIM, uint32, uint32)
{
	using F = Latte::E_GX2SURFFMT;
	if (isDepth)
	{
		if (format == F::D24_S8_UNORM) return TextureDecoder_D24_S8::getInstance();
		if (format == F::D24_S8_FLOAT) return TextureDecoder_NullData64::getInstance();
		if (format == F::D32_S8_FLOAT) return TextureDecoder_D32_S8_UINT_X24::getInstance();
		if (format == F::D16_UNORM || format == F::R16_UNORM) return TextureDecoder_R16_UNORM::getInstance();
		return TextureDecoder_R32_FLOAT::getInstance();
	}
	switch (format)
	{
	case F::R8_UNORM: case F::R8_SNORM: return TextureDecoder_R8::getInstance();
	case F::R8_UINT: case F::R8_SINT: return TextureDecoder_R8_UINT::getInstance();
	case F::R8_G8_UNORM: case F::R8_G8_SNORM: return TextureDecoder_R8_G8::getInstance();
	case F::R8_G8_UINT: case F::R8_G8_SINT: return TextureDecoder_R8_G8::getInstance();
	case F::R8_G8_B8_A8_UNORM: case F::R8_G8_B8_A8_SNORM: case F::R8_G8_B8_A8_SRGB: return TextureDecoder_R8_G8_B8_A8::getInstance();
	case F::R8_G8_B8_A8_UINT: case F::R8_G8_B8_A8_SINT: return TextureDecoder_R8_G8_B8_A8_UINT::getInstance();
	case F::R16_UNORM: return TextureDecoder_R16_UNORM::getInstance();
	case F::R16_SNORM: return TextureDecoder_R16_SNORM::getInstance();
	case F::R16_FLOAT: return TextureDecoder_R16_FLOAT::getInstance();
	case F::R16_UINT: case F::R16_SINT: return TextureDecoder_R16_UINT::getInstance();
	case F::R16_G16_UNORM: case F::R16_G16_SNORM: return TextureDecoder_R16_G16::getInstance();
	case F::R16_G16_UINT: case F::R16_G16_SINT: return TextureDecoder_R16_G16::getInstance();
	case F::R16_G16_FLOAT: return TextureDecoder_R16_G16_FLOAT::getInstance();
	case F::R16_G16_B16_A16_UNORM: case F::R16_G16_B16_A16_SNORM: return TextureDecoder_R16_G16_B16_A16::getInstance();
	case F::R16_G16_B16_A16_FLOAT: return TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance();
	case F::R16_G16_B16_A16_UINT: case F::R16_G16_B16_A16_SINT: return TextureDecoder_R16_G16_B16_A16_UINT::getInstance();
	case F::R32_FLOAT: return TextureDecoder_R32_FLOAT::getInstance();
	case F::R32_UINT: case F::R32_SINT: return TextureDecoder_R32_UINT::getInstance();
	case F::R32_G32_FLOAT: return TextureDecoder_R32_G32_FLOAT::getInstance();
	case F::R32_G32_UINT: case F::R32_G32_SINT: return TextureDecoder_R32_G32_UINT::getInstance();
	case F::R32_G32_B32_A32_FLOAT: return TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance();
	case F::R32_G32_B32_A32_UINT: case F::R32_G32_B32_A32_SINT: return TextureDecoder_R32_G32_B32_A32_UINT::getInstance();
	case F::R10_G10_B10_A2_UNORM: case F::R10_G10_B10_A2_SRGB:
	case F::R10_G10_B10_A2_UINT:
	case F::A2_B10_G10_R10_UINT:
		return TextureDecoder_R10_G10_B10_A2_UNORM::getInstance();
	case F::A2_B10_G10_R10_UNORM:
		return TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16::getInstance();
	case F::R10_G10_B10_A2_SNORM:
		return TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance();
	case F::R11_G11_B10_FLOAT: return TextureDecoder_R11_G11_B10_FLOAT::getInstance();
	case F::BC1_UNORM: case F::BC1_SRGB: return TextureDecoder_BC1::getInstance();
	case F::BC2_UNORM: case F::BC2_SRGB: return TextureDecoder_BC2::getInstance();
	case F::BC3_UNORM: case F::BC3_SRGB: return TextureDecoder_BC3::getInstance();
	case F::BC4_UNORM: case F::BC4_SNORM: return TextureDecoder_BC4::getInstance();
	case F::BC5_UNORM: case F::BC5_SNORM: return TextureDecoder_BC5::getInstance();
	case F::R4_G4_UNORM: return TextureDecoder_R4G4_UNORM_To_RGBA8::getInstance();
	case F::R4_G4_B4_A4_UNORM: return TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance();
	case F::R5_G6_B5_UNORM: return TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance();
	case F::R5_G5_B5_A1_UNORM: return TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance();
	case F::A1_B5_G5_R5_UNORM: return TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance();
	case F::R24_X8_UNORM: return TextureDecoder_R24_X8::getInstance();
	case F::X24_G8_UINT: return TextureDecoder_X24_G8_UINT::getInstance();
	case F::R24_X8_FLOAT: case F::R32_X8_FLOAT: return TextureDecoder_NullData64::getInstance();
	default: return TextureDecoder_R8_G8_B8_A8::getInstance();
	}
}

void D3D11Renderer::texture_clearSlice(LatteTexture* texture, sint32 slice, sint32 mip)
{
	if (texture->isDepth)
		texture_clearDepthSlice(texture, slice, mip, true, texture->hasStencil, 0.0f, 0);
	else
		texture_clearColorSlice(texture, slice, mip, 0, 0, 0, 0);
}

void D3D11Renderer::texture_loadSlice(LatteTexture* texture, sint32 width, sint32 height,
	sint32 depth, void* pixels, sint32 sliceIndex, sint32 mipIndex, uint32 imageSize)
{
	if (!texture || !pixels || imageSize == 0 || mipIndex < 0 || sliceIndex < 0)
		return;
	auto* d3d = static_cast<D3D11Texture*>(texture);
	d3d->AllocateOnHost();
	const auto& info = d3d->NativeFormat();
	const UINT sourceWidth = static_cast<UINT>((std::max)(width, 1));
	const UINT sourceHeight = static_cast<UINT>((std::max)(height, 1));
	const uint32 rowPitch = RowPitch(info, sourceWidth);
	const uint32 rowCount = RowCount(info, sourceHeight);
	const uint64 requiredSize = static_cast<uint64>(rowPitch) * rowCount;
	if (requiredSize > imageSize)
	{
		cemuLog_log(LogType::Force,
			"D3D11 texture upload skipped: decoded buffer is too small (required {} bytes, got {})",
			requiredSize, imageSize);
		return;
	}

	if (d3d->Texture1D())
	{
		D3D11_TEXTURE1D_DESC desc{};
		d3d->Texture1D()->GetDesc(&desc);
		if (static_cast<UINT>(mipIndex) >= desc.MipLevels ||
			static_cast<UINT>(sliceIndex) >= desc.ArraySize)
			return;
		const UINT mipWidth = (std::max)(desc.Width >> mipIndex, 1u);
		const D3D11_BOX box{ 0, 0, 0, (std::min)(sourceWidth, mipWidth), 1, 1 };
		const UINT subresource = D3D11CalcSubresource(mipIndex, sliceIndex, desc.MipLevels);
		d3d->BeginNativeWrite();
		m_context->UpdateSubresource(d3d->Resource(), subresource, &box, pixels, rowPitch, 0);
		return;
	}

	if (d3d->Texture3D())
	{
		D3D11_TEXTURE3D_DESC desc{};
		d3d->Texture3D()->GetDesc(&desc);
		if (static_cast<UINT>(mipIndex) >= desc.MipLevels ||
			static_cast<UINT>(sliceIndex) >= (std::max)(desc.Depth >> mipIndex, 1u))
			return;
		const UINT mipWidth = (std::max)(desc.Width >> mipIndex, 1u);
		const UINT mipHeight = (std::max)(desc.Height >> mipIndex, 1u);
		const UINT mipDepth = (std::max)(desc.Depth >> mipIndex, 1u);
		const UINT uploadDepth = 1;
		const D3D11_BOX box{ 0, 0, static_cast<UINT>(sliceIndex),
			(std::min)(sourceWidth, mipWidth), (std::min)(sourceHeight, mipHeight),
			static_cast<UINT>(sliceIndex) + uploadDepth };
		d3d->BeginNativeWrite();
		m_context->UpdateSubresource(d3d->Resource(), static_cast<UINT>(mipIndex), &box,
			pixels, rowPitch, rowPitch * rowCount);
		return;
	}

	D3D11_TEXTURE2D_DESC desc{};
	d3d->Texture2D()->GetDesc(&desc);
	if (static_cast<UINT>(mipIndex) >= desc.MipLevels ||
		static_cast<UINT>(sliceIndex) >= desc.ArraySize)
		return;
	const UINT mipWidth = (std::max)(desc.Width >> mipIndex, 1u);
	const UINT mipHeight = (std::max)(desc.Height >> mipIndex, 1u);
	const UINT uploadWidth = (std::min)(sourceWidth, mipWidth);
	const UINT uploadHeight = (std::min)(sourceHeight, mipHeight);
	if (!uploadWidth || !uploadHeight)
		return;
	const bool isDepthStencil = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
	if (isDepthStencil && (sourceWidth < mipWidth || sourceHeight < mipHeight))
	{
		cemuLog_log(LogType::Force,
			"D3D11 depth upload skipped: UpdateSubresource requires a complete mip slice "
			"(required {}x{}, got {}x{})",
			mipWidth, mipHeight, sourceWidth, sourceHeight);
		return;
	}
	const D3D11_BOX destinationBox{ 0, 0, 0, uploadWidth, uploadHeight, 1 };
	const UINT subresource = D3D11CalcSubresource(mipIndex, sliceIndex, desc.MipLevels);
	const void* uploadPixels = pixels;
	uint32 uploadRowPitch = rowPitch;
	uint64 uploadSize = requiredSize;
	std::vector<uint8> paddedCompressedData;
	// D3D11 forbids a destination box for resources that can be bound as a
	// depth/stencil target, even when the box covers the entire subresource.
	// The decoded depth images supplied by Latte are complete mip slices.
	const D3D11_BOX* destinationBoxPtr = isDepthStencil ? nullptr : &destinationBox;
	if (info.compressed)
	{
		// Update a complete BC subresource. Aligning the base allocation changes
		// the physical size of some lower mips (for example 130 -> 132, then
		// 65 -> 66), so pad missing edge blocks instead of allowing D3D11 to read
		// beyond the decoded GX2 buffer.
		const uint32 resourceRowPitch = RowPitch(info, mipWidth);
		const uint32 resourceRowCount = RowCount(info, mipHeight);
		uploadSize = static_cast<uint64>(resourceRowPitch) * resourceRowCount;
		paddedCompressedData.assign(static_cast<size_t>(uploadSize), 0);
		const uint32 rowsToCopy = (std::min)(rowCount, resourceRowCount);
		const uint32 bytesPerRowToCopy = (std::min)(rowPitch, resourceRowPitch);
		for (uint32 row = 0; row < rowsToCopy; ++row)
		{
			std::memcpy(paddedCompressedData.data() + static_cast<size_t>(row) * resourceRowPitch,
				static_cast<const uint8*>(pixels) + static_cast<size_t>(row) * rowPitch,
				bytesPerRowToCopy);
		}
		uploadPixels = paddedCompressedData.data();
		uploadRowPitch = resourceRowPitch;
		destinationBoxPtr = nullptr;
	}
	{
		D3D11_DRIVER_TRACE(fmt::format(
			"UpdateSubresource texture={} sub={} mip={} slice={} box={}x{}{} rowPitch={} bytes={}",
			static_cast<const void*>(d3d->Resource()), subresource, mipIndex, sliceIndex,
			uploadWidth, uploadHeight,
			info.compressed ? " full-bc" : (destinationBoxPtr ? "" : " full-depth"),
			uploadRowPitch, uploadSize));
		d3d->BeginNativeWrite();
		m_context->UpdateSubresource(d3d->Resource(), subresource, destinationBoxPtr, uploadPixels,
			uploadRowPitch, static_cast<UINT>((std::min<uint64>)(uploadSize, UINT_MAX)));
	}
}

void D3D11Renderer::texture_clearColorSlice(LatteTexture* texture, sint32 slice, sint32 mip,
	float r, float g, float b, float a)
{
	auto* view = static_cast<D3D11TextureView*>(texture->GetOrCreateView(mip, 1, slice, 1));
	view->PrepareForRenderTarget();
	if (view->RTV())
	{
		const float color[4]{ r, g, b, a };
		m_context->ClearRenderTargetView(view->RTV(), color);
	}
}

void D3D11Renderer::texture_clearDepthSlice(LatteTexture* texture, uint32 slice, sint32 mip,
	bool clearDepth, bool clearStencil, float depth, uint32 stencil)
{
	auto* view = static_cast<D3D11TextureView*>(texture->GetOrCreateView(mip, 1, slice, 1));
	view->PrepareForRenderTarget();
	UINT flags = (clearDepth ? D3D11_CLEAR_DEPTH : 0) | (clearStencil && texture->hasStencil ? D3D11_CLEAR_STENCIL : 0);
	if (view->DSV() && flags)
		m_context->ClearDepthStencilView(view->DSV(), flags, depth, static_cast<UINT8>(stencil));
}

LatteTexture* D3D11Renderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress,
	MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth,
	uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	return new D3D11Texture(this, dim, physAddress, physMipAddress, format, width, height,
		depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

ID3D11SamplerState* D3D11Renderer::GetSamplerState(LatteDecompilerShader* shader,
	uint32 textureIndex, LatteTexture* texture)
{
	if (!shader || textureIndex >= LATTE_NUM_MAX_TEX_UNITS)
		return m_presentSampler.Get();
	const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[textureIndex];
	if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
		return m_presentSampler.Get();
	const uint32 samplerIndex = stageSamplerIndex +
		LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
	const auto& sampler = LatteGPUState.contextNew.SQ_TEX_SAMPLER[samplerIndex];
	const bool comparison = shader->textureUsesDepthCompare[textureIndex];
	uint32 anisotropy = sampler.WORD0.get_MAX_ANISO_RATIO();
	if (texture->overwriteInfo.anisotropicLevel >= 0)
		anisotropy = texture->overwriteInfo.anisotropicLevel;

	D3D11_SAMPLER_DESC desc{};
	desc.Filter = SamplerFilter(sampler, comparison, anisotropy > 0);
	desc.AddressU = AddressMode(sampler.WORD0.get_CLAMP_X());
	desc.AddressV = AddressMode(sampler.WORD0.get_CLAMP_Y());
	desc.AddressW = AddressMode(sampler.WORD0.get_CLAMP_Z());
	desc.MipLODBias = static_cast<float>(sampler.WORD1.get_LOD_BIAS()) / 64.0f;
	if (texture->overwriteInfo.hasRelativeLodBias)
		desc.MipLODBias += static_cast<float>(texture->overwriteInfo.relativeLodBias) / 64.0f;
	if (texture->overwriteInfo.hasLodBias)
		desc.MipLODBias = static_cast<float>(texture->overwriteInfo.lodBias) / 64.0f;
	desc.MaxAnisotropy = (std::min)(16u, 1u << (std::min)(anisotropy, 4u));
	desc.ComparisonFunc = comparison ?
		CompareFunc(static_cast<Latte::E_COMPAREFUNC>(sampler.WORD0.get_DEPTH_COMPARE_FUNCTION())) :
		D3D11_COMPARISON_NEVER;
	desc.MinLOD = static_cast<float>(sampler.WORD1.get_MIN_LOD()) / 64.0f;
	desc.MaxLOD = static_cast<float>(sampler.WORD1.get_MAX_LOD()) / 64.0f;
	if (sampler.WORD0.get_MIP_FILTER() ==
		Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE)
	{
		desc.MinLOD = 0.0f;
		desc.MaxLOD = 0.25f;
	}
	if (desc.MaxLOD < desc.MinLOD)
		desc.MaxLOD = desc.MinLOD;

	const auto borderType = sampler.WORD0.get_BORDER_COLOR_TYPE();
	if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK)
		desc.BorderColor[3] = 1.0f;
	else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE)
		std::fill(std::begin(desc.BorderColor), std::end(desc.BorderColor), 1.0f);
	else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::REGISTER)
	{
		const _LatteRegisterSetSamplerBorderColor* color{};
		if (shader->shaderType == LatteConst::ShaderType::Vertex)
			color = LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else if (shader->shaderType == LatteConst::ShaderType::Pixel)
			color = LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else
			color = LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		desc.BorderColor[0] = color->red.get_channelValue();
		desc.BorderColor[1] = color->green.get_channelValue();
		desc.BorderColor[2] = color->blue.get_channelValue();
		desc.BorderColor[3] = color->alpha.get_channelValue();
	}

	const uint64 key = HashBytes(&desc, sizeof(desc));
	auto found = m_samplerCache.find(key);
	if (found != m_samplerCache.end())
		return found->second.Get();
	ComPtr<ID3D11SamplerState> state;
	if (FAILED(m_device->CreateSamplerState(&desc, &state)))
		return m_presentSampler.Get();
	return m_samplerCache.emplace(key, std::move(state)).first->second.Get();
}

void D3D11Renderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 unit)
{
	if (unit >= m_boundTextures.size())
		return;
	auto* view = static_cast<D3D11TextureView*>(textureView);
	if (view)
		view->PrepareForSampling();
	m_boundTextures[unit] = view ? view->SRV() : nullptr;
	ID3D11ShaderResourceView* srv = view ? view->SRV() : nullptr;
	auto bindTexture = [&](LatteDecompilerShader* shader, uint32 textureIndex, uint32 stageIndex,
		auto setResources, auto setSamplers)
	{
		auto* nativeShader = shader ? static_cast<D3D11Shader*>(shader->shader) : nullptr;
		if (!shader || !nativeShader || textureIndex >= LATTE_NUM_MAX_TEX_UNITS)
			return;
		const auto expectedDim = shader->textureUnitDim[textureIndex];
		if (textureView &&
			((expectedDim == Latte::E_DIM::DIM_1D && textureView->dim != Latte::E_DIM::DIM_1D) ||
			 (expectedDim == Latte::E_DIM::DIM_2D &&
			  textureView->dim != Latte::E_DIM::DIM_2D &&
			  textureView->dim != Latte::E_DIM::DIM_2D_MSAA)))
		{
			// Vulkan binds a null texture for these mismatches. A null SRV has
			// the same zero-read semantics in D3D11 and avoids a debug-layer
			// dimension error at draw time.
			srv = nullptr;
		}
		const sint32 originalBinding = shader->resourceMapping.textureUnitToBindingPoint[textureIndex];
		const UINT binding = originalBinding >= 0 ?
			nativeShader->TextureSlot(static_cast<UINT>(originalBinding)) : D3D11Shader::InvalidSlot;
		if (binding == D3D11Shader::InvalidSlot)
			return;
		uint32 registerBase{};
		if (shader->shaderType == LatteConst::ShaderType::Vertex)
			registerBase = Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
		else if (shader->shaderType == LatteConst::ShaderType::Pixel)
			registerBase = Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
		else
			registerBase = Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
		const uint32 word4 = LatteGPUState.contextRegister[registerBase + textureIndex * 7 + 4];
		const auto swizzleFormat = textureView ? textureView->format :
			Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
		if (binding < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
		{
			auto& selectors = m_samplerSwizzles[static_cast<uint32>(shader->shaderType)][binding];
			selectors[0] = AdjustTextureComponentSelector(swizzleFormat, (word4 >> 16) & 7);
			selectors[1] = AdjustTextureComponentSelector(swizzleFormat, (word4 >> 19) & 7);
			selectors[2] = AdjustTextureComponentSelector(swizzleFormat, (word4 >> 22) & 7);
			selectors[3] = AdjustTextureComponentSelector(swizzleFormat, (word4 >> 25) & 7);
		}
		ID3D11SamplerState* sampler = textureView ?
			GetSamplerState(shader, textureIndex, textureView->baseTexture) : nullptr;
		setResources(binding, &srv);
		if (binding < m_boundShaderResources[stageIndex].size())
		{
			m_boundShaderResources[stageIndex][binding] = srv;
			m_logicalShaderResources[stageIndex][binding] = srv;
		}
		setSamplers(binding, &sampler);
	};
	if (unit < LATTE_CEMU_VS_TEX_UNIT_BASE)
	{
		const uint32 textureIndex = unit - LATTE_CEMU_PS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActivePixelShader();
		bindTexture(shader, textureIndex, static_cast<uint32>(LatteConst::ShaderType::Pixel),
			[this](UINT binding, ID3D11ShaderResourceView** value) { m_context->PSSetShaderResources(binding, 1, value); },
			[this](UINT binding, ID3D11SamplerState** value) { m_context->PSSetSamplers(binding, 1, value); });
	}
	else if (unit < LATTE_CEMU_GS_TEX_UNIT_BASE)
	{
		const uint32 textureIndex = unit - LATTE_CEMU_VS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActiveVertexShader();
		bindTexture(shader, textureIndex, static_cast<uint32>(LatteConst::ShaderType::Vertex),
			[this](UINT binding, ID3D11ShaderResourceView** value) { m_context->VSSetShaderResources(binding, 1, value); },
			[this](UINT binding, ID3D11SamplerState** value) { m_context->VSSetSamplers(binding, 1, value); });
	}
	else
	{
		const uint32 textureIndex = unit - LATTE_CEMU_GS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActiveGeometryShader();
		bindTexture(shader, textureIndex, static_cast<uint32>(LatteConst::ShaderType::Geometry),
			[this](UINT binding, ID3D11ShaderResourceView** value) { m_context->GSSetShaderResources(binding, 1, value); },
			[this](UINT binding, ID3D11SamplerState** value) { m_context->GSSetSamplers(binding, 1, value); });
	}
}

void D3D11Renderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 srcX,
	sint32 srcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 dstX, sint32 dstY,
	sint32 dstSlice, sint32 width, sint32 height, sint32 depth)
{
	auto* source = static_cast<D3D11Texture*>(src);
	auto* destination = static_cast<D3D11Texture*>(dst);
	source->AllocateOnHost(); destination->AllocateOnHost();
	source->CommitAliasWriter();
	if (width <= 0 || height <= 0 || depth <= 0)
		return;
	const UINT sourceMipLevels = source->EffectiveMipLevels();
	const UINT destinationMipLevels = destination->EffectiveMipLevels();
	if (srcMip < 0 || dstMip < 0 || static_cast<UINT>(srcMip) >= sourceMipLevels ||
		static_cast<UINT>(dstMip) >= destinationMipLevels || srcSlice < 0 || dstSlice < 0)
		return;
	const bool source3D = src->Is3DTexture();
	const bool destination3D = dst->Is3DTexture();
	const auto arraySize = [](D3D11Texture* texture)
	{
		if (texture->Texture1D())
		{
			D3D11_TEXTURE1D_DESC desc{};
			texture->Texture1D()->GetDesc(&desc);
			return desc.ArraySize;
		}
		if (texture->Texture2D())
		{
			D3D11_TEXTURE2D_DESC desc{};
			texture->Texture2D()->GetDesc(&desc);
			return desc.ArraySize;
		}
		return 1u;
	};
	if ((!source3D && static_cast<uint64>(srcSlice) + depth > arraySize(source)) ||
		(!destination3D && static_cast<uint64>(dstSlice) + depth > arraySize(destination)))
		return;
	if (source3D)
	{
		D3D11_TEXTURE3D_DESC desc{};
		source->Texture3D()->GetDesc(&desc);
		if (static_cast<uint64>(srcSlice) + depth > (std::max)(desc.Depth >> srcMip, 1u))
			return;
	}
	if (destination3D)
	{
		D3D11_TEXTURE3D_DESC desc{};
		destination->Texture3D()->GetDesc(&desc);
		if (static_cast<uint64>(dstSlice) + depth > (std::max)(desc.Depth >> dstMip, 1u))
			return;
	}
	destination->BeginNativeWrite();
	const uint32 copyIterations = source3D && destination3D ? 1u : static_cast<uint32>(depth);
	for (uint32 layer = 0; layer < copyIterations; ++layer)
	{
		const UINT sourceZ = source3D ? static_cast<UINT>((std::max)(srcSlice + static_cast<sint32>(layer), 0)) : 0u;
		const UINT destinationZ = destination3D ? static_cast<UINT>((std::max)(dstSlice + static_cast<sint32>(layer), 0)) : 0u;
		const UINT boxDepth = source3D && destination3D ? static_cast<UINT>(depth) : 1u;
		D3D11_BOX box{ static_cast<UINT>((std::max)(srcX, 0)),
			static_cast<UINT>((std::max)(srcY, 0)), sourceZ,
			static_cast<UINT>((std::max)(srcX + width, 0)),
			static_cast<UINT>((std::max)(srcY + height, 0)), sourceZ + boxDepth };
		const UINT sourceSubresource = source3D ? static_cast<UINT>(srcMip) :
			D3D11CalcSubresource(srcMip, srcSlice + layer, sourceMipLevels);
		const UINT destinationSubresource = destination3D ? static_cast<UINT>(dstMip) :
			D3D11CalcSubresource(dstMip, dstSlice + layer, destinationMipLevels);
		m_context->CopySubresourceRegion(destination->Resource(), destinationSubresource,
			static_cast<UINT>((std::max)(dstX, 0)), static_cast<UINT>((std::max)(dstY, 0)),
			destinationZ, source->Resource(), sourceSubresource, &box);
	}
}

LatteTextureReadbackInfo* D3D11Renderer::texture_createReadback(LatteTextureView* view)
{
	return new D3D11Readback(this, static_cast<D3D11TextureView*>(view));
}

void D3D11Renderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* source,
	sint32 srcMip, sint32 srcSlice, LatteTexture* destination, sint32 dstMip, sint32 dstSlice,
	sint32 width, sint32 height)
{
	if (!source || !destination || width <= 0 || height <= 0)
		return;

	source->AllocateOnHost();
	destination->AllocateOnHost();
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(source, srcMip, destination, dstMip))
	{
		cemuLog_logDebug(LogType::Force,
			"D3D11 surface copy skipped: source and destination use different effective scale ratios");
		return;
	}
	const auto sourceFormat = GetFormatInfo(source->format, source->isDepth);
	const auto destinationFormat = GetFormatInfo(destination->format, destination->isDepth);
	if (sourceFormat.bytesPerBlock != destinationFormat.bytesPerBlock ||
		sourceFormat.blockWidth != destinationFormat.blockWidth ||
		sourceFormat.blockHeight != destinationFormat.blockHeight)
	{
		// This is the same restriction used by Vulkan's surface-copy path.
		// Reinterpreting different-sized texels would make the fullscreen draw
		// read beyond the source footprint or truncate destination pixels.
		cemuLog_logDebug(LogType::Force,
			"D3D11 surface copy skipped: incompatible source/destination texel sizes");
		return;
	}
	auto* sourceView = static_cast<D3D11TextureView*>(
		source->GetOrCreateView(srcMip, 1, srcSlice, 1));
	auto* destinationView = static_cast<D3D11TextureView*>(
		destination->GetOrCreateView(dstMip, 1, dstSlice, 1));
	if (sourceView)
		sourceView->PrepareForSampling();
	if (destinationView)
		destinationView->PrepareForRenderTarget();
	if (!sourceView || !destinationView || !sourceView->SRV())
		return;

	sint32 effectiveWidth = width;
	sint32 effectiveHeight = height;
	LatteTexture_scaleToEffectiveSize(source, &effectiveWidth, &effectiveHeight, 0);
	effectiveWidth = (std::max)(effectiveWidth, 1);
	effectiveHeight = (std::max)(effectiveHeight, 1);

	// GX2 permits depth/color copies that D3D11 explicitly rejects in
	// CopySubresourceRegion. Reproduce the other Cemu backends and convert the
	// red/depth component with a fullscreen draw into the destination view.
	UnbindTextureHazards();
	// The destination can still be present in a shader stage from the preceding
	// GX2 draw. D3D11 otherwise nulls that SRV implicitly when it becomes an RTV,
	// leaving the renderer's binding cache out of sync with the real context.
	ClearShaderResources();
	ID3D11RenderTargetView* colorTarget = destination->isDepth ? nullptr : destinationView->RTV();
	ID3D11DepthStencilView* depthTarget = destination->isDepth ? destinationView->DSV() : nullptr;
	if ((!destination->isDepth && !colorTarget) || (destination->isDepth && !depthTarget))
		return;
	m_context->OMSetRenderTargets(destination->isDepth ? 0 : 1,
		destination->isDepth ? nullptr : &colorTarget, depthTarget);

	const float blendFactor[4]{};
	m_context->RSSetState(m_rasterizerState.Get());
	m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
	m_context->OMSetDepthStencilState(destination->isDepth ?
		m_surfaceCopyDepthState.Get() : m_depthStencilState.Get(), 0);
	m_context->VSSetShader(m_presentVS.Get(), nullptr, 0);
	m_context->PSSetShader(destination->isDepth ?
		m_surfaceCopyDepthPS.Get() : m_surfaceCopyColorPS.Get(), nullptr, 0);
	m_context->GSSetShader(nullptr, nullptr, 0);
	m_context->IASetInputLayout(nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11ShaderResourceView* sourceResource = sourceView->SRV();
	ID3D11SamplerState* sampler = m_presentPointSampler.Get();
	m_context->PSSetShaderResources(0, 1, &sourceResource);
	m_context->PSSetSamplers(0, 1, &sampler);
	renderTarget_setViewport(0, 0, static_cast<float>(effectiveWidth),
		static_cast<float>(effectiveHeight), 0, 1, false);
	renderTarget_setScissor(0, 0, effectiveWidth, effectiveHeight);
	m_context->Draw(3, 0);
	D3D11_DEBUG_CHECK(destination->isDepth ?
		"color-to-depth surface copy" : "depth-to-color surface copy");

	ID3D11ShaderResourceView* nullResource = nullptr;
	m_context->PSSetShaderResources(0, 1, &nullResource);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	InvalidateNativePipelineState();
	m_graphicsStateInvalid = true;
	LatteGPUState.repeatTextureInitialization = true;
}
