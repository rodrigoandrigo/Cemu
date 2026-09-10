#pragma once

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class D3D11Renderer final : public Renderer
{
public:
	D3D11Renderer();
	~D3D11Renderer() override;

	static D3D11Renderer* GetInstance();

	void Initialize() override;
	void Shutdown() override;
	void EnableDebugMode() override;
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
	void GetVendorInformation() override;
	void RefreshBackBuffer();
	void EnsureBackBufferSize();
	void InitializePresentationPipeline();
	bool EnsureActiveShadersCompiled();
	bool BindActiveShaders();
	RendererShader* GetRectEmulationShader(class LatteDecompilerShader* vertexShader);
	bool HasRequiredShaders() const;
	bool UpdateInputLayout();
	void UpdateUniformVars(class LatteDecompilerShader* shader, uint32 verticesPerInstance,
		bool fullUpdate, bool aluConstantsDirty, uint32 uniformBufferDirtyMask);
	void UpdateSamplerSwizzleBuffer(class LatteDecompilerShader* shader);
	bool UpdateDynamicConstantBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,
		UINT& capacity, const void* data, UINT size);
	void BindConstantBuffer(LatteConst::ShaderType stage, UINT binding, ID3D11Buffer* buffer);
	void ApplyPipelineState();
	void HandleSpecialState5();
	void CheckDebugMessages(const char* scope);
	ID3D11SamplerState* GetSamplerState(class LatteDecompilerShader* shader, uint32 textureIndex,
		class LatteTexture* texture);
	void UnbindTextureHazards();
	void ClearShaderResources();
	bool ResolveTextureFeedbackLoops(const std::array<ID3D11RenderTargetView*, 8>& targets,
		ID3D11DepthStencilView* depth);
	void InvalidateNativePipelineState();
	void RecoverFromMemoryPressure(const char* resourceName, bool evictIndexCache);
	void FlushBufferCacheUploads();
	void CheckMemoryPressure();
	bool WaitForGpuIdle();
	bool EnsureNativeStreamoutBuffers();
	static bool IsDeviceLostResult(HRESULT result);
	bool CheckDeviceHealth(const char* operation);
	bool ExecutePixelStreamoutCapture(uint32 baseVertex, uint32 baseInstance,
		uint32 instanceCount, uint32 vertexCount, uint32 indexCount,
		Renderer::INDEX_TYPE indexType, ID3D11Buffer* indexBuffer, UINT indexOffset,
		const uint8* decodedIndices, UINT decodedIndexBytes);
	void RecordDeviceLost(HRESULT result, const char* operation);
	uint64 CurrentLogicalPipelineKey() const;
	uint64 ShaderFailureKey(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash) const;

	struct FeedbackSnapshot
	{
		Microsoft::WRL::ComPtr<ID3D11Resource> source;
		Microsoft::WRL::ComPtr<ID3D11Resource> copy;
		uint64 descriptorKey{};
		uint32 lastUsedFrame{};
	};

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context1;
	Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
	Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backBuffer;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferView;
	UINT m_backBufferWidth{};
	UINT m_backBufferHeight{};
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_bufferCache;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_bufferCopyScratch;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexRingBuffer;
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutBuffers{};
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_streamoutStorageBuffer;
	Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_streamoutStorageUav;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streamoutCaptureTarget;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_streamoutCaptureTargetView;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_streamoutCaptureDepthState;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_streamoutCaptureBlendState;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_streamoutCaptureConstants;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_streamoutCaptureRecordMap;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_streamoutCaptureRecordMapView;
	UINT m_streamoutCaptureConstantsCapacity{};
	UINT m_streamoutCaptureRecordMapCapacity{};
	std::vector<uint8> m_bufferCacheShadow;
	std::vector<std::pair<UINT, UINT>> m_bufferCacheDirtyRanges;
	std::vector<uint8> m_uploadBuffer;
	std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE * 3> m_boundTextures{};
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,
		D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 3> m_boundShaderResources{};
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>,
		D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 3> m_boundSamplers{};
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>,
		D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT>, 3> m_boundConstantBuffers{};
	// Keep the emulated binding separate from the physical SRV installed on the
	// immediate context. Feedback-loop resolution may temporarily bind a snapshot;
	// overwriting the logical entry made subsequent draws keep sampling stale data.
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,
		D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 3> m_logicalShaderResources{};
	std::array<UINT, 16> m_vertexOffsets{};
	std::array<UINT, 16> m_vertexStrides{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 16> m_vertexBuffers{};
	std::array<bool, 16> m_vertexBindingValid{};
	std::array<std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, LATTE_NUM_MAX_UNIFORM_BUFFERS>, 3> m_uniformBuffers{};
	std::array<std::array<UINT, LATTE_NUM_MAX_UNIFORM_BUFFERS>, 3> m_uniformBufferCapacity{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 3> m_uniformVarsBuffers{};
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 3> m_samplerSwizzleBuffers{};
	std::array<UINT, 3> m_uniformVarsBufferCapacity{};
	std::array<UINT, 3> m_samplerSwizzleBufferCapacity{};
	std::array<std::vector<uint8>, 3> m_uniformScratch{};
	std::array<std::vector<uint8>, 3> m_uploadedUniformScratch{};
	std::array<bool, 3> m_uniformScratchUploaded{};
	std::array<uint64, 3> m_uniformShaderKeys{};
	std::array<std::array<std::array<uint32, 4>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 3>
		m_samplerSwizzles{};
	decltype(m_samplerSwizzles) m_uploadedSamplerSwizzles{};
	std::array<bool, 3> m_samplerSwizzleUploaded{};
	std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutOffsets{};
	std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutRangeSizes{};
	std::array<bool, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutEnabled{};
	std::array<bool, 8> m_boundColorBlendable{ true, true, true, true, true, true, true, true };
	bool m_streamoutActive{};
	bool m_streamoutUsesPixelCapture{};
	bool m_streamoutNativeRasterized{};
	bool m_streamoutDataAvailable{};
	bool m_keepIndexStagingForPixelStreamout{};
	RendererShader* m_streamoutPixelCaptureShader{};

	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_presentVS;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_presentPS;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_presentUniformBuffer;
	UINT m_presentUniformBufferCapacity{};
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
	std::unordered_set<uint64> m_seenLogicalPipelines;
	std::unordered_set<uint64> m_failedShaderKeys;
	std::mutex m_failedShaderMutex;
	std::unordered_set<uint32> m_reportedDebugWarnings;
	std::vector<Microsoft::WRL::ComPtr<ID3D11Resource>> m_feedbackResources;
	std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_feedbackViews;
	std::vector<FeedbackSnapshot> m_feedbackSnapshots;
	LatteCachedFBO* m_activeFbo{};
	bool m_activeFeedbackLoop{};
	uint64 m_inputLayoutKey{};
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_appliedRasterizerState;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_appliedBlendState;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_appliedDepthStencilState;
	std::array<float, 4> m_appliedBlendConstant{};
	UINT m_appliedStencilRef{};
	D3D11_PRIMITIVE_TOPOLOGY m_appliedPrimitiveTopology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
	D3D11_VIEWPORT m_appliedViewport{};
	D3D11_RECT m_appliedScissor{};
	bool m_appliedBlendStateValid{};
	bool m_appliedDepthStencilStateValid{};
	bool m_appliedViewportValid{};
	bool m_appliedScissorValid{};
	uint64 m_indexUploadCount{};
	uint64 m_indexRingWrapCount{};
	uint32 m_deviceHealthFrame{ 0xFFFFFFFFu };
	uint64 m_lastVertexShaderBase{};
	uint64 m_lastVertexShaderAux{};
	uint64 m_lastPixelShaderBase{};
	uint64 m_lastPixelShaderAux{};
	uint64 m_lastGeometryShaderBase{};
	uint64 m_lastGeometryShaderAux{};
	UINT m_indexRingCapacity{};
	UINT m_indexRingOffset{};
	UINT m_bufferCopyScratchCapacity{};
	uint32 m_memoryCheckFrame{};
	uint32 m_lastHeavyMemoryRecoveryFrame{};
	uint64 m_lastHeavyMemoryRecoveryCommitMB{};
	std::atomic<uint32> m_compiledShaderCount{};
	std::atomic_bool m_deviceLost{};
	bool m_memoryPressureActive{};
	bool m_inputLayoutKeyValid{};
	bool m_imguiInitialized{};
	bool m_debugModeEnabled{};
	bool m_graphicsStateInvalid{};
};
