class D3D11Query final : public LatteQueryObject
{
public:
	D3D11Query(ID3D11Device* device, ID3D11DeviceContext* context) : m_context(context)
	{
		D3D11_QUERY_DESC desc{ D3D11_QUERY_OCCLUSION, 0 };
		ThrowIfFailed(device->CreateQuery(&desc, &m_query), "CreateQuery");
	}
	bool getResult(uint64& samples) override
	{
		if (!queryEnded)
			return false;
		const HRESULT hr = m_context->GetData(m_query.Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		return hr == S_OK;
	}
	void begin() override { queryEnded = false; m_context->Begin(m_query.Get()); }
	void end() override { m_context->End(m_query.Get()); queryEnded = true; }
private:
	ComPtr<ID3D11DeviceContext> m_context;
	ComPtr<ID3D11Query> m_query;
};

class D3D11Texture;

class D3D11TextureView final : public LatteTextureView
{
public:
	D3D11TextureView(D3D11Texture* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format,
		sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount);
	~D3D11TextureView() override;
	ID3D11ShaderResourceView* SRV() const { return m_srv.Get(); }
	ID3D11RenderTargetView* RTV() const { return m_rtv.Get(); }
	ID3D11DepthStencilView* DSV() const { return m_dsv.Get(); }
	DXGI_FORMAT RTVFormat() const { return m_rtvFormat; }
	void PrepareForSampling();
	void PrepareForRenderTarget();
	void PrepareForRenderTarget(UINT framebufferWidth, UINT framebufferHeight);
	ID3D11RenderTargetView* FramebufferRTV(UINT framebufferWidth, UINT framebufferHeight);
	void CopyAliasToBase();
	bool IsIncompatibleAlias() const { return m_incompatibleAlias; }
private:
	struct AliasStagingResource
	{
		D3D11_TEXTURE2D_DESC desc{};
		ComPtr<ID3D11Texture2D> texture;
	};
	struct AttachmentAlias
	{
		UINT width{};
		UINT height{};
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> rtv;
		uint64 syncedVersion{ (std::numeric_limits<uint64>::max)() };
	};
	bool CreateIncompatibleAlias(const FormatInfo& requested);
	AttachmentAlias* GetOrCreateAttachmentAlias(UINT framebufferWidth, UINT framebufferHeight);
	AttachmentAlias* FindAttachmentAlias(UINT framebufferWidth, UINT framebufferHeight);
	ID3D11Resource* RenderBackingResource() const;
	UINT RenderBackingSubresource() const;
	void CopyRenderBackingToAttachment(AttachmentAlias& alias);
	void CopyAttachmentToRenderBacking(AttachmentAlias& alias);
	bool CopySubresourcesRaw(ID3D11Resource* source, DXGI_FORMAT sourceFormat,
		UINT sourceFirstMip, UINT sourceFirstSlice, UINT sourceMipLevels,
		ID3D11Resource* destination,
		UINT destinationFirstMip, UINT destinationFirstSlice, UINT destinationMipLevels);
	ComPtr<ID3D11ShaderResourceView> m_srv;
	ComPtr<ID3D11RenderTargetView> m_rtv;
	ComPtr<ID3D11DepthStencilView> m_dsv;
	DXGI_FORMAT m_rtvFormat{ DXGI_FORMAT_UNKNOWN };
	ComPtr<ID3D11Resource> m_aliasResource;
	std::vector<AliasStagingResource> m_aliasStagingResources;
	std::vector<AttachmentAlias> m_attachmentAliases;
	bool m_incompatibleAlias{};
	UINT m_aliasSliceCount{ 1 };
	UINT m_attachmentWriterWidth{};
	UINT m_attachmentWriterHeight{};
	uint64 m_syncedVersion{ (std::numeric_limits<uint64>::max)() };
};

class D3D11Texture final : public LatteTexture
{
public:
	D3D11Texture(D3D11Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress,
		Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch,
		uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
		: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch,
			mipLevels, swizzle, tileMode, isDepth), m_renderer(renderer)
	{
		// Texture rules are evaluated by the LatteTexture constructor. OpenGL
		// allocates the overridden format and Vulkan views assume that the image
		// already has it; using the original GX2 format here made D3D11 graphic
		// pack replacements either fail view creation or render with the wrong
		// numeric interpretation.
		const auto effectiveFormat = overwriteInfo.hasFormatOverwrite ?
			static_cast<Latte::E_GX2SURFFMT>(overwriteInfo.format) : format;
		hasStencil = LatteTexture_GX2FormatHasStencil(isDepth, effectiveFormat);
		m_format = GetFormatInfo(effectiveFormat, isDepth);
	}

	void AllocateOnHost() override
	{
		if (m_resource)
			return;
		const uint32 logicalWidth = EffectiveWidth();
		const uint32 logicalHeight = EffectiveHeight();
		const uint32 logicalDepth = EffectiveDepth();
		const UINT effectiveMipLevels = EffectiveMipLevels();
		const uint32 nativeWidth = m_format.compressed ?
			((logicalWidth + m_format.blockWidth - 1) / m_format.blockWidth) * m_format.blockWidth :
			logicalWidth;
		const uint32 nativeHeight = m_format.compressed ?
			((logicalHeight + m_format.blockHeight - 1) / m_format.blockHeight) * m_format.blockHeight :
			logicalHeight;
		const UINT bindFlags = D3D11_BIND_SHADER_RESOURCE |
			(isDepth ? D3D11_BIND_DEPTH_STENCIL :
				(m_format.rtv != DXGI_FORMAT_UNKNOWN ? D3D11_BIND_RENDER_TARGET : 0));

		if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY)
		{
			D3D11_TEXTURE1D_DESC desc{};
			desc.Width = nativeWidth;
			desc.MipLevels = effectiveMipLevels;
			desc.ArraySize = dim == Latte::E_DIM::DIM_1D_ARRAY ? logicalDepth : 1;
			desc.Format = m_format.resource;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = bindFlags;
			ThrowIfFailed(m_renderer->GetDevice()->CreateTexture1D(&desc, nullptr, &m_texture1D),
				"CreateTexture1D");
			m_resource = m_texture1D;
			return;
		}

		if (dim == Latte::E_DIM::DIM_3D)
		{
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = nativeWidth;
			desc.Height = nativeHeight;
			desc.Depth = logicalDepth;
			desc.MipLevels = effectiveMipLevels;
			desc.Format = m_format.resource;
			desc.Usage = D3D11_USAGE_DEFAULT;
			// D3D11 cannot create a depth-stencil Texture3D. GX2 does not expose
			// that combination either, so fail explicitly instead of aliasing
			// depth slices as a 2D array.
			if (isDepth)
				throw std::runtime_error("D3D11 does not support 3D depth-stencil textures");
			desc.BindFlags = bindFlags;
			ThrowIfFailed(m_renderer->GetDevice()->CreateTexture3D(&desc, nullptr, &m_texture3D),
				"CreateTexture3D");
			m_resource = m_texture3D;
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		// BC resources are allocated in complete compression blocks. GX2 keeps
		// the logical dimensions separately and commonly uses sizes such as
		// 130x130, while D3D11 rejects those dimensions for a BC resource.
		desc.Width = nativeWidth;
		desc.Height = nativeHeight;
		desc.MipLevels = effectiveMipLevels;
		// LatteTexture::depth is the layer count for every non-3D image, not only
		// for resources whose original GX2 dimension explicitly says ARRAY. Vulkan
		// allocates them this way as well, because later views may reinterpret a 2D
		// allocation as an array or cubemap.
		desc.ArraySize = dim == Latte::E_DIM::DIM_CUBEMAP ? (std::max)(logicalDepth, 6u) :
			logicalDepth;
		desc.Format = m_format.resource;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		const bool cubeCompatible = dim != Latte::E_DIM::DIM_1D &&
			dim != Latte::E_DIM::DIM_1D_ARRAY && desc.ArraySize >= 6 &&
			(desc.ArraySize % 6) == 0 && nativeWidth == nativeHeight;
		desc.MiscFlags = cubeCompatible ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		ThrowIfFailed(m_renderer->GetDevice()->CreateTexture2D(&desc, nullptr, &m_texture2D), "CreateTexture2D");
		m_resource = m_texture2D;
	}

	ID3D11Resource* Resource() const { return m_resource.Get(); }
	ID3D11Texture1D* Texture1D() const { return m_texture1D.Get(); }
	ID3D11Texture2D* Texture2D() const { return m_texture2D.Get(); }
	ID3D11Texture3D* Texture3D() const { return m_texture3D.Get(); }
	const FormatInfo& NativeFormat() const { return m_format; }
	D3D11Renderer* Owner() const { return m_renderer; }
	uint32 EffectiveWidth() const
	{
		const sint32 value = overwriteInfo.hasResolutionOverwrite ? overwriteInfo.width : width;
		return static_cast<uint32>((std::max)(value, 1));
	}
	uint32 EffectiveHeight() const
	{
		const sint32 value = overwriteInfo.hasResolutionOverwrite ? overwriteInfo.height : height;
		return static_cast<uint32>((std::max)(value, 1));
	}
	uint32 EffectiveDepth() const
	{
		const sint32 value = overwriteInfo.hasResolutionOverwrite ? overwriteInfo.depth : depth;
		return static_cast<uint32>((std::max)(value, 1));
	}
	UINT EffectiveMipLevels() const
	{
		return static_cast<UINT>((std::max)((std::min)(mipLevels, maxPossibleMipLevels), 1));
	}
	void CommitAliasWriter()
	{
		if (!m_aliasWriter)
			return;
		auto* writer = m_aliasWriter;
		m_aliasWriter = nullptr;
		writer->CopyAliasToBase();
		++m_contentVersion;
	}
	void BeginAliasWrite(D3D11TextureView* view)
	{
		if (m_aliasWriter != view)
			CommitAliasWriter();
		m_aliasWriter = view;
	}
	void BeginNativeWrite()
	{
		CommitAliasWriter();
		++m_contentVersion;
	}
	void ReleaseAliasView(D3D11TextureView* view)
	{
		if (m_aliasWriter == view)
			CommitAliasWriter();
	}
	bool IsAliasWriter(const D3D11TextureView* view) const { return m_aliasWriter == view; }
	uint64 ContentVersion() const { return m_contentVersion; }

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format,
		sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override
	{
		return new D3D11TextureView(this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
	}
private:
	D3D11Renderer* m_renderer;
	FormatInfo m_format;
	ComPtr<ID3D11Resource> m_resource;
	ComPtr<ID3D11Texture1D> m_texture1D;
	ComPtr<ID3D11Texture2D> m_texture2D;
	ComPtr<ID3D11Texture3D> m_texture3D;
	D3D11TextureView* m_aliasWriter{};
	uint64 m_contentVersion{};
};

D3D11TextureView::D3D11TextureView(D3D11Texture* texture, Latte::E_DIM dim,
	Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format)
{
	texture->AllocateOnHost();
	const auto& baseNative = texture->NativeFormat();
	const bool formatOverwritten = texture->overwriteInfo.hasFormatOverwrite;
	const auto viewNative = GetFormatInfo(format, texture->isDepth);
	// GX2 aliases the same allocation through views with different numeric
	// interpretations. Match Vulkan's mutable images by using the requested
	// LatteTextureView format instead of silently inheriting the base format.
	// D3D11 only accepts reinterpretation inside the same typeless family.
	const auto& native = (texture->isDepth || formatOverwritten) ? baseNative : viewNative;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = native.srv;
	if (dim == Latte::E_DIM::DIM_1D)
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
		srv.Texture1D.MostDetailedMip = firstMip;
		srv.Texture1D.MipLevels = mipCount;
	}
	else if (dim == Latte::E_DIM::DIM_1D_ARRAY)
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
		srv.Texture1DArray.MostDetailedMip = firstMip;
		srv.Texture1DArray.MipLevels = mipCount;
		srv.Texture1DArray.FirstArraySlice = firstSlice;
		srv.Texture1DArray.ArraySize = sliceCount;
	}
	else if (dim == Latte::E_DIM::DIM_3D)
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srv.Texture3D.MostDetailedMip = firstMip;
		srv.Texture3D.MipLevels = mipCount;
	}
	else if (dim == Latte::E_DIM::DIM_CUBEMAP)
	{
		// Vulkan always exposes GX2 cubemaps as cube arrays. A single cube is
		// still legal through TEXTURECUBE, but titles can create views that
		// start at a later cube or span more than six faces.
		if (texture->EffectiveDepth() > 6 || firstSlice >= 6 || sliceCount > 6)
		{
			const UINT firstFace = static_cast<UINT>((std::max)(firstSlice, 0));
			const UINT availableFaces = static_cast<UINT>((std::max)(
				static_cast<sint32>(texture->EffectiveDepth()) - firstSlice, 0));
			const UINT requestedFaces = static_cast<UINT>((std::max)(sliceCount, 0));
			const UINT faceCount = (std::min)(availableFaces, requestedFaces);
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			srv.TextureCubeArray.MostDetailedMip = firstMip;
			srv.TextureCubeArray.MipLevels = mipCount;
			srv.TextureCubeArray.First2DArrayFace = firstFace;
			srv.TextureCubeArray.NumCubes = (std::max)(faceCount / 6u, 1u);
		}
		else
		{
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			srv.TextureCube.MostDetailedMip = firstMip;
			srv.TextureCube.MipLevels = mipCount;
		}
	}
	else if (texture->EffectiveDepth() > 1 || dim == Latte::E_DIM::DIM_2D_ARRAY)
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srv.Texture2DArray.MostDetailedMip = firstMip;
		srv.Texture2DArray.MipLevels = mipCount;
		srv.Texture2DArray.FirstArraySlice = firstSlice;
		srv.Texture2DArray.ArraySize = sliceCount;
	}
	else
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MostDetailedMip = firstMip;
		srv.Texture2D.MipLevels = mipCount;
	}
	HRESULT srvResult = texture->Owner()->GetDevice()->CreateShaderResourceView(
		texture->Resource(), &srv, &m_srv);
	if (FAILED(srvResult) && !texture->isDepth && native.srv != baseNative.srv)
	{
		// A mutable GX2 allocation may cross DXGI typeless families. Such a view
		// is illegal in D3D11, so back it with a synchronized shadow resource in
		// the requested family instead of silently sampling the base format.
		if (CreateIncompatibleAlias(viewNative))
			srvResult = S_OK;
		else
		{
			srv.Format = baseNative.srv;
			srvResult = texture->Owner()->GetDevice()->CreateShaderResourceView(
				texture->Resource(), &srv, &m_srv);
		}
	}
	ThrowIfFailed(srvResult, "CreateShaderResourceView");

	if (texture->isDepth)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
		dsv.Format = native.dsv;
		if (dim == Latte::E_DIM::DIM_1D_ARRAY)
		{
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
			dsv.Texture1DArray.MipSlice = firstMip;
			dsv.Texture1DArray.FirstArraySlice = firstSlice;
			dsv.Texture1DArray.ArraySize = sliceCount;
		}
		else if (dim == Latte::E_DIM::DIM_1D)
		{
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
			dsv.Texture1D.MipSlice = firstMip;
		}
		else if (texture->EffectiveDepth() > 1)
		{
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			dsv.Texture2DArray.MipSlice = firstMip;
			dsv.Texture2DArray.FirstArraySlice = firstSlice;
			dsv.Texture2DArray.ArraySize = sliceCount;
		}
		else
		{
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			dsv.Texture2D.MipSlice = firstMip;
		}
		ThrowIfFailed(texture->Owner()->GetDevice()->CreateDepthStencilView(texture->Resource(), &dsv, &m_dsv), "CreateDepthStencilView");
	}
	else if (!m_incompatibleAlias && native.rtv != DXGI_FORMAT_UNKNOWN)
	{
		D3D11_RENDER_TARGET_VIEW_DESC rtv{};
		rtv.Format = native.rtv;
		if (dim == Latte::E_DIM::DIM_1D_ARRAY)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
			rtv.Texture1DArray.MipSlice = firstMip;
			rtv.Texture1DArray.FirstArraySlice = firstSlice;
			// CB_COLORn_VIEW selects one array slice for a color attachment.  The
			// texture cache can nevertheless return its broader base view (most
			// visibly for face zero of a cubemap).  Binding that broad RTV beside
			// the five single-face RTVs makes the subresources overlap and D3D11
			// rejects the complete MRT set.  Keep the SRV broad, but make the RTV
			// describe only the render-target slice selected by GX2.
			rtv.Texture1DArray.ArraySize = 1;
		}
		else if (dim == Latte::E_DIM::DIM_1D)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
			rtv.Texture1D.MipSlice = firstMip;
		}
		else if (dim == Latte::E_DIM::DIM_3D)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
			rtv.Texture3D.MipSlice = firstMip;
			rtv.Texture3D.FirstWSlice = firstSlice;
			rtv.Texture3D.WSize = 1;
		}
		else if (texture->EffectiveDepth() > 1)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.MipSlice = firstMip;
			rtv.Texture2DArray.FirstArraySlice = firstSlice;
			rtv.Texture2DArray.ArraySize = 1;
		}
		else
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = firstMip;
		}
		HRESULT rtvResult = texture->Owner()->GetDevice()->CreateRenderTargetView(
			texture->Resource(), &rtv, &m_rtv);
		if (FAILED(rtvResult) && native.rtv != baseNative.rtv)
		{
			rtv.Format = baseNative.rtv;
			rtvResult = texture->Owner()->GetDevice()->CreateRenderTargetView(
				texture->Resource(), &rtv, &m_rtv);
		}
		ThrowIfFailed(rtvResult, "CreateRenderTargetView");
		m_rtvFormat = rtv.Format;
	}
}

D3D11TextureView::~D3D11TextureView()
{
	static_cast<D3D11Texture*>(baseTexture)->ReleaseAliasView(this);
}

bool D3D11TextureView::CreateIncompatibleAlias(const FormatInfo& requested)
{
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	const auto& base = texture->NativeFormat();
	if (!texture->Texture2D() || texture->isDepth || requested.srv == DXGI_FORMAT_UNKNOWN ||
		base.bytesPerBlock != requested.bytesPerBlock || base.blockWidth != requested.blockWidth ||
		base.blockHeight != requested.blockHeight)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 cannot create raw mutable alias from DXGI format {} to {} because the storage geometry differs",
			static_cast<uint32>(base.resource), static_cast<uint32>(requested.resource));
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = (std::max)(texture->EffectiveWidth() >> firstMip, 1u);
	desc.Height = (std::max)(texture->EffectiveHeight() >> firstMip, 1u);
	if (requested.compressed)
	{
		desc.Width = ((desc.Width + requested.blockWidth - 1) / requested.blockWidth) * requested.blockWidth;
		desc.Height = ((desc.Height + requested.blockHeight - 1) / requested.blockHeight) * requested.blockHeight;
	}
	desc.MipLevels = static_cast<UINT>((std::max)(numMip, 1));
	desc.ArraySize = dim == Latte::E_DIM::DIM_CUBEMAP ?
		static_cast<UINT>((std::max)(numSlice, 6)) :
		static_cast<UINT>((std::max)(numSlice, 1));
	desc.Format = requested.resource;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		(requested.rtv != DXGI_FORMAT_UNKNOWN ? D3D11_BIND_RENDER_TARGET : 0);
	desc.MiscFlags = dim == Latte::E_DIM::DIM_CUBEMAP && desc.ArraySize >= 6 &&
		(desc.ArraySize % 6) == 0 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
	ComPtr<ID3D11Texture2D> alias;
	if (FAILED(texture->Owner()->GetDevice()->CreateTexture2D(&desc, nullptr, &alias)))
		return false;
	m_aliasResource = alias;

	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = requested.srv;
	if (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
	{
		if (desc.ArraySize > 6)
		{
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			srv.TextureCubeArray.MostDetailedMip = 0;
			srv.TextureCubeArray.MipLevels = desc.MipLevels;
			srv.TextureCubeArray.First2DArrayFace = 0;
			srv.TextureCubeArray.NumCubes = desc.ArraySize / 6;
		}
		else
		{
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			srv.TextureCube.MostDetailedMip = 0;
			srv.TextureCube.MipLevels = desc.MipLevels;
		}
	}
	else if (desc.ArraySize > 1)
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srv.Texture2DArray.MostDetailedMip = 0;
		srv.Texture2DArray.MipLevels = desc.MipLevels;
		srv.Texture2DArray.FirstArraySlice = 0;
		srv.Texture2DArray.ArraySize = desc.ArraySize;
	}
	else
	{
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MostDetailedMip = 0;
		srv.Texture2D.MipLevels = desc.MipLevels;
	}
	if (FAILED(texture->Owner()->GetDevice()->CreateShaderResourceView(alias.Get(), &srv, &m_srv)))
		return false;
	if (requested.rtv != DXGI_FORMAT_UNKNOWN)
	{
		D3D11_RENDER_TARGET_VIEW_DESC rtv{};
		rtv.Format = requested.rtv;
		if (desc.ArraySize > 1)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.MipSlice = 0;
			rtv.Texture2DArray.FirstArraySlice = 0;
			rtv.Texture2DArray.ArraySize = 1;
		}
		else
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = 0;
		}
		if (FAILED(texture->Owner()->GetDevice()->CreateRenderTargetView(alias.Get(), &rtv, &m_rtv)))
			return false;
		m_rtvFormat = rtv.Format;
	}
	m_incompatibleAlias = true;
	m_aliasSliceCount = desc.ArraySize;
	cemuLog_logOnce(LogType::Force,
		"D3D11 mutable GX2 alias uses synchronized shadow resource (DXGI {} -> {})",
		static_cast<uint32>(base.resource), static_cast<uint32>(requested.resource));
	return true;
}

ID3D11Resource* D3D11TextureView::RenderBackingResource() const
{
	return m_incompatibleAlias ? m_aliasResource.Get() :
		static_cast<D3D11Texture*>(baseTexture)->Resource();
}

UINT D3D11TextureView::RenderBackingSubresource() const
{
	if (m_incompatibleAlias)
		return 0;
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	return D3D11CalcSubresource(static_cast<UINT>((std::max)(firstMip, 0)),
		static_cast<UINT>((std::max)(firstSlice, 0)), texture->EffectiveMipLevels());
}

D3D11TextureView::AttachmentAlias* D3D11TextureView::FindAttachmentAlias(
	UINT framebufferWidth, UINT framebufferHeight)
{
	auto it = std::find_if(m_attachmentAliases.begin(), m_attachmentAliases.end(),
		[framebufferWidth, framebufferHeight](const AttachmentAlias& alias) {
			return alias.width == framebufferWidth && alias.height == framebufferHeight;
		});
	return it == m_attachmentAliases.end() ? nullptr : &*it;
}

D3D11TextureView::AttachmentAlias* D3D11TextureView::GetOrCreateAttachmentAlias(
	UINT framebufferWidth, UINT framebufferHeight)
{
	if (!m_rtv || framebufferWidth == 0 || framebufferHeight == 0)
		return nullptr;
	if (auto* existing = FindAttachmentAlias(framebufferWidth, framebufferHeight))
		return existing;

	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	const UINT viewWidth = (std::max)(texture->EffectiveWidth() >>
		static_cast<UINT>((std::max)(firstMip, 0)), 1u);
	const UINT viewHeight = (std::max)(texture->EffectiveHeight() >>
		static_cast<UINT>((std::max)(firstMip, 0)), 1u);
	if (framebufferWidth == viewWidth && framebufferHeight == viewHeight)
		return nullptr;
	if (framebufferWidth > viewWidth || framebufferHeight > viewHeight)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 cannot expand color attachment {}x{} to framebuffer {}x{}",
			viewWidth, viewHeight, framebufferWidth, framebufferHeight);
		return nullptr;
	}

	ComPtr<ID3D11Texture2D> backingTexture;
	if (FAILED(RenderBackingResource()->QueryInterface(IID_PPV_ARGS(&backingTexture))))
		return nullptr;
	D3D11_TEXTURE2D_DESC backingDesc{};
	backingTexture->GetDesc(&backingDesc);
	if (backingDesc.SampleDesc.Count != 1)
		return nullptr;

	D3D11_RENDER_TARGET_VIEW_DESC sourceViewDesc{};
	m_rtv->GetDesc(&sourceViewDesc);
	if (sourceViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
		sourceViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
		return nullptr;

	D3D11_TEXTURE2D_DESC aliasDesc{};
	aliasDesc.Width = framebufferWidth;
	aliasDesc.Height = framebufferHeight;
	aliasDesc.MipLevels = 1;
	aliasDesc.ArraySize = sourceViewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ?
		(std::max)(sourceViewDesc.Texture2DArray.ArraySize, 1u) : 1u;
	aliasDesc.Format = backingDesc.Format;
	aliasDesc.SampleDesc = backingDesc.SampleDesc;
	aliasDesc.Usage = D3D11_USAGE_DEFAULT;
	aliasDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

	AttachmentAlias alias{};
	alias.width = framebufferWidth;
	alias.height = framebufferHeight;
	if (FAILED(texture->Owner()->GetDevice()->CreateTexture2D(
		&aliasDesc, nullptr, &alias.texture)))
		return nullptr;

	D3D11_RENDER_TARGET_VIEW_DESC aliasViewDesc{};
	aliasViewDesc.Format = m_rtvFormat;
	aliasViewDesc.ViewDimension = sourceViewDesc.ViewDimension;
	if (aliasViewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
	{
		aliasViewDesc.Texture2DArray.MipSlice = 0;
		aliasViewDesc.Texture2DArray.FirstArraySlice = 0;
		aliasViewDesc.Texture2DArray.ArraySize = aliasDesc.ArraySize;
	}
	else
		aliasViewDesc.Texture2D.MipSlice = 0;
	if (FAILED(texture->Owner()->GetDevice()->CreateRenderTargetView(
		alias.texture.Get(), &aliasViewDesc, &alias.rtv)))
		return nullptr;

	m_attachmentAliases.emplace_back(std::move(alias));
	cemuLog_logOnce(LogType::Force,
		"D3D11 color attachment {}x{} uses synchronized framebuffer alias {}x{}",
		viewWidth, viewHeight, framebufferWidth, framebufferHeight);
	return &m_attachmentAliases.back();
}

void D3D11TextureView::CopyRenderBackingToAttachment(AttachmentAlias& alias)
{
	D3D11_BOX box{ 0, 0, 0, alias.width, alias.height, 1 };
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	texture->Owner()->GetContext()->CopySubresourceRegion(alias.texture.Get(), 0,
		0, 0, 0, RenderBackingResource(), RenderBackingSubresource(), &box);
	alias.syncedVersion = texture->ContentVersion();
}

void D3D11TextureView::CopyAttachmentToRenderBacking(AttachmentAlias& alias)
{
	D3D11_BOX box{ 0, 0, 0, alias.width, alias.height, 1 };
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	texture->Owner()->GetContext()->CopySubresourceRegion(RenderBackingResource(),
		RenderBackingSubresource(), 0, 0, 0, alias.texture.Get(), 0, &box);
	// CommitAliasWriter increments the content version immediately after this
	// copy, so the alias already represents that next version.
	alias.syncedVersion = texture->ContentVersion() + 1;
}

bool D3D11TextureView::CopySubresourcesRaw(ID3D11Resource* source, DXGI_FORMAT sourceFormat,
	UINT sourceFirstMip, UINT sourceFirstSlice, UINT sourceMipLevels,
	ID3D11Resource* destination,
	UINT destinationFirstMip, UINT destinationFirstSlice, UINT destinationMipLevels)
{
	ComPtr<ID3D11Texture2D> sourceTexture;
	ComPtr<ID3D11Texture2D> destinationTexture;
	if (FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceTexture))) ||
		FAILED(destination->QueryInterface(IID_PPV_ARGS(&destinationTexture))))
		return false;
	D3D11_TEXTURE2D_DESC sourceDesc{};
	D3D11_TEXTURE2D_DESC destinationDesc{};
	sourceTexture->GetDesc(&sourceDesc);
	destinationTexture->GetDesc(&destinationDesc);
	const UINT copyMipCount = (std::min)({ static_cast<UINT>((std::max)(numMip, 1)),
		sourceMipLevels > sourceFirstMip ? sourceMipLevels - sourceFirstMip : 0,
		destinationMipLevels > destinationFirstMip ? destinationMipLevels - destinationFirstMip : 0 });
	const UINT copySliceCount = (std::min)({ m_aliasSliceCount,
		sourceDesc.ArraySize > sourceFirstSlice ? sourceDesc.ArraySize - sourceFirstSlice : 0,
		destinationDesc.ArraySize > destinationFirstSlice ? destinationDesc.ArraySize - destinationFirstSlice : 0 });
	if (copyMipCount == 0 || copySliceCount == 0)
		return false;
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	auto* context = texture->Owner()->GetContext();
	for (UINT slice = 0; slice < copySliceCount; ++slice)
	{
		for (UINT mip = 0; mip < copyMipCount; ++mip)
		{
			D3D11_TEXTURE2D_DESC stagingDesc{};
			stagingDesc.Width = (std::max)(sourceDesc.Width >> (sourceFirstMip + mip), 1u);
			stagingDesc.Height = (std::max)(sourceDesc.Height >> (sourceFirstMip + mip), 1u);
			stagingDesc.MipLevels = 1;
			stagingDesc.ArraySize = 1;
			stagingDesc.Format = sourceFormat;
			stagingDesc.SampleDesc.Count = 1;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Texture2D* staging{};
			auto stagingIt = std::find_if(m_aliasStagingResources.begin(),
				m_aliasStagingResources.end(), [&stagingDesc](const auto& candidate) {
					return std::memcmp(&candidate.desc, &stagingDesc, sizeof(stagingDesc)) == 0;
				});
			if (stagingIt == m_aliasStagingResources.end())
			{
				AliasStagingResource candidate{};
				candidate.desc = stagingDesc;
				if (FAILED(texture->Owner()->GetDevice()->CreateTexture2D(
					&candidate.desc, nullptr, &candidate.texture)))
					return false;
				m_aliasStagingResources.emplace_back(std::move(candidate));
				staging = m_aliasStagingResources.back().texture.Get();
			}
			else
				staging = stagingIt->texture.Get();
			context->CopySubresourceRegion(staging, 0, 0, 0, 0, source,
				D3D11CalcSubresource(sourceFirstMip + mip, sourceFirstSlice + slice,
					sourceMipLevels), nullptr);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
				return false;
			context->UpdateSubresource(destination,
				D3D11CalcSubresource(destinationFirstMip + mip, destinationFirstSlice + slice,
					destinationMipLevels), nullptr, mapped.pData, mapped.RowPitch, mapped.DepthPitch);
			context->Unmap(staging, 0);
		}
	}
	return true;
}

void D3D11TextureView::PrepareForSampling()
{
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	texture->CommitAliasWriter();
	if (!m_incompatibleAlias || m_syncedVersion == texture->ContentVersion())
		return;
	ComPtr<ID3D11Texture2D> aliasTexture;
	if (FAILED(m_aliasResource.As(&aliasTexture)))
		return;
	D3D11_TEXTURE2D_DESC aliasDesc{};
	aliasTexture->GetDesc(&aliasDesc);
	if (CopySubresourcesRaw(texture->Resource(), texture->NativeFormat().resource,
		firstMip, firstSlice, texture->EffectiveMipLevels(), m_aliasResource.Get(),
		0, 0, aliasDesc.MipLevels))
		m_syncedVersion = texture->ContentVersion();
}

void D3D11TextureView::PrepareForRenderTarget()
{
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	if (!m_incompatibleAlias)
	{
		texture->BeginNativeWrite();
		return;
	}
	PrepareForSampling();
	texture->BeginAliasWrite(this);
}

ID3D11RenderTargetView* D3D11TextureView::FramebufferRTV(
	UINT framebufferWidth, UINT framebufferHeight)
{
	if (auto* alias = GetOrCreateAttachmentAlias(framebufferWidth, framebufferHeight))
		return alias->rtv.Get();
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	const UINT viewWidth = (std::max)(texture->EffectiveWidth() >>
		static_cast<UINT>((std::max)(firstMip, 0)), 1u);
	const UINT viewHeight = (std::max)(texture->EffectiveHeight() >>
		static_cast<UINT>((std::max)(firstMip, 0)), 1u);
	if (viewWidth != framebufferWidth || viewHeight != framebufferHeight)
	{
		// A null hole preserves the remaining MRTs. Falling back to the oversized
		// native RTV would make D3D11 reject the complete output-merger set.
		cemuLog_logOnce(LogType::Force,
			"D3D11 omitted incompatible color attachment {}x{} for framebuffer {}x{} because its alias could not be created",
			viewWidth, viewHeight, framebufferWidth, framebufferHeight);
		return nullptr;
	}
	return m_rtv.Get();
}

void D3D11TextureView::PrepareForRenderTarget(
	UINT framebufferWidth, UINT framebufferHeight)
{
	auto* alias = FindAttachmentAlias(framebufferWidth, framebufferHeight);
	if (!alias)
	{
		PrepareForRenderTarget();
		return;
	}

	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	if (texture->IsAliasWriter(this) &&
		m_attachmentWriterWidth == framebufferWidth &&
		m_attachmentWriterHeight == framebufferHeight)
		return;

	// This commits a different writer (or a differently-sized alias owned by
	// this view) and refreshes a mutable-format backing resource when required.
	PrepareForSampling();
	alias = FindAttachmentAlias(framebufferWidth, framebufferHeight);
	if (!alias)
		return;
	if (alias->syncedVersion != texture->ContentVersion())
		CopyRenderBackingToAttachment(*alias);
	m_attachmentWriterWidth = framebufferWidth;
	m_attachmentWriterHeight = framebufferHeight;
	texture->BeginAliasWrite(this);
}

void D3D11TextureView::CopyAliasToBase()
{
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	if (m_attachmentWriterWidth != 0 && m_attachmentWriterHeight != 0)
	{
		if (auto* alias = FindAttachmentAlias(
			m_attachmentWriterWidth, m_attachmentWriterHeight))
			CopyAttachmentToRenderBacking(*alias);
		m_attachmentWriterWidth = 0;
		m_attachmentWriterHeight = 0;
	}
	if (!m_incompatibleAlias)
		return;
	ComPtr<ID3D11Texture2D> aliasTexture;
	if (FAILED(m_aliasResource.As(&aliasTexture)))
		return;
	D3D11_TEXTURE2D_DESC aliasDesc{};
	aliasTexture->GetDesc(&aliasDesc);
	CopySubresourcesRaw(m_aliasResource.Get(), aliasDesc.Format, 0, 0, aliasDesc.MipLevels,
		texture->Resource(), firstMip, firstSlice,
		texture->EffectiveMipLevels());
	m_syncedVersion = texture->ContentVersion() + 1;
}

class D3D11CachedFBO final : public LatteCachedFBO
{
public:
	explicit D3D11CachedFBO(uint64 key) : LatteCachedFBO(key) {}

	std::array<ID3D11RenderTargetView*, 8> targets{};
	ID3D11DepthStencilView* depth{};
	std::array<bool, 8> blendable{ true, true, true, true, true, true, true, true };
	UINT targetCount{};
	bool nativeViewsCached{};
};

class D3D11Readback final : public LatteTextureReadbackInfo
{
public:
	D3D11Readback(D3D11Renderer* renderer, D3D11TextureView* view)
		: LatteTextureReadbackInfo(view), m_renderer(renderer), m_view(view) {}
	void StartTransfer() override
	{
		auto* texture = static_cast<D3D11Texture*>(m_view->baseTexture);
		m_view->PrepareForSampling();
		texture->AllocateOnHost();
		const UINT subresource = D3D11CalcSubresource(m_view->firstMip, m_view->firstSlice,
			texture->EffectiveMipLevels());
		if (auto* sourceTexture = texture->Texture1D())
		{
			D3D11_TEXTURE1D_DESC desc{};
			sourceTexture->GetDesc(&desc);
			desc.Width = (std::max)(1u, desc.Width >> m_view->firstMip);
			desc.MipLevels = desc.ArraySize = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ThrowIfFailed(m_renderer->GetDevice()->CreateTexture1D(&desc, nullptr, &m_staging1D),
				"Create 1D readback texture");
			m_staging = m_staging1D;
			m_width = desc.Width;
			m_height = m_depth = 1;
		}
		else if (auto* sourceTexture = texture->Texture2D())
		{
			D3D11_TEXTURE2D_DESC desc{};
			sourceTexture->GetDesc(&desc);
			desc.Width = (std::max)(1u, desc.Width >> m_view->firstMip);
			desc.Height = (std::max)(1u, desc.Height >> m_view->firstMip);
			desc.MipLevels = desc.ArraySize = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ThrowIfFailed(m_renderer->GetDevice()->CreateTexture2D(&desc, nullptr, &m_staging2D),
				"Create 2D readback texture");
			m_staging = m_staging2D;
			m_width = desc.Width;
			m_height = desc.Height;
			m_depth = 1;
		}
		else if (auto* sourceTexture = texture->Texture3D())
		{
			D3D11_TEXTURE3D_DESC desc{};
			sourceTexture->GetDesc(&desc);
			desc.Width = (std::max)(1u, desc.Width >> m_view->firstMip);
			desc.Height = (std::max)(1u, desc.Height >> m_view->firstMip);
			const UINT sourceDepth = (std::max)(1u, desc.Depth >> m_view->firstMip);
			if (m_view->firstSlice >= sourceDepth)
				throw std::runtime_error("D3D11 readback 3D slice is outside the selected mip");
			desc.Depth = 1;
			desc.MipLevels = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ThrowIfFailed(m_renderer->GetDevice()->CreateTexture3D(&desc, nullptr, &m_staging3D),
				"Create 3D readback texture");
			m_staging = m_staging3D;
			m_width = desc.Width;
			m_height = desc.Height;
			m_depth = 1;
			D3D11_BOX sliceBox{ 0, 0, m_view->firstSlice, desc.Width, desc.Height,
				m_view->firstSlice + 1 };
			m_renderer->GetContext()->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0,
				texture->Resource(), m_view->firstMip, &sliceBox);
		}
		else
			throw std::runtime_error("D3D11 readback received an unsupported texture resource");
		if (!texture->Texture3D())
			m_renderer->GetContext()->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0,
				texture->Resource(), subresource, nullptr);
		D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
		ThrowIfFailed(m_renderer->GetDevice()->CreateQuery(&queryDesc, &m_event), "Create readback event");
		m_renderer->GetContext()->End(m_event.Get());
		m_started = true;
	}
	bool IsFinished() override
	{
		return m_started && m_renderer->GetContext()->GetData(m_event.Get(), nullptr, 0,
			D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
	}
	void ForceFinish() override
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (m_started)
		{
			const HRESULT status = m_renderer->GetContext()->GetData(m_event.Get(), nullptr, 0,
				D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (status == S_OK)
				return;
			if (status != S_FALSE)
				ThrowIfFailed(status, "Wait for readback event");
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("D3D11 texture readback timed out after 5 seconds");
			std::this_thread::yield();
		}
	}
	uint8* GetData() override
	{
		if (!m_started)
			StartTransfer();
		ForceFinish();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		ThrowIfFailed(m_renderer->GetContext()->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map readback texture");
		const FormatInfo info = GetFormatInfo(m_view->format, m_view->baseTexture->isDepth);
		const uint32 rowSize = RowPitch(info, m_width);
		const uint32 rows = RowCount(info, m_height);
		const size_t sliceSize = static_cast<size_t>(rowSize) * rows;
		m_data.resize(sliceSize * m_depth);
		for (uint32 slice = 0; slice < m_depth; ++slice)
			for (uint32 row = 0; row < rows; ++row)
				std::memcpy(m_data.data() + static_cast<size_t>(slice) * sliceSize +
					static_cast<size_t>(row) * rowSize,
					static_cast<const uint8*>(mapped.pData) + static_cast<size_t>(slice) * mapped.DepthPitch +
					static_cast<size_t>(row) * mapped.RowPitch, rowSize);
		m_renderer->GetContext()->Unmap(m_staging.Get(), 0);
		return m_data.data();
	}
private:
	D3D11Renderer* m_renderer;
	D3D11TextureView* m_view;
	bool m_started{};
	std::vector<uint8> m_data;
	UINT m_width{};
	UINT m_height{};
	UINT m_depth{};
	ComPtr<ID3D11Resource> m_staging;
	ComPtr<ID3D11Texture1D> m_staging1D;
	ComPtr<ID3D11Texture2D> m_staging2D;
	ComPtr<ID3D11Texture3D> m_staging3D;
	ComPtr<ID3D11Query> m_event;
};

struct IndexBufferAllocation
{
	std::vector<uint8> data;
	ComPtr<ID3D11Buffer> buffer;
	UINT offset{};
};

DXGI_FORMAT VertexFormat(uint8 format)
{
	switch (format & 0x3F)
	{
	case FMT_8: return DXGI_FORMAT_R8_UINT;
	case FMT_8_8: return DXGI_FORMAT_R8G8_UINT;
	case FMT_8_8_8: return DXGI_FORMAT_R8G8B8A8_UINT;
	case FMT_8_8_8_8: return DXGI_FORMAT_R8G8B8A8_UINT;
	case FMT_16: case FMT_16_FLOAT: return DXGI_FORMAT_R16_UINT;
	case FMT_16_16: case FMT_16_16_FLOAT: return DXGI_FORMAT_R16G16_UINT;
	// DXGI has no three-component 8/16-bit vertex formats. The fetch shader
	// emits raw uint attributes, so expose the containing four-component word;
	// the decompiler's destination selectors discard the unused component.
	case FMT_16_16_16: case FMT_16_16_16_FLOAT: return DXGI_FORMAT_R16G16B16A16_UINT;
	case FMT_16_16_16_16: case FMT_16_16_16_16_FLOAT: return DXGI_FORMAT_R16G16B16A16_UINT;
	case FMT_32: case FMT_32_FLOAT: return DXGI_FORMAT_R32_UINT;
	case FMT_32_32: case FMT_32_32_FLOAT: return DXGI_FORMAT_R32G32_UINT;
	case FMT_32_32_32: case FMT_32_32_32_FLOAT: return DXGI_FORMAT_R32G32B32_UINT;
	case FMT_32_32_32_32: case FMT_32_32_32_32_FLOAT: return DXGI_FORMAT_R32G32B32A32_UINT;
	case FMT_2_10_10_10: return DXGI_FORMAT_R32_UINT;
	default:
		cemuLog_logOnce(LogType::Force,
			"D3D11 unsupported vertex format 0x{:02X}; draw will be skipped", format);
		return DXGI_FORMAT_UNKNOWN;
	}
}

D3D11_PRIMITIVE_TOPOLOGY PrimitiveTopology(LattePrimitiveMode mode)
{
	switch (mode)
	{
	case LattePrimitiveMode::POINTS: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
	case LattePrimitiveMode::LINES: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
	case LattePrimitiveMode::LINES_ADJACENT: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
	case LattePrimitiveMode::LINE_STRIP:
	case LattePrimitiveMode::LINE_LOOP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
	case LattePrimitiveMode::LINE_STRIP_ADJACENT: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
	case LattePrimitiveMode::TRIANGLES_ADJACENT: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
	case LattePrimitiveMode::TRIANGLE_STRIP_ADJACENT: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
	// LatteIndices rewrites a fan into the alternating index order consumed by
	// a triangle strip on APIs (Metal/D3D11) without native fan topology.
	case LattePrimitiveMode::TRIANGLE_FAN:
	case LattePrimitiveMode::TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
}

ComPtr<ID3DBlob> CompileInternalShader(const char* source, const char* profile)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = CompileHLSLCached(source, std::strlen(source), profile,
		RuntimeShaderCompileFlags(), &blob, &errors);
	if (FAILED(hr))
		throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed");
	return blob;
}
}
