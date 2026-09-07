D3D11Renderer::D3D11Renderer() : Renderer(RendererAPI::D3D11)
{
	// Avoid rehashing the state caches during the first shader-heavy frames.
	// These are conservative reservations only; entries are still created lazily.
	m_inputLayoutCache.reserve(512);
	m_samplerCache.reserve(256);
	m_rasterizerCache.reserve(128);
	m_blendCache.reserve(256);
	m_depthStencilCache.reserve(128);
	m_rectShaderCache.reserve(128);
	m_reportedDebugWarnings.reserve(128);
	for (auto& stage : m_samplerSwizzles)
		for (auto& selectors : stage)
			selectors = { 0, 1, 2, 3 };
	const auto* surface = static_cast<const CemuEmbedD3D11Surface*>(WindowSystem::GetWindowInfo().canvas_main.surface);
	if (!surface || surface->struct_size < sizeof(CemuEmbedD3D11Surface) ||
		surface->abi_version != CEMU_EMBED_D3D11_SURFACE_VERSION ||
		!surface->device || !surface->immediate_context || !surface->swap_chain)
		throw std::runtime_error("The host did not provide a valid Direct3D 11 SwapChainPanel surface.");
	m_device = static_cast<ID3D11Device*>(surface->device);
	m_context = static_cast<ID3D11DeviceContext*>(surface->immediate_context);
	if (m_device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0)
		throw std::runtime_error(fmt::format(
			"The Direct3D host must provide exactly DirectX 11 Feature Level 11.0 (received 0x{:04X}).",
			static_cast<uint32>(m_device->GetFeatureLevel())));
	// Allocate this while pressure is low. Creating a query after the first draw
	// is too late because that draw can already have started xbsc_xs.dll on a
	// driver worker and consumed the remaining title budget.
	D3D11_QUERY_DESC idleQueryDesc{ D3D11_QUERY_EVENT, 0 };
	const HRESULT idleQueryResult = m_device->CreateQuery(&idleQueryDesc, &m_gpuIdleQuery);
	if (FAILED(idleQueryResult))
		cemuLog_log(LogType::Force,
			"D3D11: unable to create the reusable GPU-idle query (HRESULT 0x{:08X})",
			static_cast<uint32>(idleQueryResult));
	// The immediate context is exclusively owned by LatteThread after the host
	// hands the surface to Cemu. Enabling ID3D11Multithread here adds a lock to
	// every D3D call and is especially expensive for draw-heavy Wii U titles.
	if (SUCCEEDED(m_device.As(&m_infoQueue)))
	{
		// The debug layer raises exception 0x87A when break-on-error is enabled.
		// An embedded UWP host cannot treat that debugger-only notification as a
		// recoverable application exception, so collect the messages in log.txt.
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
		// Keep messages in ID3D11InfoQueue so CheckDebugMessages() can classify
		// and persist real renderer faults, but stop the debug layer from also
		// writing every message directly to Visual Studio's Output window. GX2
		// legitimately keeps a pixel shader active during depth-only passes, and
		// D3D11 otherwise emits DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET for every
		// such draw before our compatibility filter gets a chance to process it.
		m_infoQueue->SetMuteDebugOutput(TRUE);
		m_infoQueue->ClearStoredMessages();
	}
	m_swapChain = static_cast<IDXGISwapChain*>(surface->swap_chain);
	RefreshBackBuffer();
	cemuLog_log(LogType::Force, "------- Init Direct3D 11 graphics backend -------");
	cemuLog_log(LogType::Force, "Direct3D 11 backend: native GX2 resources, shaders, draw calls and SwapChainPanel presentation.");
	cemuLog_log(LogType::Force,
		"DirectX 11 Feature Level 11.0; transform feedback path: {}",
#if defined(CEMU_UWP)
			"pixel-UAV compatibility"
#else
			"native compatibility"
#endif
		);
}

D3D11Renderer::~D3D11Renderer() = default;

D3D11Renderer* D3D11Renderer::GetInstance()
{
	cemu_assert_debug(g_renderer && g_renderer->GetType() == RendererAPI::D3D11);
	return static_cast<D3D11Renderer*>(g_renderer.get());
}

void D3D11Renderer::InitializePresentationPipeline()
{
	static constexpr char vs[] =
		"struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
		"O main(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);"
		"o.uv=p;o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}";
	static constexpr char ps[] =
		"Texture2D t0:register(t0);SamplerState s0:register(s0);"
		"cbuffer Presentation:register(b0){"
		"float2 textureSrcResolution;float2 nativeResolution;"
		"float2 outputResolution;uint applySRGBEncoding;float targetGamma;"
		"float displayGamma;};"
		"float encodeSRGB(float v){return v<=0.0031308?12.92*v:1.055*pow(v,1.0/2.4)-0.055;}"
		"float3 encodeSRGB(float3 v){return float3(encodeSRGB(v.r),encodeSRGB(v.g),encodeSRGB(v.b));}"
		"float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
		"float3 color=t0.Sample(s0,uv).rgb;"
		"if(applySRGBEncoding!=0)color=encodeSRGB(color);"
		"if(displayGamma>0.0)color=pow(max(color,0.0),targetGamma/displayGamma);"
		"else color=encodeSRGB(pow(max(color,0.0),targetGamma));"
		"return float4(color,1.0);}";
	static constexpr char surfaceCopyColorPs[] =
		"Texture2D<float4> t0:register(t0);SamplerState s0:register(s0);"
		"float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
		"float v=t0.SampleLevel(s0,uv,0).r;return float4(v,0,0,1);}";
	static constexpr char surfaceCopyDepthPs[] =
		"Texture2D<float4> t0:register(t0);SamplerState s0:register(s0);"
		"float main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Depth{"
		"return t0.SampleLevel(s0,uv,0).r;}";
	auto vsBlob = CompileInternalShader(vs, "vs_5_0");
	auto psBlob = CompileInternalShader(ps, "ps_5_0");
	auto surfaceCopyColorPsBlob = CompileInternalShader(surfaceCopyColorPs, "ps_5_0");
	auto surfaceCopyDepthPsBlob = CompileInternalShader(surfaceCopyDepthPs, "ps_5_0");
	ThrowIfFailed(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_presentVS), "Create presentation VS");
	ThrowIfFailed(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_presentPS), "Create presentation PS");
	ThrowIfFailed(m_device->CreatePixelShader(surfaceCopyColorPsBlob->GetBufferPointer(),
		surfaceCopyColorPsBlob->GetBufferSize(), nullptr, &m_surfaceCopyColorPS),
		"Create surface-copy color PS");
	ThrowIfFailed(m_device->CreatePixelShader(surfaceCopyDepthPsBlob->GetBufferPointer(),
		surfaceCopyDepthPsBlob->GetBufferSize(), nullptr, &m_surfaceCopyDepthPS),
		"Create surface-copy depth PS");
	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	ThrowIfFailed(m_device->CreateSamplerState(&sampler, &m_presentSampler), "Create presentation sampler");
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	ThrowIfFailed(m_device->CreateSamplerState(&sampler, &m_presentPointSampler), "Create point presentation sampler");

	D3D11_RASTERIZER_DESC rasterizer{};
	rasterizer.FillMode = D3D11_FILL_SOLID;
	rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.ScissorEnable = TRUE;
	rasterizer.DepthClipEnable = TRUE;
	ThrowIfFailed(m_device->CreateRasterizerState(&rasterizer, &m_rasterizerState), "Create rasterizer state");
	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ThrowIfFailed(m_device->CreateBlendState(&blend, &m_blendState), "Create blend state");
	D3D11_DEPTH_STENCIL_DESC depth{};
	depth.DepthEnable = FALSE;
	depth.StencilEnable = FALSE;
	ThrowIfFailed(m_device->CreateDepthStencilState(&depth, &m_depthStencilState), "Create depth state");
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ThrowIfFailed(m_device->CreateDepthStencilState(&depth, &m_surfaceCopyDepthState),
		"Create surface-copy depth state");
}

void D3D11Renderer::RefreshBackBuffer()
{
	ComPtr<ID3D11Texture2D> buffer;
	ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "IDXGISwapChain::GetBuffer");
	if (buffer.Get() == m_backBuffer.Get() && m_backBufferView)
		return;
	m_backBuffer = buffer;
	m_backBufferView.Reset();
	ThrowIfFailed(m_device->CreateRenderTargetView(m_backBuffer.Get(), nullptr, &m_backBufferView), "Create back-buffer RTV");
}

void D3D11Renderer::EnsureBackBufferSize()
{
	int requestedWidth = 1;
	int requestedHeight = 1;
	WindowSystem::GetWindowPhysSize(requestedWidth, requestedHeight);
	requestedWidth = (std::max)(requestedWidth, 1);
	requestedHeight = (std::max)(requestedHeight, 1);

	DXGI_SWAP_CHAIN_DESC description{};
	ThrowIfFailed(m_swapChain->GetDesc(&description), "IDXGISwapChain::GetDesc");
	if (description.BufferDesc.Width == static_cast<UINT>(requestedWidth) &&
		description.BufferDesc.Height == static_cast<UINT>(requestedHeight))
		return;

	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_backBufferView.Reset();
	m_backBuffer.Reset();
	m_context->Flush();

	ThrowIfFailed(m_swapChain->ResizeBuffers(
		2, static_cast<UINT>(requestedWidth), static_cast<UINT>(requestedHeight),
		DXGI_FORMAT_B8G8R8A8_UNORM, 0), "IDXGISwapChain::ResizeBuffers");

	// A composition swap chain uses physical-pixel buffers while XAML lays out
	// the panel in DIPs. Keep the DXGI matrix synchronized with the scale used
	// by the host when it published requestedWidth/requestedHeight.
	ComPtr<IDXGISwapChain2> swapChain2;
	if (SUCCEEDED(m_swapChain.As(&swapChain2)))
	{
		const float compositionScale = static_cast<float>((std::max)(
			WindowSystem::GetWindowDPIScale(), 0.01));
		DXGI_MATRIX_3X2_F inverseScale{};
		inverseScale._11 = 1.0f / compositionScale;
		inverseScale._22 = 1.0f / compositionScale;
		ThrowIfFailed(swapChain2->SetMatrixTransform(&inverseScale),
			"IDXGISwapChain2::SetMatrixTransform");
	}

	cemuLog_log(LogType::Force, "D3D11 presentation resized to {}x{}",
		requestedWidth, requestedHeight);
}

void D3D11Renderer::Initialize()
{
	// Renderer::Initialize creates the common output shaders through shader_create.
	// D3D11 translates those shaders with glslang, so its compiler and worker must
	// exist first even though OpenGL/Vulkan can initialize their common layer first.
	glslang::InitializeProcess();
	D3D11ShaderQueueStart();
	g_compiled_shaders_total = 0;
	g_compiled_shaders_async = 0;
	g_compiling_pipelines = 0;
	InitializePresentationPipeline();
	Renderer::Initialize();
	GetVendorInformation();
	// Renderer::Initialize() creates the TV context first and the pad context
	// second, leaving the pad context current. Dear ImGui stores the DX11
	// backend data per context, while the embedded D3D11 surface renders the
	// main/TV view. Initialize the backend on the context that ImguiBegin(true)
	// will actually use.
	ImGui::SetCurrentContext(imguiTVContext);
	m_imguiInitialized = ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
	if (!m_imguiInitialized)
		cemuLog_log(LogType::Force, "D3D11: Dear ImGui backend initialization failed; overlays and notifications are unavailable");
}

void D3D11Renderer::GetVendorInformation()
{
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc{};
	if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
		FAILED(adapter->GetDesc(&desc)))
	{
		m_vendor = GfxVendor::Generic;
		cemuLog_log(LogType::Force, "D3D11 adapter: unknown");
		return;
	}

	switch (desc.VendorId)
	{
	case 0x1002:
	case 0x1022:
		m_vendor = GfxVendor::AMD;
		break;
	case 0x8086:
		m_vendor = GfxVendor::Intel;
		break;
	case 0x10DE:
		m_vendor = GfxVendor::Nvidia;
		break;
	default:
		m_vendor = GfxVendor::Generic;
		break;
	}

	char adapterName[256]{};
	const int adapterNameLength = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
		adapterName, static_cast<int>(std::size(adapterName)), nullptr, nullptr);
	if (adapterNameLength <= 0)
		std::strcpy(adapterName, "unknown");
	cemuLog_log(LogType::Force, "D3D11 adapter: {}", adapterName);
	cemuLog_log(LogType::Force,
		"D3D11 adapter IDs: vendor 0x{:04X}, device 0x{:04X}; memory dedicated {} MB, shared {} MB",
		desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory / (1024 * 1024),
		desc.SharedSystemMemory / (1024 * 1024));
}

void D3D11Renderer::EnableDebugMode()
{
	m_debugModeEnabled = true;
	if (m_infoQueue)
	{
		m_infoQueue->ClearStoredMessages();
		cemuLog_log(LogType::Force, "D3D11 debug message validation enabled");
	}
}

void D3D11Renderer::Shutdown()
{
	// Stop before releasing any D3D11 objects. The compiler worker only touches
	// the device, but it may be in an Xbox driver call while the title closes.
	D3D11ShaderQueueStop();
	if (m_imguiInitialized)
	{
		ImGui::SetCurrentContext(imguiTVContext);
		ImGui_ImplDX11_Shutdown();
		m_imguiInitialized = false;
	}
	if (m_context)
	{
		m_context->ClearState();
		m_context->Flush();
	}
	Renderer::Shutdown();
	glslang::FinalizeProcess();
}

bool D3D11Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
	usageInMB = totalInMB = -1;
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIAdapter3> adapter3;
	if (FAILED(m_device.As(&dxgiDevice)) ||
		FAILED(dxgiDevice->GetAdapter(&adapter)) ||
		FAILED(adapter.As(&adapter3)))
		return false;
	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		return false;
	usageInMB = static_cast<int>(info.CurrentUsage / (1024 * 1024));
	totalInMB = static_cast<int>(info.Budget / (1024 * 1024));
	return true;
}

bool D3D11Renderer::IsPadWindowActive() { return false; }

bool D3D11Renderer::BeginFrame(bool mainWindow)
{
	if (!mainWindow || m_deviceLost.load(std::memory_order_relaxed))
		return false;
	EnsureBackBufferSize();
	RefreshBackBuffer();
	ID3D11RenderTargetView* view = m_backBufferView.Get();
	m_context->OMSetRenderTargets(1, &view, nullptr);
	D3D11_TEXTURE2D_DESC backBufferDesc{};
	m_backBuffer->GetDesc(&backBufferDesc);
	const int width = static_cast<int>((std::max)(backBufferDesc.Width, 1u));
	const int height = static_cast<int>((std::max)(backBufferDesc.Height, 1u));
	renderTarget_setViewport(0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1, false);
	renderTarget_setScissor(0, 0, width, height);
	return true;
}

void D3D11Renderer::ClearColorbuffer(bool padView)
{
	if (!padView && m_backBufferView)
	{
		const float color[4]{ 0, 0, 0, 0 };
		m_context->ClearRenderTargetView(m_backBufferView.Get(), color);
	}
}

void D3D11Renderer::DrawEmptyFrame(bool mainWindow)
{
	if (BeginFrame(mainWindow))
	{
		ClearColorbuffer(!mainWindow);
		SwapBuffers(mainWindow, !mainWindow);
	}
}

void D3D11Renderer::SwapBuffers(bool swapTV, bool)
{
	if (!swapTV || m_deviceLost.load(std::memory_order_relaxed))
		return;
	D3D11_DEBUG_CHECK("before Present");
	{
		D3D11_DRIVER_TRACE("IDXGISwapChain::Present");
		const HRESULT presentResult = m_swapChain->Present(1, 0);
		if (IsDeviceLostResult(presentResult))
		{
			RecordDeviceLost(presentResult, "IDXGISwapChain::Present");
			return;
		}
		ThrowIfFailed(presentResult, "IDXGISwapChain::Present");
	}
	D3D11_DEBUG_CHECK("after Present");
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_backBufferView.Reset();
	m_backBuffer.Reset();
	CheckMemoryPressure();
}

void D3D11Renderer::HandleScreenshotRequest(LatteTextureView* textureView, bool padView)
{
	if ((!m_screenshot_requested && m_screenshot_state == ScreenshotState::None) ||
		padView || !textureView)
		return;
	m_screenshot_state = ScreenshotState::None;

	auto* texture = static_cast<D3D11Texture*>(textureView->baseTexture);
	const int width = static_cast<int>((std::max)(1u,
		static_cast<uint32>(texture->width) >> textureView->firstMip));
	const int height = static_cast<int>((std::max)(1u,
		static_cast<uint32>(texture->height) >> textureView->firstMip));
	const FormatInfo info = GetFormatInfo(textureView->format, texture->isDepth);
	if (texture->isDepth || info.compressed || info.bytesPerBlock != 4)
	{
		cemuLog_log(LogType::Force, "D3D11 screenshot format 0x{:x} is not an RGBA8 surface",
			static_cast<uint32>(textureView->format));
		CancelScreenshotRequest();
		return;
	}

	D3D11Readback readback(this, static_cast<D3D11TextureView*>(textureView));
	const uint8* rgba = readback.GetData();
	std::vector<uint8> rgb(static_cast<size_t>(width) * height * 3);
	const bool srcUsesSRGB = HAS_FLAG(textureView->format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
	const bool dstUsesSRGB = LatteGPUState.tvBufferUsesSRGB;
	for (size_t source = 0, destination = 0; destination < rgb.size(); source += 4, destination += 3)
	{
		for (size_t channel = 0; channel < 3; ++channel)
		{
			uint8 value = rgba[source + channel];
			if (srcUsesSRGB && !dstUsesSRGB)
				value = SRGBComponentToRGB(value);
			else if (!srcUsesSRGB && dstUsesSRGB)
				value = RGBComponentToSRGB(value);
			rgb[destination + channel] = value;
		}
	}
	SaveScreenshot(rgb, width, height, true);
}

void D3D11Renderer::DrawBackbufferQuad(LatteTextureView* textureView, RendererOutputShader* shader, bool useLinear,
	sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	if (textureView)
		static_cast<D3D11TextureView*>(textureView)->PrepareForSampling();
	if (padView || !textureView || !BeginFrame(true))
		return;
	// The common render-target path already applies the configured stretch or
	// aspect-preserving calculation. Honor the same rectangle and clear policy
	// used by the OpenGL and Vulkan presentation paths.
	if (clearBackground)
		ClearColorbuffer(false);
	auto* view = static_cast<D3D11TextureView*>(textureView);
	ID3D11ShaderResourceView* srv = view->SRV();
	ID3D11SamplerState* sampler = useLinear ? m_presentSampler.Get() : m_presentPointSampler.Get();
	const auto outputUniforms = shader->FillUniformBlockBuffer(
		*textureView, { imageWidth, imageHeight }, false);
	if (!UpdateDynamicConstantBuffer(m_presentUniformBuffer,
		m_presentUniformBufferCapacity, &outputUniforms, sizeof(outputUniforms)))
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 could not update presentation color-space parameters");
		return;
	}
	ID3D11Buffer* presentationConstants = m_presentUniformBuffer.Get();
	ComPtr<ID3D11Buffer> previousPixelConstants;
	m_context->PSGetConstantBuffers(0, 1, previousPixelConstants.GetAddressOf());
	const float blendFactor[4]{};
	m_context->RSSetState(m_rasterizerState.Get());
	m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
	m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
	m_context->VSSetShader(m_presentVS.Get(), nullptr, 0);
	m_context->PSSetShader(m_presentPS.Get(), nullptr, 0);
	m_context->PSSetConstantBuffers(0, 1, &presentationConstants);
	m_context->GSSetShader(nullptr, nullptr, 0);
	m_context->IASetInputLayout(nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_context->PSSetShaderResources(0, 1, &srv);
	m_context->PSSetSamplers(0, 1, &sampler);
	renderTarget_setViewport(static_cast<float>(imageX), static_cast<float>(imageY),
		static_cast<float>(imageWidth), static_cast<float>(imageHeight), 0, 1, false);
	m_context->Draw(3, 0);
	ID3D11ShaderResourceView* nullView = nullptr;
	m_context->PSSetShaderResources(0, 1, &nullView);
	ID3D11Buffer* previousConstants = previousPixelConstants.Get();
	m_context->PSSetConstantBuffers(0, 1, &previousConstants);
	// The presentation pass owns the immediate context temporarily. Force the
	// next GX2 draw to restore only the native states it displaced.
	InvalidateNativePipelineState();
}

void D3D11Renderer::Flush(bool waitIdle)
{
	if (m_deviceLost.load(std::memory_order_relaxed))
		return;
	m_context->Flush();
	if (waitIdle)
		WaitForGpuIdle();
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

	// End is ordered after earlier draws. Flush must follow End because GetData
	// deliberately uses DONOTFLUSH and therefore cannot submit the query itself.
	m_context->End(m_gpuIdleQuery.Get());
	m_context->Flush();
	uint32 spinCount = 0;
	for (;;)
	{
		const HRESULT status = m_context->GetData(m_gpuIdleQuery.Get(), nullptr, 0,
			D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (status == S_OK)
			return true;
		if (status != S_FALSE)
		{
			if (IsDeviceLostResult(status))
				RecordDeviceLost(status, "GPU-idle wait");
			else
				cemuLog_log(LogType::Force,
					"D3D11: GPU-idle wait failed with HRESULT 0x{:08X}",
					static_cast<uint32>(status));
			return false;
		}
		_mm_pause();
		if ((++spinCount & 0x3FF) == 0)
			std::this_thread::yield();
	}
}

void D3D11Renderer::RecoverFromMemoryPressure(const char* resourceName, bool evictIndexCache)
{
	int usageInMB = -1;
	int budgetInMB = -1;
	GetVRAMInfo(usageInMB, budgetInMB);
	const uint64 processCommitMB = QueryProcessCommitBytes() / (1024 * 1024);

	// Resource-creation retries occur inside an active draw. Clearing SRVs/RTVs or
	// evicting textures here leaves that draw with null bindings and produces
	// missing geometry/textures. Full texture eviction is performed by
	// CheckMemoryPressure() from the presentation path; here only retire work and
	// release explicitly requested transient index entries.
	cemuLog_log(LogType::Force,
		"D3D11 memory pressure while creating {} (process commit {} MB, video usage {} MB, "
		"video budget {} MB); requesting driver trim{}",
		resourceName ? resourceName : "resource", processCommitMB, usageInMB, budgetInMB,
		evictIndexCache ? " after evicting transient index data" : "");

	// Cached decoded-index offsets can be recreated from emulated memory. Drop
	// them before a ring retry so no cache entry can refer to discarded storage.
	if (evictIndexCache)
	{
		m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		LatteIndices_invalidateAll();
	}
	// D3D11On12 retains the native allocations referenced by submitted command
	// lists even after every Cemu-side ComPtr is released. A plain Flush only
	// submits those lists; it does not retire them, so Trim has nothing it can
	// reclaim while the process continues racing toward the title cap.
	m_context->Flush();
	if (!WaitForGpuIdle())
		return;

	// On UWP (and D3D11On12 on Xbox) Trim asks DXGI to release allocations that
	// became unreferenced above before the retry. Desktop drivers may not expose
	// IDXGIDevice3, in which case the eviction and Flush are still useful.
	ComPtr<IDXGIDevice3> dxgiDevice3;
	if (SUCCEEDED(m_device.As(&dxgiDevice3)))
		dxgiDevice3->Trim();
#if defined(CEMU_UWP)
	HeapCompact(GetProcessHeap(), 0);
#endif
}

uint64 D3D11Renderer::QueryProcessCommitBytes() const
{
	return QueryProcessPrivateCommitBytes();
}

uint64 D3D11Renderer::ShaderFailureKey(RendererShader::ShaderType type, uint64 baseHash,
	uint64 auxHash) const
{
	uint64 key = HashBytes(&type, sizeof(type));
	key = HashBytes(&baseHash, sizeof(baseHash), key);
	return HashBytes(&auxHash, sizeof(auxHash), key);
}

uint64 D3D11Renderer::CurrentLogicalPipelineKey() const
{
	uint64 key = HashBytes(&m_inputLayoutKey, sizeof(m_inputLayoutKey));
	const auto appendShader = [&key](LatteDecompilerShader* context)
	{
		auto* shader = context ? static_cast<D3D11Shader*>(context->shader) : nullptr;
		const uint64 base = shader ? shader->BaseHash() : 0;
		const uint64 aux = shader ? shader->AuxHash() : 0;
		key = HashBytes(&base, sizeof(base), key);
		key = HashBytes(&aux, sizeof(aux), key);
	};
	appendShader(LatteSHRC_GetActiveVertexShader());
	appendShader(LatteSHRC_GetActivePixelShader());
	appendShader(LatteSHRC_GetActiveGeometryShader());
	const auto rasterizer = reinterpret_cast<uintptr_t>(m_appliedRasterizerState.Get());
	const auto blend = reinterpret_cast<uintptr_t>(m_appliedBlendState.Get());
	const auto depth = reinterpret_cast<uintptr_t>(m_appliedDepthStencilState.Get());
	key = HashBytes(&rasterizer, sizeof(rasterizer), key);
	key = HashBytes(&blend, sizeof(blend), key);
	key = HashBytes(&depth, sizeof(depth), key);
	key = HashBytes(&m_appliedPrimitiveTopology, sizeof(m_appliedPrimitiveTopology), key);
	return key;
}

void D3D11Renderer::CheckMemoryPressure()
{
#if defined(CEMU_UWP)
	// Sampling every 10 presents catches rapidly growing title workloads while
	// keeping the accounting overhead negligible. On
	// Xbox, PrivateUsage already reaches the 5120 MiB title cap and includes the
	// driver-backed commitment. Adding DXGI CurrentUsage double-counts a large
	// portion of graphics memory, so use it only as diagnostic information.
	// Ten-frame sampling is sufficient in the normal state. Once the soft limit
	// has been crossed, continue observing every frame, while the heavy recovery
	// below is separately rate-limited to avoid turning memory hysteresis into a
	// recurring GPU stall.
	if ((++m_memoryCheckFrame % 10) != 0 && !m_memoryPressureActive)
		return;
	const uint64 processCommitMB = QueryProcessCommitBytes() / (1024 * 1024);
	int videoUsageMB = -1;
	int videoBudgetMB = -1;
	GetVRAMInfo(videoUsageMB, videoBudgetMB);
	constexpr uint64 softLimitMB = 4096;
	constexpr uint64 releaseLimitMB = 3968;
	if (processCommitMB < softLimitMB)
	{
		if (m_memoryPressureActive && processCommitMB < releaseLimitMB)
		{
			cemuLog_log(LogType::Force,
				"D3D11 Series S memory pressure cleared (process commit {} MB)", processCommitMB);
			m_memoryPressureActive = false;
			m_lastHeavyMemoryRecoveryFrame = 0;
			m_lastHeavyMemoryRecoveryCommitMB = 0;
		}
		return;
	}

	// Waiting for GPU retirement, trimming the driver and compacting the process
	// heap are deliberately expensive. Repeating that sequence every present
	// while usage remains inside the hysteresis band causes periodic frame stalls.
	// Keep the 4096 MiB trigger, but repeat a heavy recovery only after meaningful
	// growth, at a slow maintenance interval, or at a guarded emergency cadence.
	const uint32 framesSinceHeavyRecovery =
		m_memoryCheckFrame - m_lastHeavyMemoryRecoveryFrame;
	const bool commitGrewMaterially = m_lastHeavyMemoryRecoveryCommitMB == 0 ||
		processCommitMB >= m_lastHeavyMemoryRecoveryCommitMB + 128;
	const bool emergencyPressure = processCommitMB >= 4608;
	const bool shouldRunHeavyRecovery = !m_memoryPressureActive ||
		(framesSinceHeavyRecovery >= 10 && (commitGrewMaterially || emergencyPressure)) ||
		framesSinceHeavyRecovery >= 300;
	if (!shouldRunHeavyRecovery)
		return;

	ClearShaderResources();
	m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	LatteGPUState.repeatTextureInitialization = true;
	LatteIndices_invalidateAll();
	const size_t evictedTextures = LatteTC_TrimUnusedTextures(3, 96);
	std::vector<uint8>().swap(m_uploadBuffer);
	m_context->Flush();
	if (!WaitForGpuIdle())
		return;
	// The cache-copy scratch is reconstructible and only needed while cache nodes
	// are merged. Release its peak allocation once all referencing GPU work has
	// retired so it cannot permanently consume Series S headroom.
	m_bufferCopyScratch.Reset();
	m_bufferCopyScratchCapacity = 0;
	m_feedbackViews.clear();
	m_feedbackResources.clear();
	m_feedbackSnapshots.clear();
	ComPtr<IDXGIDevice3> dxgiDevice3;
	if (SUCCEEDED(m_device.As(&dxgiDevice3)))
		dxgiDevice3->Trim();
	HeapCompact(GetProcessHeap(), 0);
	const uint64 postTrimCommitMB = QueryProcessCommitBytes() / (1024 * 1024);
	m_lastHeavyMemoryRecoveryFrame = m_memoryCheckFrame;
	m_lastHeavyMemoryRecoveryCommitMB = postTrimCommitMB;
	if (!m_memoryPressureActive || evictedTextures != 0 || (m_memoryCheckFrame % 60) == 0)
	{
		cemuLog_log(LogType::Force,
			"D3D11 Series S memory guard: process commit {} MB, video usage {} MB, "
			"video budget {} MB; evicted {} textures, post-retirement commit {} MB, "
			"compiled shaders {}, index uploads {}, ring wraps {}",
			processCommitMB, videoUsageMB, videoBudgetMB, evictedTextures,
			postTrimCommitMB,
			m_compiledShaderCount.load(std::memory_order_relaxed),
			m_indexUploadCount, m_indexRingWrapCount);
	}
	m_memoryPressureActive = true;
#endif
}

void D3D11Renderer::NotifyLatteCommandProcessorIdle()
{
	// D3D11 submits work automatically. Flushing every time the emulated GX2
	// ring buffer is briefly empty destroys batching and creates CPU/GPU bubbles.
	// Explicit synchronization paths still call Flush(true) when required.
}

void D3D11Renderer::CheckDebugMessages(const char* scope)
{
	if (!m_infoQueue || !m_debugModeEnabled)
		return;

	bool hasFatalMessage = false;
	const UINT64 messageCount = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
	for (UINT64 index = 0; index < messageCount; ++index)
	{
		SIZE_T messageSize{};
		if (FAILED(m_infoQueue->GetMessage(index, nullptr, &messageSize)) || !messageSize)
			continue;
		std::vector<uint8> storage(messageSize);
		auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
		if (FAILED(m_infoQueue->GetMessage(index, message, &messageSize)))
			continue;
		if (message->Severity > D3D11_MESSAGE_SEVERITY_WARNING)
			continue;
		// GX2 permits raw component writes while a color buffer is viewed with
		// an integer format. D3D11 defines the same float-output/integer-RTV
		// case as a raw bit copy and emits warning 415 only to ask whether that
		// behaviour is intentional. It is intentional for this compatibility
		// path, so do not present it as an emulator failure.
		if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING &&
			static_cast<uint32>(message->ID) == 415 &&
			message->pDescription != nullptr &&
			std::strstr(message->pDescription, "raw bits output from the shader") != nullptr)
			continue;
		// GX2 permits depth-only and raster-discard passes to keep a pixel
		// shader active without a color attachment. D3D11 explicitly defines
		// those color writes as discarded, so warning 3146081 is informational.
		if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING &&
			static_cast<uint32>(message->ID) == 3146081 &&
			message->pDescription != nullptr &&
			std::strstr(message->pDescription,
				"writes of an unbound Render Target View are discarded") != nullptr)
			continue;
		const uint32 messageId = static_cast<uint32>(message->ID);
		if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING &&
			!m_reportedDebugWarnings.emplace(messageId).second)
			continue;

		const char* severity =
			message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
			message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING";
		cemuLog_log(LogType::Force, "D3D11 debug [{}] {} id {}: {}",
			scope, severity, messageId,
			message->pDescription ? message->pDescription : "(no description)");
		hasFatalMessage |= message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR;
	}
	m_infoQueue->ClearStoredMessages();

	if (hasFatalMessage)
		cemuLog_log(LogType::Force,
			"D3D11 rejected renderer state during {}; the invalid command was discarded by the debug runtime.",
			scope);
}

bool D3D11Renderer::ImguiBegin(bool mainWindow)
{
	if (!m_imguiInitialized || !Renderer::ImguiBegin(mainWindow))
		return false;
	ImGui_ImplDX11_NewFrame();
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();
	return true;
}

void D3D11Renderer::ImguiEnd()
{
	ImGui::Render();
	if (m_imguiInitialized)
	{
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		D3D11_DEBUG_CHECK("ImGui render");
		// The backend changes shaders, resources, samplers, blend, rasterizer and
		// depth state. Make the following GX2 draw restore its native state.
		InvalidateNativePipelineState();
	}
}

ImTextureID D3D11Renderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	if (data.empty() || size.x <= 0 || size.y <= 0)
		return nullptr;
	const size_t width = static_cast<size_t>(size.x);
	const size_t height = static_cast<size_t>(size.y);
	if (height > SIZE_MAX / width)
		return nullptr;
	const size_t pixelCount = width * height;
	if (pixelCount > SIZE_MAX / 4 || pixelCount > SIZE_MAX / 3)
		return nullptr;

	// LoadTGAFile returns tightly packed RGB pixels. Direct3D is given an
	// R8G8B8A8 texture below, so passing imageData directly with a width*4
	// pitch makes the driver read one byte beyond every source pixel and
	// eventually past the vector allocation. Some AMD drivers defer that read
	// until Present(), which made the access violation appear unrelated.
	const size_t requiredRgbBytes = pixelCount * 3;
	if (data.size() < requiredRgbBytes)
	{
		cemuLog_log(LogType::Force,
			"D3D11 ImGui texture rejected: RGB source is too small (required {}, got {})",
			requiredRgbBytes, data.size());
		return nullptr;
	}
	std::vector<uint8> rgba(pixelCount * 4);
	for (size_t i = 0; i < pixelCount; ++i)
	{
		rgba[i * 4 + 0] = data[i * 3 + 0];
		rgba[i * 4 + 1] = data[i * 3 + 1];
		rgba[i * 4 + 2] = data[i * 3 + 2];
		rgba[i * 4 + 3] = 0xFF;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = size.x; desc.Height = size.y; desc.MipLevels = 1; desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA initial{ rgba.data(), static_cast<UINT>(width * 4), 0 };
	ComPtr<ID3D11Texture2D> texture;
	{
		D3D11_DRIVER_TRACE(fmt::format(
			"CreateTexture2D ImGui {}x{} RGB={} RGBA={}",
			size.x, size.y, data.size(), rgba.size()));
		if (FAILED(m_device->CreateTexture2D(&desc, &initial, &texture)))
			return nullptr;
	}
	ID3D11ShaderResourceView* view{};
	if (FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
		return nullptr;
	D3D11_DEBUG_CHECK("ImGui texture creation");
	return view;
}

void D3D11Renderer::DeleteTexture(ImTextureID id) { if (id) static_cast<IUnknown*>(id)->Release(); }
void D3D11Renderer::DeleteFontTextures() { if (m_imguiInitialized) ImGui_ImplDX11_InvalidateDeviceObjects(); }
void D3D11Renderer::AppendOverlayDebugInfo()
{
	ImGui::Text("--- Direct3D 11 info ---");
	ImGui::TextUnformatted("DirectX 11 Feature Level 11.0");
	ImGui::Text("Logical pipelines    %zu", m_seenLogicalPipelines.size());
	ImGui::Text("Runtime shaders      %u", m_compiledShaderCount.load(std::memory_order_relaxed));
	ImGui::Text("Sampler states       %zu", m_samplerCache.size());
	ImGui::Text("Rasterizer states    %zu", m_rasterizerCache.size());
	ImGui::Text("Blend states         %zu", m_blendCache.size());
	ImGui::Text("Depth/stencil states %zu", m_depthStencilCache.size());
	ImGui::Text("Buffer cache         %zu MB", m_bufferCacheShadow.size() / (1024 * 1024));
	int usageInMB{};
	int budgetInMB{};
	if (GetVRAMInfo(usageInMB, budgetInMB))
		ImGui::Text("Video memory         %d / %d MB", usageInMB, budgetInMB);
}
