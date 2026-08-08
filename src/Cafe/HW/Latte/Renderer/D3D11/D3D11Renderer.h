#pragma once

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <d3d11_3.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>

class D3D11Renderer final : public Renderer
{
public:
	D3D11Renderer();
	~D3D11Renderer() override;

	static D3D11Renderer* GetInstance();

	void Initialize() override;
	void Shutdown() override;
	bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override;
	bool IsPadWindowActive() override;
	void ClearColorbuffer(bool padView) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;
	void HandleScreenshotRequest(LatteTextureView*, bool) override;
	void DrawBackbufferQuad(LatteTextureView*, RendererOutputShader*, bool,
		sint32, sint32, sint32, sint32, bool, bool) override;
	bool BeginFrame(bool mainWindow) override;
	void Flush(bool waitIdle) override;
	void NotifyLatteCommandProcessorIdle() override;
	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;
	void AppendOverlayDebugInfo() override;

	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ) override;
	void renderTarget_setScissor(sint32 x, sint32 y, sint32 width, sint32 height) override;
	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* fbo) override;

	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;
	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth,
		Latte::E_DIM dim, uint32 width, uint32 height) override;
	void texture_clearSlice(LatteTexture*, sint32, sint32) override;
	void texture_loadSlice(LatteTexture*, sint32, sint32, sint32, void*, sint32, sint32, uint32) override;
	void texture_clearColorSlice(LatteTexture*, sint32, sint32, float, float, float, float) override;
	void texture_clearDepthSlice(LatteTexture*, uint32, sint32, bool, bool, float, uint32) override;
	LatteTexture* texture_createTextureEx(Latte::E_DIM, MPTR, MPTR, Latte::E_GX2SURFFMT,
		uint32, uint32, uint32, uint32, uint32, uint32, Latte::E_HWTILEMODE, bool) override;
	void texture_setLatteTexture(LatteTextureView*, uint32) override;
	void texture_copyImageSubData(LatteTexture*, sint32, sint32, sint32, sint32,
		LatteTexture*, sint32, sint32, sint32, sint32, sint32, sint32, sint32) override;
	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView*) override;
	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture*, sint32, sint32,
		LatteTexture*, sint32, sint32, sint32, sint32) override;

	void bufferCache_init(const sint32 size) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 offset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void buffer_bindVertexBuffer(uint32, uint32, uint32) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType, uint32, uint32, uint32) override;
	RendererShader* shader_create(RendererShader::ShaderType, uint64, uint64,
		const std::string&, bool, bool) override;
	bool shader_creation_failed_temporary() const override;
	void streamout_setupXfbBuffer(uint32, sint32, uint32, uint32) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;
	void draw_beginSequence() override;
	void draw_execute(uint32, uint32, uint32, uint32, MPTR,
		Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE, const LatteDrawcallContext&) override;
	void draw_endSequence() override;
	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;
	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject*) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	ID3D11Device* GetDevice() const { return m_device.Get(); }
	ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

private:
	void RefreshBackBuffer();
	void EnsureBackBufferSize();
	void InitializePresentationPipeline();
	bool BindActiveShaders();
	RendererShader* GetRectEmulationShader(class LatteDecompilerShader* vertexShader);
	bool HasRequiredShaders() const;
	bool UpdateInputLayout();
	void UpdateUniformVars(class LatteDecompilerShader* shader, uint32 verticesPerInstance);
	void UpdateSamplerSwizzleBuffer(class LatteDecompilerShader* shader);
	bool UpdateDynamicConstantBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,
		UINT& capacity, const void* data, UINT size);
	void ApplyPipelineState();
	void HandleSpecialState5();
	void CheckDebugMessages(const char* scope);
	ID3D11SamplerState* GetSamplerState(class LatteDecompilerShader* shader, uint32 textureIndex,
		class LatteTexture* texture);
	void UnbindTextureHazards();
	void ClearShaderResources();
	void ResolveTextureFeedbackLoops(const std::array<ID3D11RenderTargetView*, 8>& targets,
		ID3D11DepthStencilView* depth);
	void RecoverFromMemoryPressure(const char* resourceName, bool evictIndexCache);
	void CheckMemoryPressure();
	bool WaitForGpuIdle();
	uint64 QueryProcessCommitBytes() const;
	uint64 BuildCurrentPipelineKey() const;

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11Device1> m_device1;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context1;
	Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
	Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backBuffer;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferView;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_bufferCache;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_bufferCopyScratch;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexRingBuffer;
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutBuffers{};
	std::vector<uint8> m_bufferCacheShadow;
	std::vector<uint8> m_uploadBuffer;
	std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE * 3> m_boundTextures{};
	std::array<UINT, 16> m_vertexOffsets{};
	std::array<UINT, 16> m_vertexStrides{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 16> m_vertexBuffers{};
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, LATTE_NUM_MAX_UNIFORM_BUFFERS>, 3> m_uniformBuffers{};
	std::array<std::array<UINT, LATTE_NUM_MAX_UNIFORM_BUFFERS>, 3> m_uniformBufferCapacity{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 3> m_uniformVarsBuffers{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 3> m_samplerSwizzleBuffers{};
	std::array<UINT, 3> m_uniformVarsBufferCapacity{};
	std::array<UINT, 3> m_samplerSwizzleBufferCapacity{};
	std::array<std::vector<uint8>, 3> m_uniformScratch{};
	std::array<std::vector<uint8>, 3> m_uploadedUniformScratch{};
	std::array<bool, 3> m_uniformScratchUploaded{};
	std::array<std::array<std::array<uint32, 4>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 3>
		m_samplerSwizzles{};
	decltype(m_samplerSwizzles) m_uploadedSamplerSwizzles{};
	std::array<bool, 3> m_samplerSwizzleUploaded{};
	std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutOffsets{};
	std::array<bool, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutEnabled{};
	std::array<bool, 8> m_boundColorBlendable{ true, true, true, true, true, true, true, true };
	bool m_streamoutActive{};

	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_presentVS;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_presentPS;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_surfaceCopyColorPS;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_surfaceCopyDepthPS;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_presentSampler;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_presentPointSampler;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_surfaceCopyDepthState;
	Microsoft::WRL::ComPtr<ID3D11Query> m_gpuIdleQuery;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D11InputLayout>> m_inputLayoutCache;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D11SamplerState>> m_samplerCache;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D11RasterizerState>> m_rasterizerCache;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D11BlendState>> m_blendCache;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D11DepthStencilState>> m_depthStencilCache;
	std::unordered_map<uint64, std::unique_ptr<RendererShader>> m_rectShaderCache;
	std::unordered_set<uint64> m_warmedPipelineKeys;
	std::unordered_set<uint64> m_deferredPipelineKeys;
	std::unordered_set<uint32> m_reportedDebugWarnings;
	std::vector<Microsoft::WRL::ComPtr<ID3D11Resource>> m_feedbackResources;
	std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_feedbackViews;
	uint64 m_inputLayoutKey{};
	uint64 m_indexUploadCount{};
	uint64 m_indexRingWrapCount{};
	UINT m_indexRingCapacity{};
	UINT m_indexRingOffset{};
	UINT m_bufferCopyScratchCapacity{};
	uint32 m_memoryCheckFrame{};
	std::atomic<uint32> m_compiledShaderCount{};
	bool m_memoryPressureActive{};
	std::atomic_bool m_shaderCompilationBlocked{};
	bool m_inputLayoutKeyValid{};
	bool m_imguiInitialized{};
	bool m_graphicsStateInvalid{};
};
