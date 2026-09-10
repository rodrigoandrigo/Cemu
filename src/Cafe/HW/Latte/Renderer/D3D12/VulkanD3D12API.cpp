#include "Cafe/HW/Latte/Renderer/D3D12/VulkanD3D12API.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cemu/CemuEmbed.h"
#include "interface/WindowSystem.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <d3dcompiler.h>
#include <spirv_cross/spirv_hlsl.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

struct VkInstance_T;
struct VkDevice_T;
struct VkPhysicalDevice_T
{
	VkInstance_T* instance{};
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<ID3D12Device> native;
	DXGI_ADAPTER_DESC1 desc{};
};
struct VkInstance_T { std::unique_ptr<VkPhysicalDevice_T> physical; };
struct VkQueue_T { VkDevice_T* device{}; ComPtr<ID3D12CommandQueue> native; };
struct VkDevice_T
{
	VkPhysicalDevice_T* physical{};
	ComPtr<ID3D12Device> native;
	VkQueue_T queue;
	ComPtr<ID3D12DescriptorHeap> resourceHeap;
	ComPtr<ID3D12DescriptorHeap> clearCpuHeap;
	ComPtr<ID3D12DescriptorHeap> samplerHeap;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	ComPtr<ID3D12DescriptorHeap> dsvHeap;
	ComPtr<ID3D12RootSignature> blitRoot;
	ComPtr<ID3D12PipelineState> blitPipeline;
	UINT resourceStride{}, samplerStride{}, rtvStride{}, dsvStride{};
	std::atomic_uint32_t resourceCursor{}, samplerCursor{}, rtvCursor{}, dsvCursor{};
	std::mutex viewDescriptorMutex;
	std::vector<uint32_t> freeRtvSlots;
	std::vector<uint32_t> freeDsvSlots;
};
struct VkSurfaceKHR_T { HWND window{}; };
struct VkDeviceMemory_T
{
	VkDevice device{};
	VkDeviceSize size{};
	uint32_t type{};
	ComPtr<ID3D12Heap> heap;
	ComPtr<ID3D12Resource> mapResource;
	void* mapped{};
};
struct VkBuffer_T
{
	VkDevice device{};
	VkDeviceSize size{};
	VkBufferUsageFlags usage{};
	VkDeviceMemory memory{};
	VkDeviceSize memoryOffset{};
	ComPtr<ID3D12Resource> resource;
	D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
};
struct VkImage_T
{
	VkDevice device{};
	VkImageCreateInfo info{};
	VkDeviceMemory memory{};
	VkDeviceSize memoryOffset{};
	ComPtr<ID3D12Resource> resource;
	D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
};
struct VkFence_T { ComPtr<ID3D12Fence> native; uint64_t value{1}; HANDLE eventHandle{}; };
struct VkSemaphore_T { ComPtr<ID3D12Fence> native; std::atomic_uint64_t value{0}; };
struct VkCommandPool_T { VkDevice device{}; };
struct VkCommandBuffer_T
{
	VkDevice device{};
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	bool recording{};
	VkRenderPass activeRenderPass{};
	VkFramebuffer activeFramebuffer{};
	VkPipeline activePipeline{};
	std::vector<ComPtr<ID3D12PipelineState>> transientStates;
	std::vector<ComPtr<ID3D12Resource>> transientResources;
	std::vector<const char*> operationTrace;
};
struct VkDebugUtilsMessengerEXT_T
{
	PFN_vkDebugUtilsMessengerCallbackEXT callback{};
	void* userData{};
};
struct VkDebugReportCallbackEXT_T { PFN_vkDebugReportCallbackEXT callback{}; void* userData{}; };
struct VkEvent_T { std::atomic_bool signaled{}; };
struct VkShaderModule_T { std::vector<uint32_t> spirv; };
struct VkImageView_T { VkDevice device{}; VkImage image{}; VkImageViewCreateInfo info{}; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{}; uint32_t rtvSlot{UINT32_MAX},dsvSlot{UINT32_MAX}; };
struct VkSampler_T { VkSamplerCreateInfo info{}; };
struct VkDescriptorSetLayout_T { std::vector<VkDescriptorSetLayoutBinding> bindings; };
struct DescriptorValue
{
	VkDescriptorType type{};
	VkDescriptorBufferInfo buffer{};
	VkDescriptorImageInfo image{};
};
struct VkDescriptorSet_T
{
	VkDevice device{};
	VkDescriptorSetLayout layout{};
	std::unordered_map<uint64_t, DescriptorValue> values;
	std::array<uint32_t,4> base{};
	std::array<uint32_t,4> count{};
};
struct VkDescriptorPool_T { std::vector<VkDescriptorSet> sets; };
struct VkPipelineLayout_T { ComPtr<ID3D12RootSignature> root; uint32_t setCount{}; uint32_t pushRootIndex{UINT32_MAX}; uint32_t pushDwords{}; };
struct VkPipeline_T { ComPtr<ID3D12PipelineState> state; ComPtr<ID3DBlob> vertexShader,pixelShader,geometryShader; VkPipelineLayout layout{}; D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc{}; std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements; D3D12_PRIMITIVE_TOPOLOGY topology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}; bool compute{}; };
struct VkRenderPass_T
{
	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkAttachmentReference> colors;
	VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED};
};
struct VkFramebuffer_T { VkRenderPass pass{}; std::vector<VkImageView> attachments; uint32_t width{},height{}; };
struct VkQueryPool_T { VkDevice device{}; ComPtr<ID3D12QueryHeap> heap; ComPtr<ID3D12Resource> readback; VkQueryType type{}; uint32_t count{}; };
struct VkPipelineCache_T { std::vector<uint8_t> data; };
struct VkSwapchainKHR_T { VkDevice device{}; ComPtr<IDXGISwapChain3> native; ComPtr<ISwapChainPanelNative> panel; int32_t (*setCompositionSwapChain)(void*,void*){}; void* compositionUserData{}; ComPtr<ID3D12Fence> presentFence; HANDLE presentEvent{}; uint64_t presentValue{}; std::vector<VkImage> images; uint32_t next{}; };

namespace VulkanD3D12
{
namespace
{
std::atomic_bool s_selected{};
std::mutex s_dispatchMutex;

void EnableDeviceRemovedDiagnostics()
{
	ComPtr<ID3D12Debug> debug;
	if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		cemuLog_log(LogType::Force,"D3D12 debug layer enabled for the experimental renderer");
	}
	else
		cemuLog_log(LogType::Force,"D3D12 debug layer unavailable; install Windows Graphics Tools for validation messages");
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
	if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
	{
		settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	}
}

void ConfigureValidationMessageFilter(ID3D12Device* device)
{
	if(!device)return;
	ComPtr<ID3D12InfoQueue> infoQueue;
	if(FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))return;
	D3D12_MESSAGE_ID hiddenIds[]=
	{
		D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
		D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
		D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET
	};
	D3D12_INFO_QUEUE_FILTER filter{};
	filter.DenyList.NumIDs=static_cast<UINT>(std::size(hiddenIds));
	filter.DenyList.pIDList=hiddenIds;
	if(SUCCEEDED(infoQueue->AddStorageFilterEntries(&filter)))
		cemuLog_log(LogType::Force,"D3D12 validation filter: repetitive warnings 679, 820 and 821 hidden");
}

void TraceOperation(VkCommandBuffer commandBuffer, const char* operation)
{
	if(!commandBuffer||!operation)return;
	if(commandBuffer->operationTrace.size()>=128)
		commandBuffer->operationTrace.erase(commandBuffer->operationTrace.begin(),commandBuffer->operationTrace.begin()+32);
	commandBuffer->operationTrace.push_back(operation);
}

void LogCommandBufferTrace(const VkSubmitInfo& submit)
{
	for(uint32_t i=0;i<submit.commandBufferCount;i++)
	{
		auto commandBuffer=submit.pCommandBuffers[i];
		if(!commandBuffer)continue;
		std::string trace;
		const size_t first=commandBuffer->operationTrace.size()>64?commandBuffer->operationTrace.size()-64:0;
		for(size_t n=first;n<commandBuffer->operationTrace.size();n++)
		{
			if(!trace.empty())trace.append(" -> ");
			trace.append(commandBuffer->operationTrace[n]);
		}
		cemuLog_log(LogType::Force,fmt::format("D3D12 submitted command buffer {} trace: {}",i,trace.empty()?"empty":trace));
	}
}

void LogDeviceRemovedDiagnostics(VkDevice device, const char* context)
{
	if(!device||!device->native)return;
	const HRESULT reason=device->native->GetDeviceRemovedReason();
	cemuLog_log(LogType::Force,fmt::format("D3D12 device removed during {} (HRESULT 0x{:08X})",context,static_cast<uint32_t>(reason)));

	ComPtr<ID3D12InfoQueue> infoQueue;
	if(SUCCEEDED(device->native.As(&infoQueue)))
	{
		const UINT64 messageCount=infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		const UINT64 first=messageCount>32?messageCount-32:0;
		for(UINT64 i=first;i<messageCount;i++)
		{
			SIZE_T size=0;
			if(FAILED(infoQueue->GetMessage(i,nullptr,&size))||!size)continue;
			std::vector<uint8_t> storage(size);
			auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if(SUCCEEDED(infoQueue->GetMessage(i,message,&size))&&message->pDescription)
				cemuLog_log(LogType::Force,fmt::format("D3D12 validation [{}:{}]: {}",static_cast<uint32_t>(message->Category),static_cast<uint32_t>(message->ID),message->pDescription));
		}
	}

	ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if(FAILED(device->native.As(&dred)))return;
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
	if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
	{
		const D3D12_AUTO_BREADCRUMB_NODE1* node=breadcrumbs.pHeadAutoBreadcrumbNode;
		for(uint32_t n=0;node&&n<64;n++,node=node->pNext)
		{
			const UINT completed=node->pLastBreadcrumbValue?*node->pLastBreadcrumbValue:0;
			const char* name=node->pCommandListDebugNameA?node->pCommandListDebugNameA:"unnamed";
			uint32_t operation=UINT32_MAX;
			if(completed>0&&node->pCommandHistory&&completed<=node->BreadcrumbCount)
				operation=static_cast<uint32_t>(node->pCommandHistory[completed-1]);
			cemuLog_log(LogType::Force,fmt::format("D3D12 DRED breadcrumb: list='{}', completed={}/{}, lastOp={}",name,completed,node->BreadcrumbCount,operation));
		}
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
	if(SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault))&&pageFault.PageFaultVA)
		cemuLog_log(LogType::Force,fmt::format("D3D12 DRED page fault at GPU VA 0x{:016X}",pageFault.PageFaultVA));
}

DXGI_FORMAT ToDxgiFormat(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
	case VK_FORMAT_R8_SNORM: return DXGI_FORMAT_R8_SNORM;
	case VK_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
	case VK_FORMAT_R8_SINT: return DXGI_FORMAT_R8_SINT;
	case VK_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
	case VK_FORMAT_R8G8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
	case VK_FORMAT_R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
	case VK_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_SINT;
	case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case VK_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
		case VK_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
	case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_A2B10G10R10_UINT_PACK32: return DXGI_FORMAT_R10G10B10A2_UINT;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return DXGI_FORMAT_R11G11B10_FLOAT;
	case VK_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
	case VK_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_SNORM;
	case VK_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
	case VK_FORMAT_R16_SINT: return DXGI_FORMAT_R16_SINT;
	case VK_FORMAT_R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
	case VK_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
	case VK_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
	case VK_FORMAT_R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
		case VK_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
		case VK_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
		case VK_FORMAT_R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
		case VK_FORMAT_R16G16B16A16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
		case VK_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
	case VK_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
	case VK_FORMAT_R32_SINT: return DXGI_FORMAT_R32_SINT;
	case VK_FORMAT_R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
	case VK_FORMAT_R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
	case VK_FORMAT_R32G32_SINT: return DXGI_FORMAT_R32G32_SINT;
	case VK_FORMAT_R32G32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
	case VK_FORMAT_R32G32B32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
	case VK_FORMAT_R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
	case VK_FORMAT_R32G32B32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
	case VK_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
	case VK_FORMAT_R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
	case VK_FORMAT_R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case VK_FORMAT_R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return DXGI_FORMAT_B4G4R4A4_UNORM;
		case VK_FORMAT_R5G6B5_UNORM_PACK16: return DXGI_FORMAT_B5G6R5_UNORM;
		case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return DXGI_FORMAT_B5G5R5A1_UNORM;
	case VK_FORMAT_R8G8B8A8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
	case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
	case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_D32_FLOAT;
		case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return DXGI_FORMAT_BC1_UNORM;
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return DXGI_FORMAT_BC1_UNORM_SRGB;
	case VK_FORMAT_BC2_UNORM_BLOCK: return DXGI_FORMAT_BC2_UNORM;
	case VK_FORMAT_BC2_SRGB_BLOCK: return DXGI_FORMAT_BC2_UNORM_SRGB;
	case VK_FORMAT_BC3_UNORM_BLOCK: return DXGI_FORMAT_BC3_UNORM;
	case VK_FORMAT_BC3_SRGB_BLOCK: return DXGI_FORMAT_BC3_UNORM_SRGB;
	case VK_FORMAT_BC4_UNORM_BLOCK: return DXGI_FORMAT_BC4_UNORM;
	case VK_FORMAT_BC4_SNORM_BLOCK: return DXGI_FORMAT_BC4_SNORM;
	case VK_FORMAT_BC5_UNORM_BLOCK: return DXGI_FORMAT_BC5_UNORM;
	case VK_FORMAT_BC5_SNORM_BLOCK: return DXGI_FORMAT_BC5_SNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT ToDxgiResourceFormat(VkFormat format)
{
	switch(format)
	{
	case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
	case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
	case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_R32_TYPELESS;
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
	default: return ToDxgiFormat(format);
	}
}

DXGI_FORMAT ToDxgiShaderResourceFormat(VkFormat format,VkImageAspectFlags aspectMask)
{
	switch(format)
	{
	case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
	case VK_FORMAT_D24_UNORM_S8_UINT: return (aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:DXGI_FORMAT_X24_TYPELESS_G8_UINT;
	case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return (aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
	default: return ToDxgiFormat(format);
	}
}

D3D12_HEAP_TYPE HeapType(uint32_t memoryType)
{
	return memoryType == 0 ? D3D12_HEAP_TYPE_DEFAULT :
		(memoryType == 1 ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
}

uint32_t DescriptorClass(VkDescriptorType type)
{
	switch (type)
	{
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return 0;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return 1;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 2;
	case VK_DESCRIPTOR_TYPE_SAMPLER: return 3;
	default: return UINT32_MAX;
	}
}

bool CreateDescriptorHeap(VkDevice_T* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
	D3D12_DESCRIPTOR_HEAP_FLAGS flags, ComPtr<ID3D12DescriptorHeap>& heap, UINT& stride)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = type; desc.NumDescriptors = count; desc.Flags = flags;
	if (FAILED(device->native->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return false;
	stride = device->native->GetDescriptorHandleIncrementSize(type);
	return true;
}

bool CreateBlitPipeline(VkDevice_T* device)
{
	D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&ranges[0]};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={1,&ranges[1]};params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,8};for(auto& p:params)p.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;ComPtr<ID3DBlob> root,error;if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root,&error))||FAILED(device->native->CreateRootSignature(0,root->GetBufferPointer(),root->GetBufferSize(),IID_PPV_ARGS(&device->blitRoot))))return false;
	static constexpr char shader[]="Texture2D<float4> S:register(t0);RWTexture2D<float4> D:register(u0);cbuffer C:register(b0){int2 so;int2 ss;int2 do_;int2 ds;}[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){if(any(id.xy>=uint2(ds)))return;float2 q=(float2(id.xy)+.5)*float2(ss)/float2(ds)-.5;int2 p=clamp(int2(round(q))+so,so,so+ss-1);D[do_+int2(id.xy)]=S.Load(int3(p,0));}";ComPtr<ID3DBlob> code;if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL1,0,&code,&error)))return false;D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=device->blitRoot.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};return SUCCEEDED(device->native->CreateComputePipelineState(&pd,IID_PPV_ARGS(&device->blitPipeline)));
}

constexpr std::array<VkExtensionProperties, 3> kInstanceExtensions{{
	{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
	{VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
	{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_SPEC_VERSION},
}};
constexpr std::array<VkExtensionProperties, 5> kDeviceExtensions{{
	{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION},
	{VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_SPEC_VERSION},
	{VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_SPEC_VERSION},
	{VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_ID_SPEC_VERSION},
	{VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_SPEC_VERSION},
}};

template<typename T, size_t N>
VkResult Enumerate(const std::array<T, N>& source, uint32_t* count, T* output)
{
	if (!count) return VK_ERROR_INITIALIZATION_FAILED;
	if (!output) { *count = static_cast<uint32_t>(N); return VK_SUCCESS; }
	const uint32_t copied = (std::min)(*count, static_cast<uint32_t>(N));
	std::copy_n(source.begin(), copied, output);
	*count = copied;
	return copied == N ? VK_SUCCESS : VK_INCOMPLETE;
}

VkResult CreatePhysical(VkInstance_T* instance)
{
	ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
		return VK_ERROR_INITIALIZATION_FAILED;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; ; ++i)
	{
		ComPtr<IDXGIAdapter1> candidate;
		const HRESULT hr = factory->EnumAdapterByGpuPreference(i,
			DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(hr)) continue;
		DXGI_ADAPTER_DESC1 desc{};
		candidate->GetDesc1(&desc);
		if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
			SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
				__uuidof(ID3D12Device), nullptr)))
		{ adapter = candidate; break; }
	}
	if (!adapter) return VK_ERROR_INCOMPATIBLE_DRIVER;
	auto physical = std::make_unique<VkPhysicalDevice_T>();
	physical->instance = instance;
	physical->adapter = adapter;
	adapter->GetDesc1(&physical->desc);
	if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&physical->native))))
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	ConfigureValidationMessageFilter(physical->native.Get());
	instance->physical = std::move(physical);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL ICreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* out)
{
	if (!out) return VK_ERROR_INITIALIZATION_FAILED;
	auto instance = std::make_unique<VkInstance_T>();
	const VkResult result = CreatePhysical(instance.get());
	if (result != VK_SUCCESS) return result;
	*out = instance.release();
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) { delete instance; }
VKAPI_ATTR VkResult VKAPI_CALL IEnumerateInstanceVersion(uint32_t* version)
{ if (!version) return VK_ERROR_INITIALIZATION_FAILED; *version = VK_API_VERSION_1_2; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL IEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count, VkExtensionProperties* out)
{ return layer ? VK_ERROR_LAYER_NOT_PRESENT : Enumerate(kInstanceExtensions, count, out); }
VKAPI_ATTR VkResult VKAPI_CALL IEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* layer, uint32_t* count, VkExtensionProperties* out)
{ return layer ? VK_ERROR_LAYER_NOT_PRESENT : Enumerate(kDeviceExtensions, count, out); }
VKAPI_ATTR VkResult VKAPI_CALL IEnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* out)
{
	if (!instance || !instance->physical || !count) return VK_ERROR_INITIALIZATION_FAILED;
	if (!out) { *count = 1; return VK_SUCCESS; }
	if (!*count) return VK_INCOMPLETE;
	out[0] = instance->physical.get(); *count = 1; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* out)
{
	if (!count) return;
	if (!out) { *count = 1; return; }
	if (!*count) return;
	out[0] = {};
	out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
	out[0].queueCount = 1; out[0].timestampValidBits = 64;
	out[0].minImageTransferGranularity = {1, 1, 1}; *count = 1;
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out)
{
	if (!physical || !out) return;
	*out = {};
	out->apiVersion = VK_API_VERSION_1_2; out->driverVersion = VK_MAKE_VERSION(0, 1, 0);
	out->vendorID = physical->desc.VendorId; out->deviceID = physical->desc.DeviceId;
	out->deviceType = physical->desc.DedicatedVideoMemory ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	WideCharToMultiByte(CP_UTF8, 0, physical->desc.Description, -1, out->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, nullptr, nullptr);
	auto& l = out->limits;
	l.maxImageDimension1D = D3D12_REQ_TEXTURE1D_U_DIMENSION;
	l.maxImageDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	l.maxImageDimension3D = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
	l.maxImageDimensionCube = D3D12_REQ_TEXTURECUBE_DIMENSION;
	l.maxImageArrayLayers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
	l.maxUniformBufferRange = 65536; l.maxStorageBufferRange = UINT32_MAX;
	l.maxPushConstantsSize = 256; l.maxMemoryAllocationCount = 4096;
	l.maxBoundDescriptorSets = 8; l.maxPerStageDescriptorSamplers = 16;
	l.maxPerStageDescriptorSampledImages = 128; l.maxPerStageDescriptorStorageBuffers = 64;
	l.maxPerStageResources = 128; l.maxVertexInputAttributes = 32; l.maxVertexInputBindings = 32;
	l.maxVertexInputAttributeOffset = 2047; l.maxVertexInputBindingStride = 2048;
	l.maxColorAttachments = 8; l.maxViewports = 16;
	l.maxViewportDimensions[0] = l.maxViewportDimensions[1] = D3D12_VIEWPORT_BOUNDS_MAX;
	l.viewportBoundsRange[0] = D3D12_VIEWPORT_BOUNDS_MIN; l.viewportBoundsRange[1] = D3D12_VIEWPORT_BOUNDS_MAX;
	l.timestampPeriod = 1.0f;
	l.framebufferColorSampleCounts = l.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
	l.sampledImageColorSampleCounts = l.framebufferColorSampleCounts; l.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	l.minMemoryMapAlignment = l.minUniformBufferOffsetAlignment = 256;
	l.minStorageBufferOffsetAlignment = 16; l.optimalBufferCopyOffsetAlignment = 512;
	l.optimalBufferCopyRowPitchAlignment = 256; l.nonCoherentAtomSize = 1;
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* out)
{
	if (!out) return; IGetPhysicalDeviceProperties(physical, &out->properties);
	for (auto* next = static_cast<VkBaseOutStructure*>(out->pNext); next; next = next->pNext)
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)
		{
			auto* id = reinterpret_cast<VkPhysicalDeviceIDProperties*>(next);
			std::memcpy(id->deviceUUID, &physical->desc.AdapterLuid, sizeof(physical->desc.AdapterLuid));
			std::memcpy(id->deviceLUID, &physical->desc.AdapterLuid, VK_LUID_SIZE);
			id->deviceLUIDValid = VK_TRUE;
		}
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out)
{
	if (!out) return; auto& f = out->features; f = {};
	f.fullDrawIndexUint32 = f.imageCubeArray = f.independentBlend = f.geometryShader = VK_TRUE;
	f.sampleRateShading = f.dualSrcBlend = f.logicOp = VK_TRUE;
	f.multiDrawIndirect = f.drawIndirectFirstInstance = f.depthClamp = f.depthBiasClamp = VK_TRUE;
	f.fillModeNonSolid = f.samplerAnisotropy = f.textureCompressionBC = VK_TRUE;
	f.vertexPipelineStoresAndAtomics = f.fragmentStoresAndAtomics = VK_TRUE;
	f.shaderImageGatherExtended = f.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	f.shaderClipDistance = f.shaderCullDistance = f.shaderInt16 = VK_TRUE;
	for(auto* next=static_cast<VkBaseOutStructure*>(out->pNext);next;next=next->pNext)
	{
		if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR)reinterpret_cast<VkPhysicalDeviceSynchronization2FeaturesKHR*>(next)->synchronization2=VK_TRUE;
		else if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)reinterpret_cast<VkPhysicalDevicePresentIdFeaturesKHR*>(next)->presentId=VK_TRUE;
		else if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)reinterpret_cast<VkPhysicalDevicePresentWaitFeaturesKHR*>(next)->presentWait=VK_TRUE;
	}
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* out)
{
	if (!physical || !out) return; *out = {}; out->memoryHeapCount = 2;
	out->memoryHeaps[0] = { physical->desc.DedicatedVideoMemory ? physical->desc.DedicatedVideoMemory : physical->desc.SharedSystemMemory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
	out->memoryHeaps[1] = { physical->desc.SharedSystemMemory, 0 };
	out->memoryTypeCount = 3;
	out->memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
		out->memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};
	out->memoryTypes[2] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};
}
VKAPI_ATTR void VKAPI_CALL IGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties* out)
{
		if (!out) return; *out = {};const DXGI_FORMAT nativeFormat=ToDxgiFormat(format);if(!physical||format==VK_FORMAT_UNDEFINED||nativeFormat==DXGI_FORMAT_UNKNOWN)return;D3D12_FEATURE_DATA_FORMAT_SUPPORT support{nativeFormat};if(FAILED(physical->native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))))return;
		const bool copySupported=(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE1D)||(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D)||(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE3D);if(copySupported)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		if(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT;
		if(support.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
		if(support.Support1&D3D12_FORMAT_SUPPORT1_BLENDABLE)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
		if(support.Support1&D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		if(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		if(support.Support1&D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER)out->bufferFeatures|=VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;if(support.Support1&D3D12_FORMAT_SUPPORT1_BUFFER)out->bufferFeatures|=VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;if(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)out->bufferFeatures|=VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL ICreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice* out)
{
	if (!physical || !out) return VK_ERROR_INITIALIZATION_FAILED;
	auto device = std::make_unique<VkDevice_T>(); device->physical = physical; device->native = physical->native;
	D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(device->native->CreateCommandQueue(&desc, IID_PPV_ARGS(&device->queue.native)))) return VK_ERROR_INITIALIZATION_FAILED;
	if (!CreateDescriptorHeap(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536,
		D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, device->resourceHeap, device->resourceStride) ||
		!CreateDescriptorHeap(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096,
			D3D12_DESCRIPTOR_HEAP_FLAG_NONE, device->clearCpuHeap, device->resourceStride) ||
		!CreateDescriptorHeap(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048,
			D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, device->samplerHeap, device->samplerStride) ||
		!CreateDescriptorHeap(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096,
			D3D12_DESCRIPTOR_HEAP_FLAG_NONE, device->rtvHeap, device->rtvStride) ||
		!CreateDescriptorHeap(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2048,
			D3D12_DESCRIPTOR_HEAP_FLAG_NONE, device->dsvHeap, device->dsvStride))
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if(!CreateBlitPipeline(device.get()))return VK_ERROR_INITIALIZATION_FAILED;
	device->queue.device = device.get(); *out = device.release(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IDestroyDevice(VkDevice device, const VkAllocationCallbacks*) { delete device; }
VKAPI_ATTR void VKAPI_CALL IGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out)
{ if (out) *out = device && family == 0 && index == 0 ? &device->queue : VK_NULL_HANDLE; }
VKAPI_ATTR VkResult VKAPI_CALL IDeviceWaitIdle(VkDevice device)
{
	if (!device) return VK_ERROR_DEVICE_LOST;
	ComPtr<ID3D12Fence> fence;
	if (FAILED(device->native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || FAILED(device->queue.native->Signal(fence.Get(), 1))) return VK_ERROR_DEVICE_LOST;
	if (fence->GetCompletedValue() < 1)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		if (!eventHandle) return VK_ERROR_OUT_OF_HOST_MEMORY;
		const HRESULT hr = fence->SetEventOnCompletion(1, eventHandle);
		if (SUCCEEDED(hr)) WaitForSingleObjectEx(eventHandle, INFINITE, FALSE);
		CloseHandle(eventHandle); if (FAILED(hr)) return VK_ERROR_DEVICE_LOST;
	}
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL IAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* out)
{
	if (!device || !info || !out || info->memoryTypeIndex >= 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	auto memory = std::make_unique<VkDeviceMemory_T>(); memory->device=device; memory->size=info->allocationSize; memory->type=info->memoryTypeIndex;
	D3D12_HEAP_DESC desc{}; desc.SizeInBytes=(info->allocationSize+65535)&~VkDeviceSize(65535); desc.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	desc.Properties.Type=HeapType(memory->type); desc.Properties.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_UNKNOWN; desc.Properties.MemoryPoolPreference=D3D12_MEMORY_POOL_UNKNOWN;
		desc.Properties.CreationNodeMask=desc.Properties.VisibleNodeMask=1; desc.Flags=memory->type==0?D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES:D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	if (FAILED(device->native->CreateHeap(&desc,IID_PPV_ARGS(&memory->heap)))) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if(memory->type!=0)
	{
		D3D12_RESOURCE_DESC resourceDesc{};resourceDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;resourceDesc.Width=desc.SizeInBytes;resourceDesc.Height=1;resourceDesc.DepthOrArraySize=1;resourceDesc.MipLevels=1;resourceDesc.SampleDesc.Count=1;resourceDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		const auto initialState=memory->type==1?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST;
		if(FAILED(device->native->CreatePlacedResource(memory->heap.Get(),0,&resourceDesc,initialState,nullptr,IID_PPV_ARGS(&memory->mapResource))))return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	*out=memory.release(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*)
{ if(memory){ if(memory->mapped&&memory->mapResource) memory->mapResource->Unmap(0,nullptr); delete memory; } }
VKAPI_ATTR VkResult VKAPI_CALL ICreateBuffer(VkDevice device,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* out)
{ if(!device||!info||!out) return VK_ERROR_INITIALIZATION_FAILED; auto b=std::make_unique<VkBuffer_T>(); b->device=device;b->size=info->size;b->usage=info->usage;*out=b.release();return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL IDestroyBuffer(VkDevice,VkBuffer buffer,const VkAllocationCallbacks*) { delete buffer; }
VkDeviceSize NativeBufferSize(VkDeviceSize size){const VkDeviceSize padding=(std::max)(VkDeviceSize(64*1024),size/4);return (size+padding+65535)&~VkDeviceSize(65535);}
VKAPI_ATTR void VKAPI_CALL IGetBufferMemoryRequirements(VkDevice device,VkBuffer buffer,VkMemoryRequirements* out)
{
if(!device||!buffer||!out)return; D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=NativeBufferSize(buffer->size);d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;if(buffer->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		auto a=device->native->GetResourceAllocationInfo(0,1,&d);out->size=a.SizeInBytes;out->alignment=a.Alignment;const VkBufferUsageFlags gpuRead=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;if(buffer->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)out->memoryTypeBits=0x1;else if(buffer->usage&gpuRead)out->memoryTypeBits=0x3;else if(buffer->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT)out->memoryTypeBits=0x5;else out->memoryTypeBits=0x7;
}
VKAPI_ATTR VkResult VKAPI_CALL IBindBufferMemory(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset)
{
		if(!device||!buffer||!memory)return VK_ERROR_INITIALIZATION_FAILED; D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=NativeBufferSize(buffer->size);d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;if(buffer->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if(memory->type==2&&(buffer->usage&(VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))){cemuLog_log(LogType::Force,fmt::format("D3D12 rejected READBACK memory for GPU-readable buffer (usage 0x{:X}, size {}, offset {})",static_cast<uint32_t>(buffer->usage),buffer->size,offset));return VK_ERROR_FEATURE_NOT_PRESENT;}
		const auto state=memory->type==1?D3D12_RESOURCE_STATE_GENERIC_READ:(memory->type==2?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON);
		if(memory->type!=0)buffer->resource=memory->mapResource;else if(FAILED(device->native->CreatePlacedResource(memory->heap.Get(),offset,&d,state,nullptr,IID_PPV_ARGS(&buffer->resource))))return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		buffer->memory=memory;buffer->memoryOffset=offset;buffer->state=state;return buffer->resource?VK_SUCCESS:VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
VKAPI_ATTR VkResult VKAPI_CALL IMapMemory(VkDevice,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize requestedSize,const VkMemoryMapFlags,void** out)
{
	if(!memory||!out||memory->type==0||!memory->mapResource)return VK_ERROR_MEMORY_MAP_FAILED;const VkDeviceSize resourceSize=memory->mapResource->GetDesc().Width;if(offset>resourceSize)return VK_ERROR_MEMORY_MAP_FAILED;const VkDeviceSize available=resourceSize-offset;const VkDeviceSize mappedSize=requestedSize==VK_WHOLE_SIZE?available:(std::min)(requestedSize,available);void* base{};D3D12_RANGE readRange{static_cast<SIZE_T>(offset),memory->type==2?static_cast<SIZE_T>(offset+mappedSize):static_cast<SIZE_T>(offset)};
	if(FAILED(memory->mapResource->Map(0,&readRange,&base)))return VK_ERROR_MEMORY_MAP_FAILED;memory->mapped=base;*out=static_cast<uint8_t*>(base)+offset;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IUnmapMemory(VkDevice,VkDeviceMemory memory){if(memory&&memory->mapped&&memory->mapResource){memory->mapResource->Unmap(0,nullptr);memory->mapped=nullptr;}}
VKAPI_ATTR VkResult VKAPI_CALL IFlushMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL IInvalidateMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateImage(VkDevice device,const VkImageCreateInfo* info,const VkAllocationCallbacks*,VkImage* out)
{if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;const auto nativeFormat=ToDxgiFormat(info->format);if(nativeFormat==DXGI_FORMAT_UNKNOWN){cemuLog_log(LogType::Force,fmt::format("D3D12 image creation rejected unsupported Vulkan format {} (type {}, {}x{}x{}, mipLevels {}, layers {}, usage 0x{:X}, flags 0x{:X})",static_cast<uint32_t>(info->format),static_cast<uint32_t>(info->imageType),info->extent.width,info->extent.height,info->extent.depth,info->mipLevels,info->arrayLayers,static_cast<uint32_t>(info->usage),static_cast<uint32_t>(info->flags)));return VK_ERROR_FORMAT_NOT_SUPPORTED;}auto image=std::make_unique<VkImage_T>();image->device=device;image->info=*info;*out=image.release();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyImage(VkDevice,VkImage image,const VkAllocationCallbacks*){delete image;}
D3D12_RESOURCE_DESC ImageDesc(VkImage image)
{D3D12_RESOURCE_DESC d{};d.Dimension=image->info.imageType==VK_IMAGE_TYPE_1D?D3D12_RESOURCE_DIMENSION_TEXTURE1D:(image->info.imageType==VK_IMAGE_TYPE_3D?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D);d.Width=image->info.extent.width;d.Height=image->info.extent.height;d.DepthOrArraySize=static_cast<UINT16>(image->info.imageType==VK_IMAGE_TYPE_3D?image->info.extent.depth:image->info.arrayLayers);d.MipLevels=static_cast<UINT16>(image->info.mipLevels);d.Format=ToDxgiResourceFormat(image->info.format);d.SampleDesc.Count=static_cast<UINT>(image->info.samples);d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;if(image->info.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;if(image->info.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;if(image->info.usage&VK_IMAGE_USAGE_STORAGE_BIT){D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ToDxgiFormat(image->info.format)};if(SUCCEEDED(image->device->native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))&&(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;}return d;}
VKAPI_ATTR void VKAPI_CALL IGetImageMemoryRequirements(VkDevice device,VkImage image,VkMemoryRequirements* out)
{if(!device||!image||!out)return;auto d=ImageDesc(image);auto a=device->native->GetResourceAllocationInfo(0,1,&d);out->size=a.SizeInBytes;out->alignment=a.Alignment;out->memoryTypeBits=1;}
VKAPI_ATTR VkResult VKAPI_CALL IBindImageMemory(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset)
{if(!device||!image||!memory||memory->type!=0)return VK_ERROR_INITIALIZATION_FAILED;auto d=ImageDesc(image);const HRESULT hr=device->native->CreatePlacedResource(memory->heap.Get(),offset,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image->resource));if(FAILED(hr)){const HRESULT removed=device->native->GetDeviceRemovedReason();cemuLog_log(LogType::Force,fmt::format("D3D12 image allocation failed (HRESULT 0x{:08X}, removed 0x{:08X}, dimension {}, format {}, {}x{}x{})",static_cast<uint32_t>(hr),static_cast<uint32_t>(removed),static_cast<uint32_t>(d.Dimension),static_cast<uint32_t>(d.Format),d.Width,d.Height,d.DepthOrArraySize));return VK_ERROR_OUT_OF_DEVICE_MEMORY;}image->memory=memory;image->memoryOffset=offset;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateCommandPool(VkDevice device,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* out)
{if(!device||!out)return VK_ERROR_INITIALIZATION_FAILED;auto p=new VkCommandPool_T();p->device=device;*out=p;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyCommandPool(VkDevice,VkCommandPool pool,const VkAllocationCallbacks*){delete pool;}
VKAPI_ATTR VkResult VKAPI_CALL IAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out)
{if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;for(uint32_t i=0;i<info->commandBufferCount;i++){auto c=std::make_unique<VkCommandBuffer_T>();c->device=device;if(FAILED(device->native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c->allocator)))||FAILED(device->native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c->allocator.Get(),nullptr,IID_PPV_ARGS(&c->list))))return VK_ERROR_OUT_OF_HOST_MEMORY;c->list->Close();out[i]=c.release();}return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IFreeCommandBuffers(VkDevice,VkCommandPool,uint32_t count,const VkCommandBuffer* buffers){for(uint32_t i=0;i<count;i++)delete buffers[i];}
VKAPI_ATTR VkResult VKAPI_CALL IBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo*){if(!c)return VK_ERROR_INITIALIZATION_FAILED;c->transientStates.clear();c->transientResources.clear();c->operationTrace.clear();c->activePipeline=VK_NULL_HANDLE;c->activeRenderPass=VK_NULL_HANDLE;c->activeFramebuffer=VK_NULL_HANDLE;if(FAILED(c->allocator->Reset())||FAILED(c->list->Reset(c->allocator.Get(),nullptr)))return VK_ERROR_DEVICE_LOST;c->recording=true;TraceOperation(c,"BeginCommandBuffer");return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL IEndCommandBuffer(VkCommandBuffer c){if(!c||!c->recording)return VK_ERROR_INITIALIZATION_FAILED;TraceOperation(c,"EndCommandBuffer");c->recording=false;return SUCCEEDED(c->list->Close())?VK_SUCCESS:VK_ERROR_DEVICE_LOST;}
VKAPI_ATTR VkResult VKAPI_CALL IResetCommandBuffer(VkCommandBuffer c,VkCommandBufferResetFlags){return IBeginCommandBuffer(c,nullptr)==VK_SUCCESS?(c->list->Close(),c->recording=false,VK_SUCCESS):VK_ERROR_DEVICE_LOST;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateFence(VkDevice device,const VkFenceCreateInfo* info,const VkAllocationCallbacks*,VkFence* out)
{if(!device||!out)return VK_ERROR_INITIALIZATION_FAILED;auto f=std::make_unique<VkFence_T>();const UINT64 initial=(info&&info->flags&VK_FENCE_CREATE_SIGNALED_BIT)?1:0;if(FAILED(device->native->CreateFence(initial,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->native))))return VK_ERROR_OUT_OF_HOST_MEMORY;f->eventHandle=CreateEventEx(nullptr,nullptr,0,EVENT_ALL_ACCESS);if(!f->eventHandle)return VK_ERROR_OUT_OF_HOST_MEMORY;*out=f.release();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyFence(VkDevice,VkFence f,const VkAllocationCallbacks*){if(f){if(f->eventHandle)CloseHandle(f->eventHandle);delete f;}}
VKAPI_ATTR VkResult VKAPI_CALL IGetFenceStatus(VkDevice,VkFence f){return f&&f->native->GetCompletedValue()>=f->value?VK_SUCCESS:VK_NOT_READY;}
VKAPI_ATTR VkResult VKAPI_CALL IResetFences(VkDevice,uint32_t count,const VkFence* fences){for(uint32_t i=0;i<count;i++)fences[i]->value=fences[i]->native->GetCompletedValue()+1;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL IWaitForFences(VkDevice,uint32_t count,const VkFence* fences,VkBool32 waitAll,uint64_t timeout)
{const DWORD ms=timeout==UINT64_MAX?INFINITE:static_cast<DWORD>((std::min)(timeout/1000000ull,uint64_t(INFINITE-1)));for(uint32_t i=0;i<count;i++){auto f=fences[i];if(f->native->GetCompletedValue()>=f->value){if(!waitAll)return VK_SUCCESS;continue;}if(FAILED(f->native->SetEventOnCompletion(f->value,f->eventHandle)))return VK_ERROR_DEVICE_LOST;if(WaitForSingleObjectEx(f->eventHandle,ms,FALSE)!=WAIT_OBJECT_0)return VK_TIMEOUT;if(!waitAll)return VK_SUCCESS;}return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateSemaphore(VkDevice device,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* out)
{if(!device||!out)return VK_ERROR_INITIALIZATION_FAILED;auto s=std::make_unique<VkSemaphore_T>();if(FAILED(device->native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->native))))return VK_ERROR_OUT_OF_HOST_MEMORY;*out=s.release();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroySemaphore(VkDevice,VkSemaphore s,const VkAllocationCallbacks*){delete s;}
VKAPI_ATTR void VKAPI_CALL ICmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*);
VKAPI_ATTR VkResult VKAPI_CALL ICreateEvent(VkDevice,const VkEventCreateInfo*,const VkAllocationCallbacks*,VkEvent* out){if(!out)return VK_ERROR_INITIALIZATION_FAILED;*out=new VkEvent_T();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyEvent(VkDevice,VkEvent event,const VkAllocationCallbacks*){delete event;}
VKAPI_ATTR VkResult VKAPI_CALL IGetEventStatus(VkDevice,VkEvent event){return event&&event->signaled.load()?VK_EVENT_SET:VK_EVENT_RESET;}
VKAPI_ATTR void VKAPI_CALL ICmdSetEvent(VkCommandBuffer,VkEvent event,VkPipelineStageFlags){if(event)event->signaled.store(true);}
VKAPI_ATTR void VKAPI_CALL ICmdWaitEvents(VkCommandBuffer c,uint32_t count,const VkEvent* events,VkPipelineStageFlags,VkPipelineStageFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t imageCount,const VkImageMemoryBarrier* images){if(!c)return;for(uint32_t i=0;i<count;i++)if(events[i])(void)events[i]->signaled.load();ICmdPipelineBarrier(c,0,0,0,0,nullptr,0,nullptr,imageCount,images);}
VKAPI_ATTR VkResult VKAPI_CALL IQueueSubmit(VkQueue q,uint32_t count,const VkSubmitInfo* submits,VkFence fence)
{if(!q)return VK_ERROR_DEVICE_LOST;for(uint32_t s=0;s<count;s++){for(uint32_t i=0;i<submits[s].waitSemaphoreCount;i++){auto sem=submits[s].pWaitSemaphores[i];if(FAILED(q->native->Wait(sem->native.Get(),sem->value.load()))){LogCommandBufferTrace(submits[s]);LogDeviceRemovedDiagnostics(q->device,"queue wait");return VK_ERROR_DEVICE_LOST;}}std::vector<ID3D12CommandList*> lists;for(uint32_t i=0;i<submits[s].commandBufferCount;i++)if(submits[s].pCommandBuffers[i]&&submits[s].pCommandBuffers[i]->list)lists.push_back(submits[s].pCommandBuffers[i]->list.Get());if(!lists.empty())q->native->ExecuteCommandLists(static_cast<UINT>(lists.size()),lists.data());for(uint32_t i=0;i<submits[s].signalSemaphoreCount;i++){auto sem=submits[s].pSignalSemaphores[i];const auto value=sem->value.fetch_add(1)+1;if(FAILED(q->native->Signal(sem->native.Get(),value))){LogCommandBufferTrace(submits[s]);LogDeviceRemovedDiagnostics(q->device,"queue semaphore signal");return VK_ERROR_DEVICE_LOST;}}const HRESULT removed=q->device->native->GetDeviceRemovedReason();if(FAILED(removed)){LogCommandBufferTrace(submits[s]);LogDeviceRemovedDiagnostics(q->device,"queue submission");return VK_ERROR_DEVICE_LOST;}}if(fence&&FAILED(q->native->Signal(fence->native.Get(),fence->value))){if(count)LogCommandBufferTrace(submits[count-1]);LogDeviceRemovedDiagnostics(q->device,"queue fence signal");return VK_ERROR_DEVICE_LOST;}return VK_SUCCESS;}
D3D12_RESOURCE_STATES StateForLayout(VkImage image,VkImageLayout layout)
{
	if(layout==VK_IMAGE_LAYOUT_GENERAL&&image&&image->resource)
	{
		const auto flags=image->resource->GetDesc().Flags;
		if(flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if(flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)return D3D12_RESOURCE_STATE_RENDER_TARGET;
		if(flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		return D3D12_RESOURCE_STATE_COMMON;
	}
	switch(layout){case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return D3D12_RESOURCE_STATE_RENDER_TARGET;case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:return D3D12_RESOURCE_STATE_DEPTH_WRITE;case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:return D3D12_RESOURCE_STATE_DEPTH_READ;case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return D3D12_RESOURCE_STATE_COPY_SOURCE;case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return D3D12_RESOURCE_STATE_COPY_DEST;case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:return D3D12_RESOURCE_STATE_PRESENT;default:return D3D12_RESOURCE_STATE_COMMON;}
}
void TransitionBuffer(VkCommandBuffer c,VkBuffer buffer,D3D12_RESOURCE_STATES target)
{
	if(!c||!buffer||!buffer->resource||buffer->state==target||buffer->memory&&buffer->memory->type==1)return;
	D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={buffer->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,buffer->state,target};c->list->ResourceBarrier(1,&barrier);buffer->state=target;
}
void TransitionImage(VkCommandBuffer c,VkImage image,D3D12_RESOURCE_STATES target)
{
	if(!c||!image||!image->resource||image->state==target)return;D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,target};c->list->ResourceBarrier(1,&barrier);image->state=target;
}
VKAPI_ATTR void VKAPI_CALL ICmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t imageCount,const VkImageMemoryBarrier* images)
{if(!c)return;std::vector<D3D12_RESOURCE_BARRIER> barriers;for(uint32_t i=0;i<imageCount;i++){auto image=images[i].image;if(!image||!image->resource)continue;auto target=StateForLayout(image,images[i].newLayout);if(target==image->state)continue;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=image->resource.Get();b.Transition.StateBefore=image->state;b.Transition.StateAfter=target;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barriers.push_back(b);image->state=target;}if(!barriers.empty())c->list->ResourceBarrier(static_cast<UINT>(barriers.size()),barriers.data());}
VKAPI_ATTR void VKAPI_CALL ICmdPipelineBarrier2KHR(VkCommandBuffer c,const VkDependencyInfoKHR* info)
{if(!c||!info)return;std::vector<D3D12_RESOURCE_BARRIER> barriers;for(uint32_t i=0;i<info->imageMemoryBarrierCount;i++){auto image=info->pImageMemoryBarriers[i].image;if(!image||!image->resource)continue;const auto target=StateForLayout(image,info->pImageMemoryBarriers[i].newLayout);if(target==image->state)continue;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,target};barriers.push_back(b);image->state=target;}if(info->memoryBarrierCount||info->bufferMemoryBarrierCount){D3D12_RESOURCE_BARRIER u{};u.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;u.UAV.pResource=nullptr;barriers.push_back(u);}if(!barriers.empty())c->list->ResourceBarrier(static_cast<UINT>(barriers.size()),barriers.data());}
VKAPI_ATTR void VKAPI_CALL ICmdCopyBuffer(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy* regions)
{
	if(!c||!src||!dst||!src->resource||!dst->resource||!regions)return;
	if(src->memory&&src->memory->type==2){cemuLog_log(LogType::Force,"D3D12 skipped invalid CopyBufferRegion whose source is READBACK memory");return;}
	if(dst->memory&&dst->memory->type==1){cemuLog_log(LogType::Force,"D3D12 skipped invalid CopyBufferRegion whose destination is UPLOAD memory");return;}
	if(src->resource.Get()==dst->resource.Get())
	{
		D3D12_RESOURCE_STATES nativeState=src->state;
		for(uint32_t i=0;i<count;i++)
		{
			if(!regions[i].size)continue;
			D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=regions[i].size;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> temporary;
			if(FAILED(c->device->native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&temporary))))
			{
				cemuLog_log(LogType::Force,"D3D12 failed to allocate temporary buffer for CopyBufferRegion between aliases");
				return;
			}
			if(nativeState!=D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={src->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,nativeState,D3D12_RESOURCE_STATE_COPY_SOURCE};c->list->ResourceBarrier(1,&barrier);
			}
			c->list->CopyBufferRegion(temporary.Get(),0,src->resource.Get(),src->memoryOffset+regions[i].srcOffset,regions[i].size);
			D3D12_RESOURCE_BARRIER barriers[2]{};
			barriers[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barriers[0].Transition={temporary.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};
			barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barriers[1].Transition={src->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
			c->list->ResourceBarrier(2,barriers);
			c->list->CopyBufferRegion(dst->resource.Get(),dst->memoryOffset+regions[i].dstOffset,temporary.Get(),0,regions[i].size);
			c->transientResources.push_back(std::move(temporary));
			nativeState=D3D12_RESOURCE_STATE_COPY_DEST;
		}
		src->state=nativeState;dst->state=nativeState;
		return;
	}
	TransitionBuffer(c,src,D3D12_RESOURCE_STATE_COPY_SOURCE);TransitionBuffer(c,dst,D3D12_RESOURCE_STATE_COPY_DEST);
	for(uint32_t i=0;i<count;i++)c->list->CopyBufferRegion(dst->resource.Get(),dst->memoryOffset+regions[i].dstOffset,src->resource.Get(),src->memoryOffset+regions[i].srcOffset,regions[i].size);
}
VKAPI_ATTR void VKAPI_CALL ICmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,VkIndexType type)
{if(!c||!b||!b->resource)return;TransitionBuffer(c,b,D3D12_RESOURCE_STATE_INDEX_BUFFER);D3D12_INDEX_BUFFER_VIEW v{};v.BufferLocation=b->resource->GetGPUVirtualAddress()+b->memoryOffset+offset;v.SizeInBytes=static_cast<UINT>(b->size-offset);v.Format=type==VK_INDEX_TYPE_UINT16?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT;c->list->IASetIndexBuffer(&v);}
VKAPI_ATTR void VKAPI_CALL ICmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets)
{if(!c)return;std::vector<D3D12_VERTEX_BUFFER_VIEW> views(count);for(uint32_t i=0;i<count;i++)if(buffers[i]&&buffers[i]->resource){TransitionBuffer(c,buffers[i],D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);views[i].BufferLocation=buffers[i]->resource->GetGPUVirtualAddress()+buffers[i]->memoryOffset+offsets[i];views[i].SizeInBytes=static_cast<UINT>(buffers[i]->size-offsets[i]);}c->list->IASetVertexBuffers(first,count,views.data());}
VKAPI_ATTR void VKAPI_CALL ICmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t count,const VkViewport* v)
{if(!c||first!=0)return;std::vector<D3D12_VIEWPORT> out(count);for(uint32_t i=0;i<count;i++){const float height=std::abs(v[i].height);const float y=v[i].height<0?v[i].y+v[i].height:v[i].y;out[i]={v[i].x,y,v[i].width,height,v[i].minDepth,v[i].maxDepth};}c->list->RSSetViewports(count,out.data());}
VKAPI_ATTR void VKAPI_CALL ICmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t count,const VkRect2D* r)
{if(!c||first!=0)return;std::vector<D3D12_RECT> out(count);for(uint32_t i=0;i<count;i++)out[i]={r[i].offset.x,r[i].offset.y,r[i].offset.x+static_cast<LONG>(r[i].extent.width),r[i].offset.y+static_cast<LONG>(r[i].extent.height)};c->list->RSSetScissorRects(count,out.data());}
VKAPI_ATTR void VKAPI_CALL ICmdSetBlendConstants(VkCommandBuffer c,const float values[4]){if(c&&values)c->list->OMSetBlendFactor(values);}
VKAPI_ATTR void VKAPI_CALL ICmdSetDepthBias(VkCommandBuffer c,float constant,float clamp,float slope){if(!c||!c->activePipeline||c->activePipeline->compute)return;auto d=c->activePipeline->graphicsDesc;d.RasterizerState.DepthBias=static_cast<INT>(constant);d.RasterizerState.DepthBiasClamp=clamp;d.RasterizerState.SlopeScaledDepthBias=slope;ComPtr<ID3D12PipelineState> variant;if(SUCCEEDED(c->device->native->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&variant)))){c->list->SetPipelineState(variant.Get());c->transientStates.push_back(std::move(variant));}}
VKAPI_ATTR void VKAPI_CALL ICmdDraw(VkCommandBuffer c,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance){if(c){TraceOperation(c,"Draw");c->list->DrawInstanced(vertices,instances,firstVertex,firstInstance);}}
VKAPI_ATTR void VKAPI_CALL ICmdDrawIndexed(VkCommandBuffer c,uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance){if(c){TraceOperation(c,"DrawIndexed");c->list->DrawIndexedInstanced(indices,instances,firstIndex,vertexOffset,firstInstance);}}
VKAPI_ATTR VkResult VKAPI_CALL ICreateShaderModule(VkDevice,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks*,VkShaderModule* out){if(!info||!out||info->codeSize%4)return VK_ERROR_INITIALIZATION_FAILED;auto m=new VkShaderModule_T();m->spirv.assign(info->pCode,info->pCode+info->codeSize/4);*out=m;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyShaderModule(VkDevice,VkShaderModule m,const VkAllocationCallbacks*){delete m;}
bool AllocateViewDescriptorSlot(VkDevice device,bool depth,uint32_t& slot)
{
	std::scoped_lock lock(device->viewDescriptorMutex);
	auto& freeSlots=depth?device->freeDsvSlots:device->freeRtvSlots;
	if(!freeSlots.empty()){slot=freeSlots.back();freeSlots.pop_back();return true;}
	auto& cursor=depth?device->dsvCursor:device->rtvCursor;
	const uint32_t limit=depth?2048u:4096u;
	slot=cursor.fetch_add(1);
	return slot<limit;
}
void ReleaseViewDescriptorSlot(VkDevice device,bool depth,uint32_t& slot)
{
	if(!device||slot==UINT32_MAX)return;
	std::scoped_lock lock(device->viewDescriptorMutex);
	(depth?device->freeDsvSlots:device->freeRtvSlots).push_back(slot);
	slot=UINT32_MAX;
}
VKAPI_ATTR VkResult VKAPI_CALL ICreateImageView(VkDevice device,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* out)
{
	if(!device||!info||!out||!info->image||!info->image->resource)return VK_ERROR_INITIALIZATION_FAILED;
	auto v=new VkImageView_T();v->device=device;v->image=info->image;v->info=*info;
	const DXGI_FORMAT format=ToDxgiFormat(info->format);
	if(info->image->info.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
	{
		uint32_t slot;if(!AllocateViewDescriptorSlot(device,false,slot)){delete v;cemuLog_log(LogType::Force,"D3D12 RTV descriptor heap exhausted while creating image view");return VK_ERROR_OUT_OF_HOST_MEMORY;}v->rtvSlot=slot;
		v->rtv=device->rtvHeap->GetCPUDescriptorHandleForHeapStart();v->rtv.ptr+=SIZE_T(slot)*device->rtvStride;
		D3D12_RENDER_TARGET_VIEW_DESC d{};d.Format=format;if(info->viewType==VK_IMAGE_VIEW_TYPE_1D){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE1D;d.Texture1D.MipSlice=info->subresourceRange.baseMipLevel;}else if(info->viewType==VK_IMAGE_VIEW_TYPE_1D_ARRAY){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE1DARRAY;d.Texture1DArray.MipSlice=info->subresourceRange.baseMipLevel;d.Texture1DArray.FirstArraySlice=info->subresourceRange.baseArrayLayer;d.Texture1DArray.ArraySize=info->subresourceRange.layerCount;}else if(info->viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY||info->viewType==VK_IMAGE_VIEW_TYPE_CUBE||info->viewType==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipSlice=info->subresourceRange.baseMipLevel;d.Texture2DArray.FirstArraySlice=info->subresourceRange.baseArrayLayer;d.Texture2DArray.ArraySize=info->subresourceRange.layerCount;}else if(info->viewType==VK_IMAGE_VIEW_TYPE_3D){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE3D;d.Texture3D.MipSlice=info->subresourceRange.baseMipLevel;d.Texture3D.FirstWSlice=info->subresourceRange.baseArrayLayer;d.Texture3D.WSize=info->subresourceRange.layerCount;}else{d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=info->subresourceRange.baseMipLevel;}
		device->native->CreateRenderTargetView(info->image->resource.Get(),&d,v->rtv);
	}
	if(info->image->info.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
	{
		const DXGI_FORMAT depthFormat=ToDxgiFormat(info->image->info.format);
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{depthFormat};
		if(depthFormat==DXGI_FORMAT_UNKNOWN||FAILED(device->native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||!(support.Support1&D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)){ReleaseViewDescriptorSlot(device,false,v->rtvSlot);delete v;return VK_ERROR_FORMAT_NOT_SUPPORTED;}
		uint32_t slot;if(!AllocateViewDescriptorSlot(device,true,slot)){ReleaseViewDescriptorSlot(device,false,v->rtvSlot);delete v;cemuLog_log(LogType::Force,"D3D12 DSV descriptor heap exhausted while creating image view");return VK_ERROR_OUT_OF_HOST_MEMORY;}v->dsvSlot=slot;
		v->dsv=device->dsvHeap->GetCPUDescriptorHandleForHeapStart();v->dsv.ptr+=SIZE_T(slot)*device->dsvStride;
		D3D12_DEPTH_STENCIL_VIEW_DESC d{};d.Format=depthFormat;if(info->viewType==VK_IMAGE_VIEW_TYPE_1D){d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE1D;d.Texture1D.MipSlice=info->subresourceRange.baseMipLevel;}else if(info->viewType==VK_IMAGE_VIEW_TYPE_1D_ARRAY){d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE1DARRAY;d.Texture1DArray.MipSlice=info->subresourceRange.baseMipLevel;d.Texture1DArray.FirstArraySlice=info->subresourceRange.baseArrayLayer;d.Texture1DArray.ArraySize=info->subresourceRange.layerCount;}else if(info->viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY||info->viewType==VK_IMAGE_VIEW_TYPE_CUBE||info->viewType==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY){d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipSlice=info->subresourceRange.baseMipLevel;d.Texture2DArray.FirstArraySlice=info->subresourceRange.baseArrayLayer;d.Texture2DArray.ArraySize=info->subresourceRange.layerCount;}else{d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=info->subresourceRange.baseMipLevel;}
		device->native->CreateDepthStencilView(info->image->resource.Get(),&d,v->dsv);
	}
	*out=v;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IDestroyImageView(VkDevice,VkImageView v,const VkAllocationCallbacks*){if(!v)return;ReleaseViewDescriptorSlot(v->device,false,v->rtvSlot);ReleaseViewDescriptorSlot(v->device,true,v->dsvSlot);delete v;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateSampler(VkDevice,const VkSamplerCreateInfo* info,const VkAllocationCallbacks*,VkSampler* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;auto s=new VkSampler_T();s->info=*info;*out=s;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroySampler(VkDevice,VkSampler s,const VkAllocationCallbacks*){delete s;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorSetLayout* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;auto l=new VkDescriptorSetLayout_T();l->bindings.assign(info->pBindings,info->pBindings+info->bindingCount);*out=l;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyDescriptorSetLayout(VkDevice,VkDescriptorSetLayout l,const VkAllocationCallbacks*){delete l;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateDescriptorPool(VkDevice,const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*,VkDescriptorPool* out){if(!out)return VK_ERROR_INITIALIZATION_FAILED;*out=new VkDescriptorPool_T();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyDescriptorPool(VkDevice,VkDescriptorPool p,const VkAllocationCallbacks*){if(p){for(auto s:p->sets)delete s;delete p;}}
VKAPI_ATTR VkResult VKAPI_CALL IAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out)
{
	if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;
	for(uint32_t i=0;i<info->descriptorSetCount;i++)
	{
		auto s=new VkDescriptorSet_T();s->device=device;s->layout=info->pSetLayouts[i];
		for(const auto& b:s->layout->bindings)
		{
			const uint32_t cls=DescriptorClass(b.descriptorType);if(cls<4)s->count[cls]=(std::max)(s->count[cls],b.binding+b.descriptorCount);
			if(b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)s->count[3]=(std::max)(s->count[3],b.binding+b.descriptorCount);
		}
		for(uint32_t cls=0;cls<4;cls++)
		{
			auto& cursor=cls==3?device->samplerCursor:device->resourceCursor;
			s->base[cls]=cursor.fetch_add(s->count[cls]);
			if((cls==3&&s->base[cls]+s->count[cls]>2048)||(cls!=3&&s->base[cls]+s->count[cls]>65536)){delete s;return VK_ERROR_OUT_OF_POOL_MEMORY;}
		}
		out[i]=s;info->descriptorPool->sets.push_back(s);
	}
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL IFreeDescriptorSets(VkDevice,VkDescriptorPool p,uint32_t count,const VkDescriptorSet* sets){for(uint32_t i=0;i<count;i++){auto it=std::find(p->sets.begin(),p->sets.end(),sets[i]);if(it!=p->sets.end())p->sets.erase(it);delete sets[i];}return VK_SUCCESS;}

void FillShaderResourceViewDesc(const VkImageViewCreateInfo& info, const VkImageCreateInfo& imageInfo, D3D12_SHADER_RESOURCE_VIEW_DESC& d)
{
	d.Format=ToDxgiShaderResourceFormat(info.format,info.subresourceRange.aspectMask);d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;const UINT levels=info.subresourceRange.levelCount==VK_REMAINING_MIP_LEVELS?imageInfo.mipLevels-info.subresourceRange.baseMipLevel:info.subresourceRange.levelCount;
	if(info.viewType==VK_IMAGE_VIEW_TYPE_1D){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE1D;d.Texture1D.MostDetailedMip=info.subresourceRange.baseMipLevel;d.Texture1D.MipLevels=levels;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_1D_ARRAY){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE1DARRAY;d.Texture1DArray.MostDetailedMip=info.subresourceRange.baseMipLevel;d.Texture1DArray.MipLevels=levels;d.Texture1DArray.FirstArraySlice=info.subresourceRange.baseArrayLayer;d.Texture1DArray.ArraySize=info.subresourceRange.layerCount;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_CUBE){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;d.TextureCube.MostDetailedMip=info.subresourceRange.baseMipLevel;d.TextureCube.MipLevels=levels;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;d.TextureCubeArray.MostDetailedMip=info.subresourceRange.baseMipLevel;d.TextureCubeArray.MipLevels=levels;d.TextureCubeArray.First2DArrayFace=info.subresourceRange.baseArrayLayer;d.TextureCubeArray.NumCubes=info.subresourceRange.layerCount/6;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MostDetailedMip=info.subresourceRange.baseMipLevel;d.Texture2DArray.MipLevels=levels;d.Texture2DArray.FirstArraySlice=info.subresourceRange.baseArrayLayer;d.Texture2DArray.ArraySize=info.subresourceRange.layerCount;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_3D){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;d.Texture3D.MostDetailedMip=info.subresourceRange.baseMipLevel;d.Texture3D.MipLevels=levels;}
	else{d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MostDetailedMip=info.subresourceRange.baseMipLevel;d.Texture2D.MipLevels=levels;}
}

void FillUnorderedAccessViewDesc(const VkImageViewCreateInfo& info, D3D12_UNORDERED_ACCESS_VIEW_DESC& d)
{
	d.Format=ToDxgiFormat(info.format);
	if(info.viewType==VK_IMAGE_VIEW_TYPE_1D){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE1D;d.Texture1D.MipSlice=info.subresourceRange.baseMipLevel;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_1D_ARRAY){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE1DARRAY;d.Texture1DArray.MipSlice=info.subresourceRange.baseMipLevel;d.Texture1DArray.FirstArraySlice=info.subresourceRange.baseArrayLayer;d.Texture1DArray.ArraySize=info.subresourceRange.layerCount;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY||info.viewType==VK_IMAGE_VIEW_TYPE_CUBE||info.viewType==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipSlice=info.subresourceRange.baseMipLevel;d.Texture2DArray.FirstArraySlice=info.subresourceRange.baseArrayLayer;d.Texture2DArray.ArraySize=info.subresourceRange.layerCount;}
	else if(info.viewType==VK_IMAGE_VIEW_TYPE_3D){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;d.Texture3D.MipSlice=info.subresourceRange.baseMipLevel;d.Texture3D.FirstWSlice=info.subresourceRange.baseArrayLayer;d.Texture3D.WSize=info.subresourceRange.layerCount;}
	else{d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=info.subresourceRange.baseMipLevel;}
}

VKAPI_ATTR void VKAPI_CALL IUpdateDescriptorSets(VkDevice device,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copyCount,const VkCopyDescriptorSet* copies)
{
	if(!device)return;if(count||copyCount)IDeviceWaitIdle(device);
	auto cpuHandle=[&](VkDescriptorSet set,uint32_t cls,uint32_t binding,uint32_t element){D3D12_CPU_DESCRIPTOR_HANDLE h=(cls==3?device->samplerHeap:device->resourceHeap)->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(set->base[cls]+binding+element)*(cls==3?device->samplerStride:device->resourceStride);return h;};
	for(uint32_t i=0;i<count;i++)
	{
		const auto& w=writes[i];if(!w.dstSet)continue;
		for(uint32_t e=0;e<w.descriptorCount;e++)
		{
			DescriptorValue value{};value.type=w.descriptorType;if(w.pBufferInfo)value.buffer=w.pBufferInfo[e];if(w.pImageInfo)value.image=w.pImageInfo[e];
			w.dstSet->values[(uint64_t(w.dstBinding)<<32)|(w.dstArrayElement+e)]=value;
			const uint32_t cls=DescriptorClass(w.descriptorType);if(cls==UINT32_MAX)continue;
			if(cls==0&&value.buffer.buffer&&value.buffer.buffer->resource)
			{
					if(value.buffer.offset>=value.buffer.buffer->size)continue;const VkDeviceSize available=value.buffer.buffer->size-value.buffer.offset;const VkDeviceSize requested=value.buffer.range==VK_WHOLE_SIZE?available:(std::min)(value.buffer.range,available);const VkDeviceSize cbvSize=(std::min)(VkDeviceSize(65536),(requested+255)&~VkDeviceSize(255));if(!cbvSize)continue;D3D12_CONSTANT_BUFFER_VIEW_DESC d{};d.BufferLocation=value.buffer.buffer->resource->GetGPUVirtualAddress()+value.buffer.buffer->memoryOffset+value.buffer.offset;d.SizeInBytes=static_cast<UINT>(cbvSize);device->native->CreateConstantBufferView(&d,cpuHandle(w.dstSet,cls,w.dstBinding,w.dstArrayElement+e));
			}
			else if((cls==1||cls==2)&&value.image.imageView&&value.image.imageView->image->resource)
			{
				auto view=value.image.imageView;
				if(cls==1){D3D12_SHADER_RESOURCE_VIEW_DESC d{};FillShaderResourceViewDesc(view->info,view->image->info,d);device->native->CreateShaderResourceView(view->image->resource.Get(),&d,cpuHandle(w.dstSet,cls,w.dstBinding,w.dstArrayElement+e));}
				else{D3D12_UNORDERED_ACCESS_VIEW_DESC d{};FillUnorderedAccessViewDesc(view->info,d);device->native->CreateUnorderedAccessView(view->image->resource.Get(),nullptr,&d,cpuHandle(w.dstSet,cls,w.dstBinding,w.dstArrayElement+e));}
			}
			else if(cls==2&&value.buffer.buffer&&value.buffer.buffer->resource)
			{
					D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.FirstElement=(value.buffer.buffer->memoryOffset+value.buffer.offset)/4;d.Buffer.NumElements=static_cast<UINT>((value.buffer.range==VK_WHOLE_SIZE?value.buffer.buffer->size-value.buffer.offset:value.buffer.range)/4);d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->native->CreateUnorderedAccessView(value.buffer.buffer->resource.Get(),nullptr,&d,cpuHandle(w.dstSet,cls,w.dstBinding,w.dstArrayElement+e));
			}
			if((w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER||w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)&&value.image.sampler)
			{
const auto& si=value.image.sampler->info;D3D12_SAMPLER_DESC d{};d.Filter=si.anisotropyEnable?D3D12_FILTER_ANISOTROPIC:(si.magFilter==VK_FILTER_LINEAR?D3D12_FILTER_MIN_MAG_MIP_LINEAR:D3D12_FILTER_MIN_MAG_MIP_POINT);auto address=[](VkSamplerAddressMode m){return m==VK_SAMPLER_ADDRESS_MODE_REPEAT?D3D12_TEXTURE_ADDRESS_MODE_WRAP:(m==VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT?D3D12_TEXTURE_ADDRESS_MODE_MIRROR:(m==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER?D3D12_TEXTURE_ADDRESS_MODE_BORDER:D3D12_TEXTURE_ADDRESS_MODE_CLAMP));};d.AddressU=address(si.addressModeU);d.AddressV=address(si.addressModeV);d.AddressW=address(si.addressModeW);d.MipLODBias=si.mipLodBias;d.MaxAnisotropy=si.anisotropyEnable?static_cast<UINT>((std::max)(1.0f,si.maxAnisotropy)):1;d.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;d.MinLOD=si.minLod;d.MaxLOD=si.maxLod;device->native->CreateSampler(&d,cpuHandle(w.dstSet,3,w.dstBinding,w.dstArrayElement+e));
			}
		}
	}
	for(uint32_t i=0;i<copyCount;i++)for(uint32_t e=0;e<copies[i].descriptorCount;e++)
	{
		const uint64_t sk=(uint64_t(copies[i].srcBinding)<<32)|(copies[i].srcArrayElement+e),dk=(uint64_t(copies[i].dstBinding)<<32)|(copies[i].dstArrayElement+e);auto it=copies[i].srcSet->values.find(sk);if(it==copies[i].srcSet->values.end())continue;copies[i].dstSet->values[dk]=it->second;const uint32_t cls=DescriptorClass(it->second.type);if(cls<4)device->native->CopyDescriptorsSimple(1,cpuHandle(copies[i].dstSet,cls,copies[i].dstBinding,copies[i].dstArrayElement+e),cpuHandle(copies[i].srcSet,cls,copies[i].srcBinding,copies[i].srcArrayElement+e),cls==3?D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);if(it->second.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)device->native->CopyDescriptorsSimple(1,cpuHandle(copies[i].dstSet,3,copies[i].dstBinding,copies[i].dstArrayElement+e),cpuHandle(copies[i].srcSet,3,copies[i].srcBinding,copies[i].srcArrayElement+e),D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	}
}
VKAPI_ATTR VkResult VKAPI_CALL ICreatePipelineLayout(VkDevice device,const VkPipelineLayoutCreateInfo* info,const VkAllocationCallbacks*,VkPipelineLayout* out)
{
	if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;const uint32_t tableCount=info->setLayoutCount*4;const uint32_t pushDwords=info->pushConstantRangeCount?(info->pPushConstantRanges[0].size+3)/4:0;const uint32_t paramCount=tableCount+(pushDwords?1:0);if(tableCount+pushDwords>64)return VK_ERROR_INITIALIZATION_FAILED;
	std::vector<D3D12_ROOT_PARAMETER> params(paramCount);std::vector<D3D12_DESCRIPTOR_RANGE> ranges(tableCount);const D3D12_DESCRIPTOR_RANGE_TYPE types[4]={D3D12_DESCRIPTOR_RANGE_TYPE_CBV,D3D12_DESCRIPTOR_RANGE_TYPE_SRV,D3D12_DESCRIPTOR_RANGE_TYPE_UAV,D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER};const uint32_t tableBase=pushDwords?1u:0u;
	if(pushDwords){params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,255,pushDwords};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
	for(uint32_t set=0;set<info->setLayoutCount;set++)for(uint32_t cls=0;cls<4;cls++){const uint32_t rangeIndex=set*4+cls;const uint32_t p=tableBase+rangeIndex;ranges[rangeIndex]={types[cls],cls==3?256u:1024u,0,set,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};params[p].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[p].DescriptorTable={1,&ranges[rangeIndex]};params[p].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
	D3D12_ROOT_SIGNATURE_DESC d{};d.NumParameters=paramCount;d.pParameters=params.data();d.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;ComPtr<ID3DBlob> blob,error;if(FAILED(D3D12SerializeRootSignature(&d,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return VK_ERROR_INITIALIZATION_FAILED;auto l=new VkPipelineLayout_T();l->setCount=info->setLayoutCount;l->pushDwords=pushDwords;l->pushRootIndex=pushDwords?0u:UINT32_MAX;if(FAILED(device->native->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&l->root)))){delete l;return VK_ERROR_INITIALIZATION_FAILED;}*out=l;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IDestroyPipelineLayout(VkDevice,VkPipelineLayout l,const VkAllocationCallbacks*){delete l;}
VKAPI_ATTR void VKAPI_CALL ICmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint,VkPipeline p){if(c&&p){TraceOperation(c,p->compute?"BindComputePipeline":"BindGraphicsPipeline");c->activePipeline=p;c->list->SetPipelineState(p->state.Get());if(p->compute)c->list->SetComputeRootSignature(p->layout->root.Get());else{c->list->SetGraphicsRootSignature(p->layout->root.Get());c->list->IASetPrimitiveTopology(p->topology);}}}
VKAPI_ATTR void VKAPI_CALL ICmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamicOffsets)
{
		TraceOperation(c,"BindDescriptorSets");
		if(!c||!layout)return;ID3D12DescriptorHeap* heaps[]={c->device->resourceHeap.Get(),c->device->samplerHeap.Get()};c->list->SetDescriptorHeaps(2,heaps);
		for(uint32_t i=0;i<count;i++)if(sets[i])for(const auto& entry:sets[i]->values){const auto& value=entry.second;if(value.buffer.buffer){if(value.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||value.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)TransitionBuffer(c,value.buffer.buffer,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);else if(value.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||value.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)TransitionBuffer(c,value.buffer.buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}}
		uint32_t dynamicIndex=0;for(uint32_t i=0;i<count;i++)if(sets[i]){auto bindings=sets[i]->layout->bindings;std::sort(bindings.begin(),bindings.end(),[](const auto& a,const auto& b){return a.binding<b.binding;});for(const auto& b:bindings)if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)for(uint32_t e=0;e<b.descriptorCount&&dynamicIndex<dynamicCount;e++,dynamicIndex++){auto it=sets[i]->values.find((uint64_t(b.binding)<<32)|e);if(it==sets[i]->values.end()||!it->second.buffer.buffer||!it->second.buffer.buffer->resource)continue;const auto& value=it->second.buffer;const VkDeviceSize dynamicOffset=dynamicOffsets[dynamicIndex];if(value.offset>=value.buffer->size||dynamicOffset>=value.buffer->size-value.offset)continue;const VkDeviceSize totalOffset=value.offset+dynamicOffset;const VkDeviceSize available=value.buffer->size-totalOffset;const VkDeviceSize requested=value.range==VK_WHOLE_SIZE?available:(std::min)(value.range,available);const VkDeviceSize cbvSize=(std::min)(VkDeviceSize(65536),(requested+255)&~VkDeviceSize(255));if(!cbvSize)continue;D3D12_CONSTANT_BUFFER_VIEW_DESC d{};d.BufferLocation=value.buffer->resource->GetGPUVirtualAddress()+value.buffer->memoryOffset+totalOffset;d.SizeInBytes=static_cast<UINT>(cbvSize);auto h=c->device->resourceHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(sets[i]->base[0]+b.binding+e)*c->device->resourceStride;c->device->native->CreateConstantBufferView(&d,h);}}
	for(uint32_t i=0;i<count&&first+i<layout->setCount;i++)for(uint32_t cls=0;cls<4;cls++)if(sets[i]&&sets[i]->count[cls]){D3D12_GPU_DESCRIPTOR_HANDLE h=(cls==3?c->device->samplerHeap:c->device->resourceHeap)->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(sets[i]->base[cls])*(cls==3?c->device->samplerStride:c->device->resourceStride);const uint32_t rootIndex=(layout->pushDwords?1u:0u)+(first+i)*4+cls;if(point==VK_PIPELINE_BIND_POINT_COMPUTE)c->list->SetComputeRootDescriptorTable(rootIndex,h);else c->list->SetGraphicsRootDescriptorTable(rootIndex,h);}
}
VKAPI_ATTR void VKAPI_CALL ICmdPushConstants(VkCommandBuffer c,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data){if(!c||!layout||!c->activePipeline||!c->activePipeline->layout||!data)return;const auto nativeLayout=c->activePipeline->layout;if(nativeLayout->pushRootIndex==UINT32_MAX||offset/4+(size+3)/4>nativeLayout->pushDwords)return;const uint32_t count=(size+3)/4;if(c->activePipeline->compute&&(stages&VK_SHADER_STAGE_COMPUTE_BIT))c->list->SetComputeRoot32BitConstants(nativeLayout->pushRootIndex,count,data,offset/4);else if(!c->activePipeline->compute)c->list->SetGraphicsRoot32BitConstants(nativeLayout->pushRootIndex,count,data,offset/4);}
VKAPI_ATTR VkResult VKAPI_CALL ICreatePipelineCache(VkDevice,const VkPipelineCacheCreateInfo* info,const VkAllocationCallbacks*,VkPipelineCache* out){if(!out)return VK_ERROR_INITIALIZATION_FAILED;auto p=new VkPipelineCache_T();if(info&&info->pInitialData)p->data.assign(static_cast<const uint8_t*>(info->pInitialData),static_cast<const uint8_t*>(info->pInitialData)+info->initialDataSize);*out=p;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyPipelineCache(VkDevice,VkPipelineCache p,const VkAllocationCallbacks*){delete p;}
VKAPI_ATTR VkResult VKAPI_CALL IGetPipelineCacheData(VkDevice,VkPipelineCache p,size_t* size,void* data){if(!p||!size)return VK_ERROR_INITIALIZATION_FAILED;if(!data){*size=p->data.size();return VK_SUCCESS;}const size_t n=(std::min)(*size,p->data.size());std::memcpy(data,p->data.data(),n);*size=n;return n==p->data.size()?VK_SUCCESS:VK_INCOMPLETE;}
VKAPI_ATTR VkResult VKAPI_CALL IMergePipelineCaches(VkDevice,VkPipelineCache,uint32_t,const VkPipelineCache*){return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyPipeline(VkDevice,VkPipeline p,const VkAllocationCallbacks*){delete p;}
ComPtr<ID3DBlob> CompileStage(VkShaderModule module,VkShaderStageFlagBits stage,const char* entry)
{spirv_cross::CompilerHLSL compiler(module->spirv);for(const auto& resource:compiler.get_shader_resources().push_constant_buffers){compiler.set_decoration(resource.id,spv::DecorationBinding,0);compiler.set_decoration(resource.id,spv::DecorationDescriptorSet,255);}spirv_cross::CompilerHLSL::Options options;options.shader_model=51;compiler.set_hlsl_options(options);const std::string source=compiler.compile();const char* target=stage==VK_SHADER_STAGE_VERTEX_BIT?"vs_5_1":(stage==VK_SHADER_STAGE_GEOMETRY_BIT?"gs_5_1":(stage==VK_SHADER_STAGE_COMPUTE_BIT?"cs_5_1":"ps_5_1"));ComPtr<ID3DBlob> code,error;if(FAILED(D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,"main",target,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL1,0,&code,&error))){cemuLog_log(LogType::Force,"D3D12 shader compilation failed for entry {}: {}",entry?entry:"main",error?static_cast<const char*>(error->GetBufferPointer()):"unknown error");return {};}return code;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateGraphicsPipelines(VkDevice device,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks*,VkPipeline* out)
{
	auto compare=[](VkCompareOp op){return static_cast<D3D12_COMPARISON_FUNC>(op+1);};
	auto stencilOp=[](VkStencilOp op){switch(op){case VK_STENCIL_OP_KEEP:return D3D12_STENCIL_OP_KEEP;case VK_STENCIL_OP_ZERO:return D3D12_STENCIL_OP_ZERO;case VK_STENCIL_OP_REPLACE:return D3D12_STENCIL_OP_REPLACE;case VK_STENCIL_OP_INCREMENT_AND_CLAMP:return D3D12_STENCIL_OP_INCR_SAT;case VK_STENCIL_OP_DECREMENT_AND_CLAMP:return D3D12_STENCIL_OP_DECR_SAT;case VK_STENCIL_OP_INVERT:return D3D12_STENCIL_OP_INVERT;case VK_STENCIL_OP_INCREMENT_AND_WRAP:return D3D12_STENCIL_OP_INCR;case VK_STENCIL_OP_DECREMENT_AND_WRAP:return D3D12_STENCIL_OP_DECR;default:return D3D12_STENCIL_OP_KEEP;}};
	auto blend=[](VkBlendFactor f){switch(f){case VK_BLEND_FACTOR_ZERO:return D3D12_BLEND_ZERO;case VK_BLEND_FACTOR_ONE:return D3D12_BLEND_ONE;case VK_BLEND_FACTOR_SRC_COLOR:return D3D12_BLEND_SRC_COLOR;case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:return D3D12_BLEND_INV_SRC_COLOR;case VK_BLEND_FACTOR_DST_COLOR:return D3D12_BLEND_DEST_COLOR;case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:return D3D12_BLEND_INV_DEST_COLOR;case VK_BLEND_FACTOR_SRC_ALPHA:return D3D12_BLEND_SRC_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:return D3D12_BLEND_INV_SRC_ALPHA;case VK_BLEND_FACTOR_DST_ALPHA:return D3D12_BLEND_DEST_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:return D3D12_BLEND_INV_DEST_ALPHA;case VK_BLEND_FACTOR_CONSTANT_COLOR:case VK_BLEND_FACTOR_CONSTANT_ALPHA:return D3D12_BLEND_BLEND_FACTOR;case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:return D3D12_BLEND_INV_BLEND_FACTOR;case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:return D3D12_BLEND_SRC_ALPHA_SAT;case VK_BLEND_FACTOR_SRC1_COLOR:return D3D12_BLEND_SRC1_COLOR;case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:return D3D12_BLEND_INV_SRC1_COLOR;case VK_BLEND_FACTOR_SRC1_ALPHA:return D3D12_BLEND_SRC1_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:return D3D12_BLEND_INV_SRC1_ALPHA;default:return D3D12_BLEND_ONE;}};
	auto blendAlpha=[&](VkBlendFactor f){switch(f){case VK_BLEND_FACTOR_SRC_COLOR:return D3D12_BLEND_SRC_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:return D3D12_BLEND_INV_SRC_ALPHA;case VK_BLEND_FACTOR_DST_COLOR:return D3D12_BLEND_DEST_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:return D3D12_BLEND_INV_DEST_ALPHA;case VK_BLEND_FACTOR_SRC1_COLOR:return D3D12_BLEND_SRC1_ALPHA;case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:return D3D12_BLEND_INV_SRC1_ALPHA;default:return blend(f);}};
	for(uint32_t n=0;n<count;n++)
	{
		const auto& i=infos[n];ComPtr<ID3DBlob> vs,ps,gs;for(uint32_t s=0;s<i.stageCount;s++){auto code=CompileStage(i.pStages[s].module,static_cast<VkShaderStageFlagBits>(i.pStages[s].stage),i.pStages[s].pName);if(!code)return VK_ERROR_INVALID_SHADER_NV;if(i.pStages[s].stage==VK_SHADER_STAGE_VERTEX_BIT)vs=code;else if(i.pStages[s].stage==VK_SHADER_STAGE_FRAGMENT_BIT)ps=code;else if(i.pStages[s].stage==VK_SHADER_STAGE_GEOMETRY_BIT)gs=code;}if(!vs)return VK_ERROR_INVALID_SHADER_NV;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};d.pRootSignature=i.layout->root.Get();d.VS={vs->GetBufferPointer(),vs->GetBufferSize()};if(ps)d.PS={ps->GetBufferPointer(),ps->GetBufferSize()};if(gs)d.GS={gs->GetBufferPointer(),gs->GetBufferSize()};d.SampleMask=i.pMultisampleState?i.pMultisampleState->rasterizationSamples?UINT_MAX:UINT_MAX:UINT_MAX;
		if(i.pRasterizationState){const auto& r=*i.pRasterizationState;d.RasterizerState.FillMode=r.polygonMode==VK_POLYGON_MODE_LINE?D3D12_FILL_MODE_WIREFRAME:D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=r.cullMode==VK_CULL_MODE_FRONT_BIT?D3D12_CULL_MODE_FRONT:(r.cullMode==VK_CULL_MODE_BACK_BIT?D3D12_CULL_MODE_BACK:D3D12_CULL_MODE_NONE);d.RasterizerState.FrontCounterClockwise=r.frontFace==VK_FRONT_FACE_COUNTER_CLOCKWISE;d.RasterizerState.DepthBias=static_cast<INT>(r.depthBiasConstantFactor);d.RasterizerState.SlopeScaledDepthBias=r.depthBiasSlopeFactor;d.RasterizerState.DepthBiasClamp=r.depthBiasClamp;d.RasterizerState.DepthClipEnable=!r.depthClampEnable;}else{d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;d.RasterizerState.DepthClipEnable=TRUE;}
		if(i.pMultisampleState)d.BlendState.AlphaToCoverageEnable=i.pMultisampleState->alphaToCoverageEnable;
		if(i.pColorBlendState)for(uint32_t a=0;a<i.pColorBlendState->attachmentCount&&a<8;a++){const auto& v=i.pColorBlendState->pAttachments[a];auto& rt=d.BlendState.RenderTarget[a];rt.BlendEnable=v.blendEnable;rt.SrcBlend=blend(v.srcColorBlendFactor);rt.DestBlend=blend(v.dstColorBlendFactor);rt.BlendOp=static_cast<D3D12_BLEND_OP>(v.colorBlendOp+1);rt.SrcBlendAlpha=blendAlpha(v.srcAlphaBlendFactor);rt.DestBlendAlpha=blendAlpha(v.dstAlphaBlendFactor);rt.BlendOpAlpha=static_cast<D3D12_BLEND_OP>(v.alphaBlendOp+1);rt.RenderTargetWriteMask=static_cast<UINT8>(v.colorWriteMask);}else for(auto& rt:d.BlendState.RenderTarget)rt.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
		if(i.pDepthStencilState){const auto& z=*i.pDepthStencilState;d.DepthStencilState.DepthEnable=z.depthTestEnable;d.DepthStencilState.DepthWriteMask=z.depthWriteEnable?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;d.DepthStencilState.DepthFunc=compare(z.depthCompareOp);d.DepthStencilState.StencilEnable=z.stencilTestEnable;d.DepthStencilState.StencilReadMask=static_cast<UINT8>(z.front.compareMask);d.DepthStencilState.StencilWriteMask=static_cast<UINT8>(z.front.writeMask);auto face=[&](const VkStencilOpState& s){D3D12_DEPTH_STENCILOP_DESC o{};o.StencilFailOp=stencilOp(s.failOp);o.StencilDepthFailOp=stencilOp(s.depthFailOp);o.StencilPassOp=stencilOp(s.passOp);o.StencilFunc=compare(s.compareOp);return o;};d.DepthStencilState.FrontFace=face(z.front);d.DepthStencilState.BackFace=face(z.back);}else{d.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;d.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};d.DepthStencilState.BackFace=d.DepthStencilState.FrontFace;}
		auto p=new VkPipeline_T();p->layout=i.layout;const auto topology=i.pInputAssemblyState?i.pInputAssemblyState->topology:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;if(topology==VK_PRIMITIVE_TOPOLOGY_POINT_LIST){d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;p->topology=D3D_PRIMITIVE_TOPOLOGY_POINTLIST;}else if(topology==VK_PRIMITIVE_TOPOLOGY_LINE_LIST||topology==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP){d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;p->topology=topology==VK_PRIMITIVE_TOPOLOGY_LINE_LIST?D3D_PRIMITIVE_TOPOLOGY_LINELIST:D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;}else{d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p->topology=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP?D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;}
		if(i.renderPass){const uint32_t colorCount=(std::min)(static_cast<uint32_t>(i.renderPass->colors.size()),8u);for(uint32_t a=0;a<colorCount;a++){const auto attachment=i.renderPass->colors[a].attachment;if(attachment==VK_ATTACHMENT_UNUSED)continue;if(attachment>=i.renderPass->attachments.size()){delete p;return VK_ERROR_INITIALIZATION_FAILED;}const VkFormat vkFormat=i.renderPass->attachments[attachment].format;const DXGI_FORMAT rtvFormat=ToDxgiFormat(vkFormat);if(rtvFormat==DXGI_FORMAT_UNKNOWN){cemuLog_log(LogType::Force,fmt::format("D3D12 graphics pipeline rejected unsupported color attachment format {} at slot {}",static_cast<uint32_t>(vkFormat),a));delete p;return VK_ERROR_FORMAT_NOT_SUPPORTED;}d.RTVFormats[a]=rtvFormat;d.NumRenderTargets=a+1;}if(i.renderPass->depth.attachment!=VK_ATTACHMENT_UNUSED&&i.renderPass->depth.attachment<i.renderPass->attachments.size())d.DSVFormat=ToDxgiFormat(i.renderPass->attachments[i.renderPass->depth.attachment].format);}
		else
		{
			const VkPipelineRenderingCreateInfoKHR* rendering=nullptr;
			for(auto* next=static_cast<const VkBaseInStructure*>(i.pNext);next;next=next->pNext)if(next->sType==VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR){rendering=reinterpret_cast<const VkPipelineRenderingCreateInfoKHR*>(next);break;}
			if(rendering)
			{
				const uint32_t colorCount=(std::min)(rendering->colorAttachmentCount,8u);
				for(uint32_t a=0;a<colorCount;a++)
				{
					if(rendering->pColorAttachmentFormats[a]==VK_FORMAT_UNDEFINED)continue;
					const DXGI_FORMAT rtvFormat=ToDxgiFormat(rendering->pColorAttachmentFormats[a]);
					if(rtvFormat==DXGI_FORMAT_UNKNOWN){cemuLog_log(LogType::Force,fmt::format("D3D12 dynamic graphics pipeline rejected unsupported color attachment format {} at slot {}",static_cast<uint32_t>(rendering->pColorAttachmentFormats[a]),a));delete p;return VK_ERROR_FORMAT_NOT_SUPPORTED;}
					d.RTVFormats[a]=rtvFormat;d.NumRenderTargets=a+1;
				}
				const VkFormat depthFormat=rendering->depthAttachmentFormat!=VK_FORMAT_UNDEFINED?rendering->depthAttachmentFormat:rendering->stencilAttachmentFormat;
				if(depthFormat!=VK_FORMAT_UNDEFINED)d.DSVFormat=ToDxgiFormat(depthFormat);
			}
		}
		d.SampleDesc.Count=i.pMultisampleState?static_cast<UINT>(i.pMultisampleState->rasterizationSamples):1;
		std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
		if(i.pVertexInputState)
		{
			inputElements.reserve(i.pVertexInputState->vertexAttributeDescriptionCount);
			for(uint32_t a=0;a<i.pVertexInputState->vertexAttributeDescriptionCount;a++)
			{
				const auto& attribute=i.pVertexInputState->pVertexAttributeDescriptions[a];
				D3D12_INPUT_ELEMENT_DESC element{};element.SemanticName="TEXCOORD";element.SemanticIndex=attribute.location;element.Format=ToDxgiFormat(attribute.format);element.InputSlot=attribute.binding;element.AlignedByteOffset=attribute.offset;
				for(uint32_t b=0;b<i.pVertexInputState->vertexBindingDescriptionCount;b++)if(i.pVertexInputState->pVertexBindingDescriptions[b].binding==attribute.binding){element.InputSlotClass=i.pVertexInputState->pVertexBindingDescriptions[b].inputRate==VK_VERTEX_INPUT_RATE_INSTANCE?D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA:D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;element.InstanceDataStepRate=element.InputSlotClass==D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA?1:0;break;}
				if(element.Format!=DXGI_FORMAT_UNKNOWN)inputElements.push_back(element);
			}
			d.InputLayout={inputElements.data(),static_cast<UINT>(inputElements.size())};
		}
		p->vertexShader=vs;p->pixelShader=ps;p->geometryShader=gs;p->inputElements=inputElements;if(!p->inputElements.empty())d.InputLayout={p->inputElements.data(),static_cast<UINT>(p->inputElements.size())};p->graphicsDesc=d;if(FAILED(device->native->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&p->state)))){delete p;return VK_ERROR_INITIALIZATION_FAILED;}out[n]=p;
	}
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL ICreateQueryPool(VkDevice device,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks*,VkQueryPool* out){if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;auto q=new VkQueryPool_T();q->device=device;q->type=info->queryType;q->count=info->queryCount;D3D12_QUERY_HEAP_DESC hd{};hd.Count=info->queryCount;hd.Type=info->queryType==VK_QUERY_TYPE_OCCLUSION?D3D12_QUERY_HEAP_TYPE_OCCLUSION:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;if(FAILED(device->native->CreateQueryHeap(&hd,IID_PPV_ARGS(&q->heap)))){delete q;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=VkDeviceSize(info->queryCount)*8;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;if(FAILED(device->native->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&q->readback)))){delete q;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}*out=q;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyQueryPool(VkDevice,VkQueryPool q,const VkAllocationCallbacks*){delete q;}
VKAPI_ATTR void VKAPI_CALL ICmdResetQueryPool(VkCommandBuffer,VkQueryPool,uint32_t,uint32_t){}
VKAPI_ATTR void VKAPI_CALL ICmdBeginQuery(VkCommandBuffer c,VkQueryPool q,uint32_t index,VkQueryControlFlags){if(c&&q)c->list->BeginQuery(q->heap.Get(),D3D12_QUERY_TYPE_OCCLUSION,index);}
VKAPI_ATTR void VKAPI_CALL ICmdEndQuery(VkCommandBuffer c,VkQueryPool q,uint32_t index){if(c&&q){const auto t=q->type==VK_QUERY_TYPE_OCCLUSION?D3D12_QUERY_TYPE_OCCLUSION:D3D12_QUERY_TYPE_TIMESTAMP;c->list->EndQuery(q->heap.Get(),t,index);}}
VKAPI_ATTR void VKAPI_CALL ICmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool q,uint32_t first,uint32_t count,VkBuffer dst,VkDeviceSize offset,VkDeviceSize stride,VkQueryResultFlags){if(!c||!q||!dst||!dst->resource)return;TransitionBuffer(c,dst,D3D12_RESOURCE_STATE_COPY_DEST);const auto t=q->type==VK_QUERY_TYPE_OCCLUSION?D3D12_QUERY_TYPE_OCCLUSION:D3D12_QUERY_TYPE_TIMESTAMP;if(stride==8)c->list->ResolveQueryData(q->heap.Get(),t,first,count,dst->resource.Get(),dst->memoryOffset+offset);else for(uint32_t i=0;i<count;i++)c->list->ResolveQueryData(q->heap.Get(),t,first+i,1,dst->resource.Get(),dst->memoryOffset+offset+VkDeviceSize(i)*stride);}
VKAPI_ATTR VkResult VKAPI_CALL IGetQueryPoolResults(VkDevice device,VkQueryPool q,uint32_t first,uint32_t count,size_t dataSize,void* data,VkDeviceSize stride,VkQueryResultFlags flags)
{
	if(!device||!q||!data||first+count>q->count)return VK_ERROR_INITIALIZATION_FAILED;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;if(FAILED(device->native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return VK_ERROR_DEVICE_LOST;const auto type=q->type==VK_QUERY_TYPE_OCCLUSION?D3D12_QUERY_TYPE_OCCLUSION:D3D12_QUERY_TYPE_TIMESTAMP;list->ResolveQueryData(q->heap.Get(),type,first,count,q->readback.Get(),VkDeviceSize(first)*8);list->Close();ID3D12CommandList* lists[]={list.Get()};device->queue.native->ExecuteCommandLists(1,lists);if(IDeviceWaitIdle(device)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;void* mapped{};D3D12_RANGE range{SIZE_T(first)*8,SIZE_T(first+count)*8};if(FAILED(q->readback->Map(0,&range,&mapped)))return VK_ERROR_MEMORY_MAP_FAILED;const bool use64=flags&VK_QUERY_RESULT_64_BIT;const size_t valueSize=use64?8:4;for(uint32_t i=0;i<count&&size_t(i)*stride+valueSize<=dataSize;i++){const uint64_t value=static_cast<const uint64_t*>(mapped)[first+i];if(use64)*reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(data)+size_t(i)*stride)=value;else *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data)+size_t(i)*stride)=static_cast<uint32_t>(value);}D3D12_RANGE written{0,0};q->readback->Unmap(0,&written);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL ICmdCopyImage(VkCommandBuffer c,VkImage src,VkImageLayout,VkImage dst,VkImageLayout,uint32_t count,const VkImageCopy* regions){if(!c||!src||!dst||!src->resource||!dst->resource)return;TransitionImage(c,src,D3D12_RESOURCE_STATE_COPY_SOURCE);TransitionImage(c,dst,D3D12_RESOURCE_STATE_COPY_DEST);for(uint32_t i=0;i<count;i++){D3D12_TEXTURE_COPY_LOCATION s{src->resource.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},d{dst->resource.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};s.SubresourceIndex=regions[i].srcSubresource.mipLevel+regions[i].srcSubresource.baseArrayLayer*src->info.mipLevels;d.SubresourceIndex=regions[i].dstSubresource.mipLevel+regions[i].dstSubresource.baseArrayLayer*dst->info.mipLevels;D3D12_BOX box{static_cast<UINT>(regions[i].srcOffset.x),static_cast<UINT>(regions[i].srcOffset.y),static_cast<UINT>(regions[i].srcOffset.z),static_cast<UINT>(regions[i].srcOffset.x+regions[i].extent.width),static_cast<UINT>(regions[i].srcOffset.y+regions[i].extent.height),static_cast<UINT>(regions[i].srcOffset.z+regions[i].extent.depth)};c->list->CopyTextureRegion(&d,regions[i].dstOffset.x,regions[i].dstOffset.y,regions[i].dstOffset.z,&s,&box);}}
VKAPI_ATTR void VKAPI_CALL ICmdCopyBufferToImage(VkCommandBuffer c,VkBuffer src,VkImage dst,VkImageLayout,uint32_t count,const VkBufferImageCopy* regions)
{
	TransitionImage(c,dst,D3D12_RESOURCE_STATE_COPY_DEST);
	if(!c||!src||!dst||!src->resource||!dst->resource)return;TransitionBuffer(c,src,D3D12_RESOURCE_STATE_COPY_SOURCE);const auto desc=dst->resource->GetDesc();for(uint32_t i=0;i<count;i++){const UINT sub=regions[i].imageSubresource.mipLevel+regions[i].imageSubresource.baseArrayLayer*dst->info.mipLevels;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowSize{},total{};c->device->native->GetCopyableFootprints(&desc,sub,1,src->memoryOffset+regions[i].bufferOffset,&fp,&rows,&rowSize,&total);if(regions[i].bufferRowLength)fp.Footprint.RowPitch=(regions[i].bufferRowLength*static_cast<UINT>(rowSize)/(std::max)(1u,regions[i].imageExtent.width)+255)&~255u;D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=src->resource.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=fp;D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=dst->resource.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;d.SubresourceIndex=sub;c->list->CopyTextureRegion(&d,regions[i].imageOffset.x,regions[i].imageOffset.y,regions[i].imageOffset.z,&s,nullptr);}}
VKAPI_ATTR void VKAPI_CALL ICmdCopyImageToBuffer(VkCommandBuffer c,VkImage src,VkImageLayout,VkBuffer dst,uint32_t count,const VkBufferImageCopy* regions)
{
	TransitionImage(c,src,D3D12_RESOURCE_STATE_COPY_SOURCE);
	if(!c||!src||!dst||!src->resource||!dst->resource)return;TransitionBuffer(c,dst,D3D12_RESOURCE_STATE_COPY_DEST);const auto desc=src->resource->GetDesc();for(uint32_t i=0;i<count;i++){const UINT sub=regions[i].imageSubresource.mipLevel+regions[i].imageSubresource.baseArrayLayer*src->info.mipLevels;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowSize{},total{};c->device->native->GetCopyableFootprints(&desc,sub,1,dst->memoryOffset+regions[i].bufferOffset,&fp,&rows,&rowSize,&total);D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=src->resource.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;s.SubresourceIndex=sub;D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=dst->resource.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=fp;D3D12_BOX box{static_cast<UINT>(regions[i].imageOffset.x),static_cast<UINT>(regions[i].imageOffset.y),static_cast<UINT>(regions[i].imageOffset.z),static_cast<UINT>(regions[i].imageOffset.x+regions[i].imageExtent.width),static_cast<UINT>(regions[i].imageOffset.y+regions[i].imageExtent.height),static_cast<UINT>(regions[i].imageOffset.z+regions[i].imageExtent.depth)};c->list->CopyTextureRegion(&d,0,0,0,&s,&box);}}
VKAPI_ATTR void VKAPI_CALL ICmdBlitImage(VkCommandBuffer c,VkImage src,VkImageLayout,VkImage dst,VkImageLayout,uint32_t count,const VkImageBlit* regions,VkFilter)
{
	if(!c||!src||!dst||!src->resource||!dst->resource)return;for(uint32_t i=0;i<count;i++){const auto& r=regions[i];const int sw=std::abs(r.srcOffsets[1].x-r.srcOffsets[0].x),sh=std::abs(r.srcOffsets[1].y-r.srcOffsets[0].y),dw=std::abs(r.dstOffsets[1].x-r.dstOffsets[0].x),dh=std::abs(r.dstOffsets[1].y-r.dstOffsets[0].y);if(sw==dw&&sh==dh){VkImageCopy copy{};copy.srcSubresource=r.srcSubresource;copy.srcOffset=r.srcOffsets[0];copy.dstSubresource=r.dstSubresource;copy.dstOffset=r.dstOffsets[0];copy.extent={static_cast<uint32_t>(sw),static_cast<uint32_t>(sh),1};ICmdCopyImage(c,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);continue;}const uint32_t base=c->device->resourceCursor.fetch_add(2);if(base+2>65536)continue;auto cpu=c->device->resourceHeap->GetCPUDescriptorHandleForHeapStart();auto gpu=c->device->resourceHeap->GetGPUDescriptorHandleForHeapStart();D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{cpu.ptr+SIZE_T(base)*c->device->resourceStride},uavCpu{cpu.ptr+SIZE_T(base+1)*c->device->resourceStride};D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{gpu.ptr+UINT64(base)*c->device->resourceStride},uavGpu{gpu.ptr+UINT64(base+1)*c->device->resourceStride};D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=src->resource->GetDesc().Format;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MostDetailedMip=r.srcSubresource.mipLevel;sv.Texture2D.MipLevels=1;c->device->native->CreateShaderResourceView(src->resource.Get(),&sv,srvCpu);D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=dst->resource->GetDesc().Format;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;uv.Texture2D.MipSlice=r.dstSubresource.mipLevel;c->device->native->CreateUnorderedAccessView(dst->resource.Get(),nullptr,&uv,uavCpu);D3D12_RESOURCE_BARRIER barriers[2]{};barriers[0].Type=barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barriers[0].Transition={src->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,src->state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};barriers[1].Transition={dst->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,dst->state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};c->list->ResourceBarrier(2,barriers);src->state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;dst->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;ID3D12DescriptorHeap* heaps[]={c->device->resourceHeap.Get()};c->list->SetDescriptorHeaps(1,heaps);c->list->SetComputeRootSignature(c->device->blitRoot.Get());c->list->SetPipelineState(c->device->blitPipeline.Get());c->list->SetComputeRootDescriptorTable(0,srvGpu);c->list->SetComputeRootDescriptorTable(1,uavGpu);const int constants[8]={r.srcOffsets[0].x,r.srcOffsets[0].y,sw,sh,r.dstOffsets[0].x,r.dstOffsets[0].y,dw,dh};c->list->SetComputeRoot32BitConstants(2,8,constants,0);c->list->Dispatch((dw+7)/8,(dh+7)/8,1);}}
VKAPI_ATTR VkResult VKAPI_CALL ICreateRenderPass(VkDevice,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks*,VkRenderPass* out)
{
	if(!info||!out||!info->subpassCount)return VK_ERROR_INITIALIZATION_FAILED;auto r=new VkRenderPass_T();r->attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);const auto& s=info->pSubpasses[0];r->colors.assign(s.pColorAttachments,s.pColorAttachments+s.colorAttachmentCount);if(s.pDepthStencilAttachment)r->depth=*s.pDepthStencilAttachment;*out=r;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL ICmdClearColorImage(VkCommandBuffer c,VkImage image,VkImageLayout,const VkClearColorValue* color,uint32_t rangeCount,const VkImageSubresourceRange*)
{
	TraceOperation(c,"ClearColorImage");
	if(!c||!image||!image->resource||!color||!rangeCount)return;const auto desc=image->resource->GetDesc();if(!(desc.Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))return;const auto clearState=(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;if(image->state!=clearState){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,clearState};c->list->ResourceBarrier(1,&barrier);image->state=clearState;}if(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET){uint32_t slot;if(!AllocateViewDescriptorSlot(c->device,false,slot))return;auto h=c->device->rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot)*c->device->rtvStride;D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=desc.Format;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE1D)view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE1D;else if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE3D;view.Texture3D.WSize=desc.DepthOrArraySize;}else view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;c->device->native->CreateRenderTargetView(image->resource.Get(),&view,h);c->list->ClearRenderTargetView(h,color->float32,0,nullptr);ReleaseViewDescriptorSlot(c->device,false,slot);}else{const uint32_t slot=c->device->resourceCursor.fetch_add(1);if(slot>=4096)return;auto cpu=c->device->resourceHeap->GetCPUDescriptorHandleForHeapStart();auto clearCpu=c->device->clearCpuHeap->GetCPUDescriptorHandleForHeapStart();auto gpu=c->device->resourceHeap->GetGPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*c->device->resourceStride;clearCpu.ptr+=SIZE_T(slot)*c->device->resourceStride;gpu.ptr+=UINT64(slot)*c->device->resourceStride;D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=desc.Format;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE1D)view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE1D;else if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;view.Texture3D.WSize=desc.DepthOrArraySize;}else view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;c->device->native->CreateUnorderedAccessView(image->resource.Get(),nullptr,&view,cpu);c->device->native->CreateUnorderedAccessView(image->resource.Get(),nullptr,&view,clearCpu);ID3D12DescriptorHeap* heaps[]={c->device->resourceHeap.Get()};c->list->SetDescriptorHeaps(1,heaps);c->list->ClearUnorderedAccessViewFloat(gpu,clearCpu,image->resource.Get(),color->float32,0,nullptr);}}
VKAPI_ATTR void VKAPI_CALL ICmdClearDepthStencilImage(VkCommandBuffer c,VkImage image,VkImageLayout,const VkClearDepthStencilValue* value,uint32_t rangeCount,const VkImageSubresourceRange* ranges)
{
	if(!c||!image||!image->resource||!value||!rangeCount||!ranges)return;const DXGI_FORMAT depthFormat=ToDxgiFormat(image->info.format);D3D12_FEATURE_DATA_FORMAT_SUPPORT support{depthFormat};if(depthFormat==DXGI_FORMAT_UNKNOWN||FAILED(c->device->native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||!(support.Support1&D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL))return;const bool hasStencil=image->info.format==VK_FORMAT_D24_UNORM_S8_UINT||image->info.format==VK_FORMAT_D32_SFLOAT_S8_UINT;D3D12_CLEAR_FLAGS clearFlags=static_cast<D3D12_CLEAR_FLAGS>(0);for(uint32_t i=0;i<rangeCount;i++){if(ranges[i].aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)clearFlags=static_cast<D3D12_CLEAR_FLAGS>(clearFlags|D3D12_CLEAR_FLAG_DEPTH);if(hasStencil&&(ranges[i].aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT))clearFlags=static_cast<D3D12_CLEAR_FLAGS>(clearFlags|D3D12_CLEAR_FLAG_STENCIL);}if(clearFlags==0)return;if(image->state!=D3D12_RESOURCE_STATE_DEPTH_WRITE){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,D3D12_RESOURCE_STATE_DEPTH_WRITE};c->list->ResourceBarrier(1,&barrier);image->state=D3D12_RESOURCE_STATE_DEPTH_WRITE;}uint32_t slot;if(!AllocateViewDescriptorSlot(c->device,true,slot))return;auto h=c->device->dsvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot)*c->device->dsvStride;D3D12_DEPTH_STENCIL_VIEW_DESC view{};view.Format=depthFormat;const auto& range=ranges[0];if(image->info.imageType==VK_IMAGE_TYPE_1D&&image->info.arrayLayers>1){view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE1DARRAY;view.Texture1DArray.MipSlice=range.baseMipLevel;view.Texture1DArray.FirstArraySlice=range.baseArrayLayer;view.Texture1DArray.ArraySize=range.layerCount==VK_REMAINING_ARRAY_LAYERS?image->info.arrayLayers-range.baseArrayLayer:range.layerCount;}else if(image->info.imageType==VK_IMAGE_TYPE_1D){view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE1D;view.Texture1D.MipSlice=range.baseMipLevel;}else if(image->info.arrayLayers>1){view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipSlice=range.baseMipLevel;view.Texture2DArray.FirstArraySlice=range.baseArrayLayer;view.Texture2DArray.ArraySize=range.layerCount==VK_REMAINING_ARRAY_LAYERS?image->info.arrayLayers-range.baseArrayLayer:range.layerCount;}else{view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;view.Texture2D.MipSlice=range.baseMipLevel;}c->device->native->CreateDepthStencilView(image->resource.Get(),&view,h);c->list->ClearDepthStencilView(h,clearFlags,value->depth,value->stencil,0,nullptr);ReleaseViewDescriptorSlot(c->device,true,slot);
}
VKAPI_ATTR void VKAPI_CALL IDestroyRenderPass(VkDevice,VkRenderPass r,const VkAllocationCallbacks*){delete r;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateFramebuffer(VkDevice,const VkFramebufferCreateInfo* info,const VkAllocationCallbacks*,VkFramebuffer* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;auto f=new VkFramebuffer_T();f->pass=info->renderPass;f->attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);f->width=info->width;f->height=info->height;*out=f;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyFramebuffer(VkDevice,VkFramebuffer f,const VkAllocationCallbacks*){delete f;}
VKAPI_ATTR void VKAPI_CALL ICmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo* info,VkSubpassContents)
{
	TraceOperation(c,"BeginRenderPass");
	if(!c||!info||!info->renderPass||!info->framebuffer)return;c->activeRenderPass=info->renderPass;c->activeFramebuffer=info->framebuffer;auto pass=info->renderPass;auto fb=info->framebuffer;std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
	for(const auto& ref:pass->colors)if(ref.attachment!=VK_ATTACHMENT_UNUSED&&ref.attachment<fb->attachments.size())
	{
		auto view=fb->attachments[ref.attachment];if(!view||!view->rtv.ptr)continue;auto image=view->image;const auto target=StateForLayout(image,ref.layout);if(image->state!=target){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,target};c->list->ResourceBarrier(1,&b);image->state=target;}rtvs.push_back(view->rtv);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE dsv{};if(pass->depth.attachment!=VK_ATTACHMENT_UNUSED&&pass->depth.attachment<fb->attachments.size())
	{
		auto view=fb->attachments[pass->depth.attachment];if(view&&view->dsv.ptr){auto image=view->image;const auto target=StateForLayout(image,pass->depth.layout);if(image->state!=target){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,target};c->list->ResourceBarrier(1,&b);image->state=target;}dsv=view->dsv;}
	}
	c->list->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),rtvs.data(),FALSE,dsv.ptr?&dsv:nullptr);
	for(uint32_t a=0;a<pass->attachments.size()&&a<info->clearValueCount;a++)
	{
		const auto& ad=pass->attachments[a];auto view=a<fb->attachments.size()?fb->attachments[a]:VK_NULL_HANDLE;if(!view)continue;if(ad.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR&&view->rtv.ptr)c->list->ClearRenderTargetView(view->rtv,info->pClearValues[a].color.float32,0,nullptr);if((ad.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR||ad.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)&&view->dsv.ptr){UINT flags=0;if(ad.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)flags|=D3D12_CLEAR_FLAG_DEPTH;if(ad.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)flags|=D3D12_CLEAR_FLAG_STENCIL;c->list->ClearDepthStencilView(view->dsv,static_cast<D3D12_CLEAR_FLAGS>(flags),info->pClearValues[a].depthStencil.depth,info->pClearValues[a].depthStencil.stencil,0,nullptr);}
	}
}
VKAPI_ATTR void VKAPI_CALL ICmdEndRenderPass(VkCommandBuffer c)
{
	if(!c)return;auto pass=c->activeRenderPass;auto fb=c->activeFramebuffer;if(pass&&fb)for(uint32_t i=0;i<pass->attachments.size()&&i<fb->attachments.size();i++){auto view=fb->attachments[i];if(!view||!view->image||!view->image->resource)continue;auto image=view->image;const auto target=StateForLayout(image,pass->attachments[i].finalLayout);if(target==image->state)continue;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,target};c->list->ResourceBarrier(1,&b);image->state=target;}c->activeRenderPass=VK_NULL_HANDLE;c->activeFramebuffer=VK_NULL_HANDLE;
}
VKAPI_ATTR void VKAPI_CALL ICmdClearAttachments(VkCommandBuffer c,uint32_t attachmentCount,const VkClearAttachment* attachments,uint32_t rectCount,const VkClearRect* rects)
{
	if(!c||!c->activeRenderPass||!c->activeFramebuffer)return;for(uint32_t i=0;i<attachmentCount;i++){const auto& a=attachments[i];VkImageView view=VK_NULL_HANDLE;if(a.aspectMask&VK_IMAGE_ASPECT_COLOR_BIT){if(a.colorAttachment<c->activeRenderPass->colors.size()){const auto index=c->activeRenderPass->colors[a.colorAttachment].attachment;if(index<c->activeFramebuffer->attachments.size())view=c->activeFramebuffer->attachments[index];}}else if(c->activeRenderPass->depth.attachment<c->activeFramebuffer->attachments.size())view=c->activeFramebuffer->attachments[c->activeRenderPass->depth.attachment];if(!view)continue;std::vector<D3D12_RECT> nativeRects(rectCount);for(uint32_t r=0;r<rectCount;r++)nativeRects[r]={rects[r].rect.offset.x,rects[r].rect.offset.y,rects[r].rect.offset.x+static_cast<LONG>(rects[r].rect.extent.width),rects[r].rect.offset.y+static_cast<LONG>(rects[r].rect.extent.height)};if(view->rtv.ptr)c->list->ClearRenderTargetView(view->rtv,a.clearValue.color.float32,rectCount,nativeRects.data());if(view->dsv.ptr){UINT flags=0;if(a.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)flags|=D3D12_CLEAR_FLAG_DEPTH;if(a.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)flags|=D3D12_CLEAR_FLAG_STENCIL;c->list->ClearDepthStencilView(view->dsv,static_cast<D3D12_CLEAR_FLAGS>(flags),a.clearValue.depthStencil.depth,a.clearValue.depthStencil.stencil,rectCount,nativeRects.data());}}}
VKAPI_ATTR void VKAPI_CALL IDestroySwapchainKHR(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ICreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks*,VkSwapchainKHR* out)
{
	if(!device||!info||!out)return VK_ERROR_INITIALIZATION_FAILED;
	ComPtr<IDXGIFactory4> factory;
	HRESULT hr=device->physical->adapter->GetParent(IID_PPV_ARGS(&factory));
	if(FAILED(hr)){cemuLog_log(LogType::Force,fmt::format("D3D12 swapchain: GetParent failed (HRESULT 0x{:08X})",static_cast<uint32_t>(hr)));return VK_ERROR_INITIALIZATION_FAILED;}
	DXGI_SWAP_CHAIN_DESC1 d{};d.Width=info->imageExtent.width;d.Height=info->imageExtent.height;d.Format=ToDxgiFormat(info->imageFormat);d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=(std::max)(2u,info->minImageCount);d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;d.Scaling=DXGI_SCALING_STRETCH;d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
	ComPtr<IDXGISwapChain1> chain;
	hr=factory->CreateSwapChainForComposition(device->queue.native.Get(),&d,nullptr,&chain);
	if(FAILED(hr)){cemuLog_log(LogType::Force,fmt::format("D3D12 swapchain: CreateSwapChainForComposition failed (HRESULT 0x{:08X}, {}x{}, format {})",static_cast<uint32_t>(hr),d.Width,d.Height,static_cast<uint32_t>(d.Format)));return VK_ERROR_INITIALIZATION_FAILED;}
	auto swap=new VkSwapchainKHR_T();swap->device=device;
	hr=chain.As(&swap->native);if(FAILED(hr)){cemuLog_log(LogType::Force,fmt::format("D3D12 swapchain: IDXGISwapChain3 query failed (HRESULT 0x{:08X})",static_cast<uint32_t>(hr)));delete swap;return VK_ERROR_INITIALIZATION_FAILED;}
	if(FAILED(device->native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&swap->presentFence)))){delete swap;return VK_ERROR_OUT_OF_HOST_MEMORY;}
	swap->presentEvent=CreateEventEx(nullptr,nullptr,0,EVENT_ALL_ACCESS);if(!swap->presentEvent){delete swap;return VK_ERROR_OUT_OF_HOST_MEMORY;}
	auto* hostSurface=static_cast<const CemuEmbedD3D11Surface*>(WindowSystem::GetWindowInfo().canvas_main.surface);
	if(!hostSurface||hostSurface->struct_size<sizeof(CemuEmbedD3D11Surface)||hostSurface->abi_version!=CEMU_EMBED_D3D11_SURFACE_VERSION||!hostSurface->set_composition_swap_chain){cemuLog_log(LogType::Force,"D3D12 swapchain: host composition callback is unavailable or has an incompatible ABI");CloseHandle(swap->presentEvent);delete swap;return VK_ERROR_SURFACE_LOST_KHR;}
	hr=static_cast<HRESULT>(hostSurface->set_composition_swap_chain(hostSurface->set_composition_swap_chain_user_data,swap->native.Get()));
	if(FAILED(hr)){cemuLog_log(LogType::Force,fmt::format("D3D12 swapchain: host failed to attach the swapchain (HRESULT 0x{:08X})",static_cast<uint32_t>(hr)));CloseHandle(swap->presentEvent);delete swap;return VK_ERROR_SURFACE_LOST_KHR;}
	swap->setCompositionSwapChain=hostSurface->set_composition_swap_chain;swap->compositionUserData=hostSurface->set_composition_swap_chain_user_data;
	for(UINT i=0;i<d.BufferCount;i++){auto image=new VkImage_T();image->device=device;image->info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image->info.format=info->imageFormat;image->info.extent={info->imageExtent.width,info->imageExtent.height,1};image->info.mipLevels=1;image->info.arrayLayers=1;image->info.samples=VK_SAMPLE_COUNT_1_BIT;image->info.usage=info->imageUsage;image->state=D3D12_RESOURCE_STATE_PRESENT;hr=swap->native->GetBuffer(i,IID_PPV_ARGS(&image->resource));if(FAILED(hr)){cemuLog_log(LogType::Force,fmt::format("D3D12 swapchain: GetBuffer({}) failed (HRESULT 0x{:08X})",i,static_cast<uint32_t>(hr)));delete image;IDestroySwapchainKHR(device,swap,nullptr);return VK_ERROR_INITIALIZATION_FAILED;}swap->images.push_back(image);}*out=swap;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL IDestroySwapchainKHR(VkDevice,VkSwapchainKHR swap,const VkAllocationCallbacks*){if(swap){if(swap->setCompositionSwapChain)swap->setCompositionSwapChain(swap->compositionUserData,nullptr);else if(swap->panel)swap->panel->SetSwapChain(nullptr);for(auto image:swap->images)delete image;if(swap->presentEvent)CloseHandle(swap->presentEvent);delete swap;}}
VKAPI_ATTR VkResult VKAPI_CALL IGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR swap,uint32_t* count,VkImage* out){if(!swap||!count)return VK_ERROR_INITIALIZATION_FAILED;if(!out){*count=static_cast<uint32_t>(swap->images.size());return VK_SUCCESS;}const uint32_t n=(std::min)(*count,static_cast<uint32_t>(swap->images.size()));std::copy_n(swap->images.begin(),n,out);*count=n;return n==swap->images.size()?VK_SUCCESS:VK_INCOMPLETE;}
VKAPI_ATTR VkResult VKAPI_CALL IAcquireNextImageKHR(VkDevice,VkSwapchainKHR swap,uint64_t,VkSemaphore semaphore,VkFence fence,uint32_t* index){if(!swap||!index)return VK_ERROR_OUT_OF_DATE_KHR;*index=swap->native->GetCurrentBackBufferIndex();if(semaphore){const auto v=semaphore->value.fetch_add(1)+1;semaphore->native->Signal(v);}if(fence)fence->native->Signal(fence->value);return VK_SUCCESS;}
VkResult TransitionSwapchainForPresent(VkQueue q,VkSwapchainKHR swap)
{
	if(!q||!swap)return VK_ERROR_OUT_OF_DATE_KHR;const UINT index=swap->native->GetCurrentBackBufferIndex();if(index>=swap->images.size())return VK_ERROR_OUT_OF_DATE_KHR;auto image=swap->images[index];if(!image||!image->resource||image->state==D3D12_RESOURCE_STATE_PRESENT)return VK_SUCCESS;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;if(FAILED(q->device->native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(q->device->native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return VK_ERROR_DEVICE_LOST;D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,D3D12_RESOURCE_STATE_PRESENT};list->ResourceBarrier(1,&barrier);if(FAILED(list->Close()))return VK_ERROR_DEVICE_LOST;ID3D12CommandList* lists[]={list.Get()};q->native->ExecuteCommandLists(1,lists);image->state=D3D12_RESOURCE_STATE_PRESENT;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL IQueuePresentKHR(VkQueue q,const VkPresentInfoKHR* info){if(!q||!info)return VK_ERROR_INITIALIZATION_FAILED;const VkPresentIdKHR* ids=nullptr;for(auto* next=static_cast<const VkBaseInStructure*>(info->pNext);next;next=next->pNext)if(next->sType==VK_STRUCTURE_TYPE_PRESENT_ID_KHR)ids=reinterpret_cast<const VkPresentIdKHR*>(next);for(uint32_t i=0;i<info->waitSemaphoreCount;i++)q->native->Wait(info->pWaitSemaphores[i]->native.Get(),info->pWaitSemaphores[i]->value.load());VkResult final=VK_SUCCESS;for(uint32_t i=0;i<info->swapchainCount;i++){auto swap=info->pSwapchains[i];const VkResult transition=TransitionSwapchainForPresent(q,swap);HRESULT hr=S_OK;if(transition!=VK_SUCCESS)final=transition;else hr=swap->native->Present(1,0);if(FAILED(hr))final=hr==DXGI_ERROR_DEVICE_REMOVED?VK_ERROR_DEVICE_LOST:VK_ERROR_OUT_OF_DATE_KHR;else if(transition==VK_SUCCESS){swap->presentValue=ids&&i<ids->swapchainCount?ids->pPresentIds[i]:swap->presentValue+1;q->native->Signal(swap->presentFence.Get(),swap->presentValue);}if(info->pResults)info->pResults[i]=final;}return final;}
VKAPI_ATTR VkResult VKAPI_CALL IWaitForPresentKHR(VkDevice,VkSwapchainKHR swap,uint64_t presentId,uint64_t timeout){if(!swap)return VK_ERROR_OUT_OF_DATE_KHR;const uint64_t value=presentId?presentId:swap->presentValue;if(swap->presentFence->GetCompletedValue()>=value)return VK_SUCCESS;if(FAILED(swap->presentFence->SetEventOnCompletion(value,swap->presentEvent)))return VK_ERROR_DEVICE_LOST;const DWORD ms=timeout==UINT64_MAX?INFINITE:static_cast<DWORD>((std::min)(timeout/1000000ull,uint64_t(INFINITE-1)));return WaitForSingleObjectEx(swap->presentEvent,ms,FALSE)==WAIT_OBJECT_0?VK_SUCCESS:VK_TIMEOUT;}
VKAPI_ATTR VkResult VKAPI_CALL ICreateComputePipelines(VkDevice device,VkPipelineCache,uint32_t count,const VkComputePipelineCreateInfo* infos,const VkAllocationCallbacks*,VkPipeline* out)
{
	if(!device||!infos||!out)return VK_ERROR_INITIALIZATION_FAILED;for(uint32_t i=0;i<count;i++){auto code=CompileStage(infos[i].stage.module,VK_SHADER_STAGE_COMPUTE_BIT,infos[i].stage.pName);if(!code)return VK_ERROR_INVALID_SHADER_NV;D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=infos[i].layout->root.Get();d.CS={code->GetBufferPointer(),code->GetBufferSize()};auto p=new VkPipeline_T();p->layout=infos[i].layout;p->compute=true;if(FAILED(device->native->CreateComputePipelineState(&d,IID_PPV_ARGS(&p->state)))){delete p;return VK_ERROR_INITIALIZATION_FAILED;}out[i]=p;}return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL ICmdBeginRenderingKHR(VkCommandBuffer c,const VkRenderingInfoKHR* info)
{
		if(!c||!info)return;std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;for(uint32_t i=0;i<info->colorAttachmentCount;i++){const auto& a=info->pColorAttachments[i];if(!a.imageView||!a.imageView->rtv.ptr)continue;auto image=a.imageView->image;if(image->state!=D3D12_RESOURCE_STATE_RENDER_TARGET){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,D3D12_RESOURCE_STATE_RENDER_TARGET};c->list->ResourceBarrier(1,&b);image->state=D3D12_RESOURCE_STATE_RENDER_TARGET;}rtvs.push_back(a.imageView->rtv);if(a.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)c->list->ClearRenderTargetView(a.imageView->rtv,a.clearValue.color.float32,0,nullptr);}D3D12_CPU_DESCRIPTOR_HANDLE dsv{};const VkRenderingAttachmentInfoKHR* depth=info->pDepthAttachment?info->pDepthAttachment:info->pStencilAttachment;if(depth&&depth->imageView&&depth->imageView->dsv.ptr){auto image=depth->imageView->image;if(image->state!=D3D12_RESOURCE_STATE_DEPTH_WRITE){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image->resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image->state,D3D12_RESOURCE_STATE_DEPTH_WRITE};c->list->ResourceBarrier(1,&b);image->state=D3D12_RESOURCE_STATE_DEPTH_WRITE;}dsv=depth->imageView->dsv;if(depth->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR){UINT flags=info->pDepthAttachment?D3D12_CLEAR_FLAG_DEPTH:0;if(info->pStencilAttachment)flags|=D3D12_CLEAR_FLAG_STENCIL;c->list->ClearDepthStencilView(dsv,static_cast<D3D12_CLEAR_FLAGS>(flags),depth->clearValue.depthStencil.depth,depth->clearValue.depthStencil.stencil,0,nullptr);}}c->list->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),rtvs.data(),FALSE,dsv.ptr?&dsv:nullptr);
}
VKAPI_ATTR void VKAPI_CALL ICmdEndRenderingKHR(VkCommandBuffer){}
VKAPI_ATTR void VKAPI_CALL ICmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer, VkImageAspectFlags){}
VKAPI_ATTR void VKAPI_CALL ICmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){if(c)c->list->Dispatch(x,y,z);}
VKAPI_ATTR VkResult VKAPI_CALL ICreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR* info, const VkAllocationCallbacks*, VkSurfaceKHR* out)
{ if (!info || !out || !info->hwnd) return VK_ERROR_INITIALIZATION_FAILED; auto s = std::make_unique<VkSurfaceKHR_T>(); s->window = info->hwnd; *out = s.release(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL IDestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*) { delete surface; }
VKAPI_ATTR VkResult VKAPI_CALL IGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t family, VkSurfaceKHR surface, VkBool32* out)
{ if (!out) return VK_ERROR_INITIALIZATION_FAILED; *out = family == 0 && surface; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL IGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* out)
{
	if (!surface || !out) return VK_ERROR_SURFACE_LOST_KHR;
	int width = 0;
	int height = 0;
	WindowSystem::GetWindowPhysSize(width, height);
	*out = {};
	out->minImageCount = 2; out->maxImageCount = 3; out->currentExtent = {static_cast<uint32_t>((std::max)(width, 1)), static_cast<uint32_t>((std::max)(height, 1))};
	out->minImageExtent = {1,1}; out->maxImageExtent = {D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION}; out->maxImageArrayLayers = 1;
	out->supportedTransforms = out->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; out->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	out->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL IGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkSurfaceFormatKHR* out)
{ constexpr std::array<VkSurfaceFormatKHR,2> f{{{VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},{VK_FORMAT_B8G8R8A8_SRGB,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}}; return Enumerate(f,count,out); }
VKAPI_ATTR VkResult VKAPI_CALL IGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkPresentModeKHR* out)
{ constexpr std::array<VkPresentModeKHR,2> m{{VK_PRESENT_MODE_FIFO_KHR,VK_PRESENT_MODE_IMMEDIATE_KHR}}; return Enumerate(m,count,out); }
VKAPI_ATTR VkResult VKAPI_CALL IGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice, uint32_t* count, VkPhysicalDeviceToolProperties*)
{ if (!count) return VK_ERROR_INITIALIZATION_FAILED; *count=0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL ISetDebugUtilsObjectNameEXT(VkDevice,const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL ICreateDebugReportCallbackEXT(VkInstance,const VkDebugReportCallbackCreateInfoEXT* info,const VkAllocationCallbacks*,VkDebugReportCallbackEXT* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;auto cb=new VkDebugReportCallbackEXT_T();cb->callback=info->pfnCallback;cb->userData=info->pUserData;*out=cb;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL IDestroyDebugReportCallbackEXT(VkInstance,VkDebugReportCallbackEXT cb,const VkAllocationCallbacks*){delete cb;}

PFN_vkVoidFunction Lookup(std::string_view name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL IGetInstanceProcAddr(VkInstance,const char* name) { return name ? Lookup(name) : nullptr; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL IGetDeviceProcAddr(VkDevice,const char* name) { return name ? Lookup(name) : nullptr; }
PFN_vkVoidFunction Lookup(std::string_view n)
{
#define P(name) if(n=="vk" #name) return reinterpret_cast<PFN_vkVoidFunction>(I##name)
	P(GetInstanceProcAddr); P(GetDeviceProcAddr); P(CreateInstance); P(DestroyInstance); P(EnumerateInstanceVersion);
	P(EnumerateInstanceExtensionProperties); P(EnumerateDeviceExtensionProperties); P(EnumeratePhysicalDevices); P(CreateDevice); P(DestroyDevice); P(DeviceWaitIdle); P(GetDeviceQueue);
	P(GetPhysicalDeviceQueueFamilyProperties); P(GetPhysicalDeviceProperties); P(GetPhysicalDeviceProperties2); P(GetPhysicalDeviceFeatures2); P(GetPhysicalDeviceMemoryProperties); P(GetPhysicalDeviceFormatProperties);
	P(CreateWin32SurfaceKHR); P(DestroySurfaceKHR); P(GetPhysicalDeviceSurfaceSupportKHR); P(GetPhysicalDeviceSurfaceCapabilitiesKHR); P(GetPhysicalDeviceSurfaceFormatsKHR); P(GetPhysicalDeviceSurfacePresentModesKHR);
	P(GetPhysicalDeviceToolPropertiesEXT); P(SetDebugUtilsObjectNameEXT); P(CreateDebugReportCallbackEXT); P(DestroyDebugReportCallbackEXT);
	P(AllocateMemory); P(FreeMemory); P(CreateBuffer); P(DestroyBuffer); P(GetBufferMemoryRequirements); P(BindBufferMemory); P(MapMemory); P(UnmapMemory); P(FlushMappedMemoryRanges); P(InvalidateMappedMemoryRanges);
	P(CreateImage); P(DestroyImage); P(GetImageMemoryRequirements); P(BindImageMemory); P(CreateCommandPool); P(DestroyCommandPool); P(AllocateCommandBuffers); P(FreeCommandBuffers); P(BeginCommandBuffer); P(EndCommandBuffer); P(ResetCommandBuffer); P(QueueSubmit);
	P(CreateFence); P(DestroyFence); P(GetFenceStatus); P(ResetFences); P(WaitForFences); P(CreateSemaphore); P(DestroySemaphore); P(CreateEvent); P(DestroyEvent); P(GetEventStatus); P(CmdSetEvent); P(CmdWaitEvents); P(CmdPipelineBarrier); P(CmdPipelineBarrier2KHR); P(CmdCopyBuffer); P(CmdBindIndexBuffer); P(CmdBindVertexBuffers); P(CmdSetViewport); P(CmdSetScissor); P(CmdSetBlendConstants); P(CmdSetDepthBias); P(CmdDraw); P(CmdDrawIndexed);
	P(CreateShaderModule);P(DestroyShaderModule);P(CreateImageView);P(DestroyImageView);P(CreateSampler);P(DestroySampler);P(CreateDescriptorSetLayout);P(DestroyDescriptorSetLayout);P(CreateDescriptorPool);P(DestroyDescriptorPool);P(AllocateDescriptorSets);P(FreeDescriptorSets);P(UpdateDescriptorSets);P(CreatePipelineLayout);P(DestroyPipelineLayout);P(CreateGraphicsPipelines);P(CreateComputePipelines);P(DestroyPipeline);P(CmdBindPipeline);P(CmdBindDescriptorSets);P(CmdPushConstants);P(CmdDispatch);P(CreatePipelineCache);P(DestroyPipelineCache);P(GetPipelineCacheData);P(MergePipelineCaches);
	P(CreateQueryPool);P(DestroyQueryPool);P(GetQueryPoolResults);P(CmdResetQueryPool);P(CmdBeginQuery);P(CmdEndQuery);P(CmdCopyQueryPoolResults);P(CmdCopyImage);P(CmdCopyBufferToImage);P(CmdCopyImageToBuffer);P(CmdBlitImage);P(CmdClearColorImage);P(CmdClearDepthStencilImage);P(CmdClearAttachments);P(CreateRenderPass);P(DestroyRenderPass);P(CreateFramebuffer);P(DestroyFramebuffer);P(CmdBeginRenderPass);P(CmdEndRenderPass);P(CmdBeginRenderingKHR);P(CmdEndRenderingKHR);P(CmdSetAttachmentFeedbackLoopEnableEXT);P(CreateSwapchainKHR);P(DestroySwapchainKHR);P(GetSwapchainImagesKHR);P(AcquireNextImageKHR);P(QueuePresentKHR);P(WaitForPresentKHR);
#undef P
	return nullptr;
}
}

void SelectInternalDriver(bool selected) { s_selected.store(selected, std::memory_order_release); }
bool IsInternalDriverSelected() { return s_selected.load(std::memory_order_acquire); }
bool InitializeGlobalDispatch()
{
	std::scoped_lock lock(s_dispatchMutex);
	EnableDeviceRemovedDiagnostics();
	vkGetInstanceProcAddr=IGetInstanceProcAddr; vkGetDeviceProcAddr=IGetDeviceProcAddr; vkCreateInstance=ICreateInstance;
	vkEnumerateInstanceExtensionProperties=IEnumerateInstanceExtensionProperties; vkEnumerateDeviceExtensionProperties=IEnumerateDeviceExtensionProperties; vkEnumerateInstanceVersion=IEnumerateInstanceVersion;
	cemuLog_log(LogType::Force,"Internal Vulkan-to-D3D12 global dispatch initialized (no Vulkan loader or Mesa runtime)"); return true;
}
bool InitializeInstanceDispatch(VkInstance)
{
	std::scoped_lock lock(s_dispatchMutex);
	vkDestroyInstance=IDestroyInstance; vkEnumeratePhysicalDevices=IEnumeratePhysicalDevices; vkCreateDevice=ICreateDevice; vkDestroyDevice=IDestroyDevice; vkDeviceWaitIdle=IDeviceWaitIdle;
	vkGetPhysicalDeviceQueueFamilyProperties=IGetPhysicalDeviceQueueFamilyProperties; vkGetPhysicalDeviceSurfaceSupportKHR=IGetPhysicalDeviceSurfaceSupportKHR; vkGetPhysicalDeviceSurfaceCapabilitiesKHR=IGetPhysicalDeviceSurfaceCapabilitiesKHR;
	vkGetPhysicalDeviceSurfaceFormatsKHR=IGetPhysicalDeviceSurfaceFormatsKHR; vkGetPhysicalDeviceSurfacePresentModesKHR=IGetPhysicalDeviceSurfacePresentModesKHR; vkGetPhysicalDeviceMemoryProperties=IGetPhysicalDeviceMemoryProperties;
	vkGetPhysicalDeviceProperties=IGetPhysicalDeviceProperties; vkGetPhysicalDeviceProperties2=IGetPhysicalDeviceProperties2; vkGetPhysicalDeviceFeatures2=IGetPhysicalDeviceFeatures2; vkGetPhysicalDeviceFormatProperties=IGetPhysicalDeviceFormatProperties;
	vkCreateWin32SurfaceKHR=ICreateWin32SurfaceKHR; vkDestroySurfaceKHR=IDestroySurfaceKHR; vkGetPhysicalDeviceToolPropertiesEXT=IGetPhysicalDeviceToolPropertiesEXT; vkSetDebugUtilsObjectNameEXT=ISetDebugUtilsObjectNameEXT;vkCreateDebugReportCallbackEXT=ICreateDebugReportCallbackEXT;vkDestroyDebugReportCallbackEXT=IDestroyDebugReportCallbackEXT; return true;
}
bool InitializeDeviceDispatch(VkDevice)
{
	std::scoped_lock lock(s_dispatchMutex); vkGetDeviceQueue=IGetDeviceQueue;
	vkAllocateMemory=IAllocateMemory;vkFreeMemory=IFreeMemory;vkCreateBuffer=ICreateBuffer;vkDestroyBuffer=IDestroyBuffer;vkGetBufferMemoryRequirements=IGetBufferMemoryRequirements;vkBindBufferMemory=IBindBufferMemory;vkMapMemory=IMapMemory;vkUnmapMemory=IUnmapMemory;vkFlushMappedMemoryRanges=IFlushMappedMemoryRanges;vkInvalidateMappedMemoryRanges=IInvalidateMappedMemoryRanges;
	vkCreateImage=ICreateImage;vkDestroyImage=IDestroyImage;vkGetImageMemoryRequirements=IGetImageMemoryRequirements;vkBindImageMemory=IBindImageMemory;
	vkCreateCommandPool=ICreateCommandPool;vkDestroyCommandPool=IDestroyCommandPool;vkAllocateCommandBuffers=IAllocateCommandBuffers;vkFreeCommandBuffers=IFreeCommandBuffers;vkBeginCommandBuffer=IBeginCommandBuffer;vkEndCommandBuffer=IEndCommandBuffer;vkResetCommandBuffer=IResetCommandBuffer;vkQueueSubmit=IQueueSubmit;
	vkCreateFence=ICreateFence;vkDestroyFence=IDestroyFence;vkGetFenceStatus=IGetFenceStatus;vkResetFences=IResetFences;vkWaitForFences=IWaitForFences;vkCreateSemaphore=ICreateSemaphore;vkDestroySemaphore=IDestroySemaphore;vkCreateEvent=ICreateEvent;vkDestroyEvent=IDestroyEvent;vkGetEventStatus=IGetEventStatus;vkCmdSetEvent=ICmdSetEvent;vkCmdWaitEvents=ICmdWaitEvents;
	vkCmdPipelineBarrier=ICmdPipelineBarrier;vkCmdPipelineBarrier2KHR=ICmdPipelineBarrier2KHR;vkCmdCopyBuffer=ICmdCopyBuffer;vkCmdBindIndexBuffer=ICmdBindIndexBuffer;vkCmdBindVertexBuffers=ICmdBindVertexBuffers;vkCmdSetViewport=ICmdSetViewport;vkCmdSetScissor=ICmdSetScissor;vkCmdSetBlendConstants=ICmdSetBlendConstants;vkCmdSetDepthBias=ICmdSetDepthBias;vkCmdDraw=ICmdDraw;vkCmdDrawIndexed=ICmdDrawIndexed;
	vkCreateShaderModule=ICreateShaderModule;vkDestroyShaderModule=IDestroyShaderModule;vkCreateImageView=ICreateImageView;vkDestroyImageView=IDestroyImageView;vkCreateSampler=ICreateSampler;vkDestroySampler=IDestroySampler;
	vkCreateDescriptorSetLayout=ICreateDescriptorSetLayout;vkDestroyDescriptorSetLayout=IDestroyDescriptorSetLayout;vkCreateDescriptorPool=ICreateDescriptorPool;vkDestroyDescriptorPool=IDestroyDescriptorPool;vkAllocateDescriptorSets=IAllocateDescriptorSets;vkFreeDescriptorSets=IFreeDescriptorSets;vkUpdateDescriptorSets=IUpdateDescriptorSets;
	vkCreatePipelineLayout=ICreatePipelineLayout;vkDestroyPipelineLayout=IDestroyPipelineLayout;vkCreateGraphicsPipelines=ICreateGraphicsPipelines;vkCreateComputePipelines=ICreateComputePipelines;vkDestroyPipeline=IDestroyPipeline;vkCmdBindPipeline=ICmdBindPipeline;vkCmdBindDescriptorSets=ICmdBindDescriptorSets;vkCmdPushConstants=ICmdPushConstants;vkCmdDispatch=ICmdDispatch;vkCreatePipelineCache=ICreatePipelineCache;vkDestroyPipelineCache=IDestroyPipelineCache;vkGetPipelineCacheData=IGetPipelineCacheData;vkMergePipelineCaches=IMergePipelineCaches;
	vkCreateQueryPool=ICreateQueryPool;vkDestroyQueryPool=IDestroyQueryPool;vkGetQueryPoolResults=IGetQueryPoolResults;vkCmdResetQueryPool=ICmdResetQueryPool;vkCmdBeginQuery=ICmdBeginQuery;vkCmdEndQuery=ICmdEndQuery;vkCmdCopyQueryPoolResults=ICmdCopyQueryPoolResults;vkCmdCopyImage=ICmdCopyImage;vkCmdCopyBufferToImage=ICmdCopyBufferToImage;vkCmdCopyImageToBuffer=ICmdCopyImageToBuffer;vkCmdBlitImage=ICmdBlitImage;vkCmdClearColorImage=ICmdClearColorImage;vkCmdClearDepthStencilImage=ICmdClearDepthStencilImage;vkCmdClearAttachments=ICmdClearAttachments;
	vkCreateRenderPass=ICreateRenderPass;vkDestroyRenderPass=IDestroyRenderPass;vkCreateFramebuffer=ICreateFramebuffer;vkDestroyFramebuffer=IDestroyFramebuffer;vkCmdBeginRenderPass=ICmdBeginRenderPass;vkCmdEndRenderPass=ICmdEndRenderPass;vkCmdBeginRenderingKHR=ICmdBeginRenderingKHR;vkCmdEndRenderingKHR=ICmdEndRenderingKHR;vkCmdSetAttachmentFeedbackLoopEnableEXT=ICmdSetAttachmentFeedbackLoopEnableEXT;
	vkCreateSwapchainKHR=ICreateSwapchainKHR;vkDestroySwapchainKHR=IDestroySwapchainKHR;vkGetSwapchainImagesKHR=IGetSwapchainImagesKHR;vkAcquireNextImageKHR=IAcquireNextImageKHR;vkQueuePresentKHR=IQueuePresentKHR;vkWaitForPresentKHR=IWaitForPresentKHR;
	cemuLog_log(LogType::Force,"Experimental Vulkan-to-D3D12 device dispatch initialized");
	return true;
}
}
