#include "Cafe/HW/Latte/Renderer/D3D11/D3D11Renderer.h"

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteQueryObject.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cemu/CemuEmbed.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "interface/WindowSystem.h"

#include <backends/imgui_impl_dx11.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <Psapi.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_hlsl.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include "imgui/imgui_extension.h"
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

using Microsoft::WRL::ComPtr;

// Implemented by the common renderer. GX2 uses this special state to clear
// color aliases of a depth surface without issuing a normal shader draw.
void LatteDraw_handleSpecialState8_clearAsDepth();

namespace
{
#if defined(CEMU_UWP)
// Serialize Cemu shader translation with the first draw that makes D3D11On12
// compile a native Xbox pipeline. Both paths have large transient footprints.
std::mutex s_xboxShaderPipelineMutex;
#endif

#if defined(CEMU_D3D11_DRIVER_TRACE)
std::atomic_uint64_t s_driverCallSequence{};

class DriverCallTrace
{
public:
	explicit DriverCallTrace(std::string operation)
		: m_operation(std::move(operation)),
		  m_sequence(s_driverCallSequence.fetch_add(1, std::memory_order_relaxed) + 1)
	{
		Write("BEGIN");
	}

	~DriverCallTrace()
	{
		Write("END");
	}

private:
	void Write(const char* phase) const
	{
		const std::string line =
			fmt::format("[Cemu/D3D11] {} #{} {}\n", phase, m_sequence, m_operation);
		OutputDebugStringA(line.c_str());
	}

	std::string m_operation;
	uint64 m_sequence{};
};
#define CEMU_D3D11_JOIN_IMPL(a, b) a##b
#define CEMU_D3D11_JOIN(a, b) CEMU_D3D11_JOIN_IMPL(a, b)
#define D3D11_DRIVER_TRACE(operation) \
	DriverCallTrace CEMU_D3D11_JOIN(driverCallTrace, __LINE__)(operation)
#else
#define D3D11_DRIVER_TRACE(operation) do { } while (false)
#endif

#if defined(CEMU_D3D11_DEBUG_VALIDATION)
#define D3D11_DEBUG_CHECK(scope) CheckDebugMessages(scope)
#else
#define D3D11_DEBUG_CHECK(scope) do { } while (false)
#endif

void ThrowIfFailed(HRESULT result, const char* operation)
{
	if (FAILED(result))
	{
		const std::string message = fmt::format(
			"{} failed with HRESULT 0x{:08X}", operation,
			static_cast<uint32>(result));
		// Preserve the failing operation in log.txt even when this originated on
		// a worker thread.  Visual Studio's first-chance _com_error entry alone
		// does not contain enough information to diagnose an Xbox driver failure.
		cemuLog_log(LogType::Force, "D3D11: {}", message);
		throw std::runtime_error(message);
	}
}

uint32 Align16(uint32 value)
{
	return (value + 15u) & ~15u;
}

bool IsMemoryPressureResult(HRESULT result)
{
	// Some UWP SDK/header combinations do not declare DXGI_ERROR_OUT_OF_MEMORY.
	// Keep the numeric DXGI HRESULT local so both allocator and driver OOMs use
	// the same recoverable path without making the build depend on that macro.
	constexpr HRESULT kDxgiErrorOutOfMemory = static_cast<HRESULT>(0x887A000Eu);
	return result == E_OUTOFMEMORY || result == kDxgiErrorOutOfMemory;
}

bool IsDeviceLostResult(HRESULT result)
{
	return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
		result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

uint64 QueryProcessPrivateCommitBytes()
{
	PROCESS_MEMORY_COUNTERS_EX counters{};
	counters.cb = sizeof(counters);
	if (!GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
		return 0;
	return static_cast<uint64>(counters.PrivateUsage);
}

UINT RuntimeShaderCompileFlags()
{
	// Fully unoptimized DXBC leaves large, deeply nested control-flow expressions
	// for the Series S driver compiler. newbe_xs.dll has exhausted even a 32 MiB
	// thread stack while lowering that form. Level 1 performs the inexpensive
	// canonicalization needed before the driver sees it, without the transient
	// memory peak of Level 3. The flags are part of the cache key, so bytecode
	// produced by the previous SKIP_OPTIMIZATION policy is not reused.
#if defined(CEMU_UWP)
	return D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL1;
#else
	return D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
}

uint64 HashBytes(const void* data, size_t size, uint64 hash = 1469598103934665603ull)
{
	const auto* bytes = static_cast<const uint8*>(data);
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}

#if defined(CEMU_UWP)
fs::path GetD3D11SpirvCachePath(const std::string& source, uint32 shaderType)
{
	uint64 hash = HashBytes(source.data(), source.size());
	hash = HashBytes(&shaderType, sizeof(shaderType), hash);
	// Bump when the glslang environment or SPIR-V optimization policy changes.
	constexpr uint32 spirvCacheVersion = 1;
	hash = HashBytes(&spirvCacheVersion, sizeof(spirvCacheVersion), hash);
	return ActiveSettings::GetUserDataPath(
		fmt::format("shaderCache/driver/d3d11/spirv/{:016x}.spv", hash));
}

bool LoadD3D11SpirvCache(const fs::path& path, std::vector<uint32>& spirv)
{
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return false;
	const std::streamoff length = static_cast<std::streamoff>(input.tellg());
	if (length < 5 * static_cast<std::streamoff>(sizeof(uint32)) ||
		length > 64 * 1024 * 1024 || (length % sizeof(uint32)) != 0)
		return false;
	spirv.resize(static_cast<size_t>(length) / sizeof(uint32));
	input.seekg(0);
	input.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(length));
	bool valid = input && spirv.size() > 5 && spirv[0] == 0x07230203u &&
		spirv[3] != 0 && spirv[4] == 0;
	// A truncated SPIR-V file often retains a valid header. Walk every
	// instruction so SPIRV-Cross never receives a partial cached module and turns
	// a recoverable cache-write interruption into a permanent missing shader.
	for (size_t word = 5; valid && word < spirv.size();)
	{
		const uint32 instructionWords = spirv[word] >> 16;
		if (instructionWords == 0 || instructionWords > spirv.size() - word)
		{
			valid = false;
			break;
		}
		word += instructionWords;
		if (word == spirv.size())
			break;
	}
	if (!valid)
	{
		std::vector<uint32>().swap(spirv);
		return false;
	}
	return true;
}

void StoreD3D11SpirvCache(const fs::path& path, const std::vector<uint32>& spirv)
{
	if (spirv.empty())
		return;
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	if (error)
		return;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (output)
		output.write(reinterpret_cast<const char*>(spirv.data()),
			static_cast<std::streamsize>(spirv.size() * sizeof(uint32)));
}
#endif

HRESULT CompileHLSLCached(const void* source, size_t sourceSize, const char* profile,
	UINT flags, ID3DBlob** bytecode, ID3DBlob** errors)
{
	if (!source || !sourceSize || !profile || !bytecode)
		return E_INVALIDARG;
	*bytecode = nullptr;
	if (errors)
		*errors = nullptr;

	// The cache key includes the compiler profile, flags and an explicit format
	// version so changes to the generated HLSL cannot reuse stale bytecode.
	uint64 hash = HashBytes(source, sourceSize);
	hash = HashBytes(profile, std::strlen(profile), hash);
	hash = HashBytes(&flags, sizeof(flags), hash);
	constexpr uint32 cacheVersion = 2;
	hash = HashBytes(&cacheVersion, sizeof(cacheVersion), hash);
	const fs::path directory = ActiveSettings::GetUserDataPath("shaderCache/driver/d3d11");
	const fs::path path = directory / fmt::format("{:016x}.dxbc", hash);

	static std::mutex cacheMutex;
	{
		std::lock_guard lock(cacheMutex);
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (input)
		{
			const std::streamoff length = static_cast<std::streamoff>(input.tellg());
			if (length >= 4 && length <= 64 * 1024 * 1024)
			{
				ComPtr<ID3DBlob> cached;
				if (SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(length), &cached)))
				{
					input.seekg(0);
					input.read(static_cast<char*>(cached->GetBufferPointer()),
						static_cast<std::streamsize>(length));
					ComPtr<ID3D11ShaderReflection> reflection;
					const bool validDxbc = input &&
						std::memcmp(cached->GetBufferPointer(), "DXBC", 4) == 0 &&
						SUCCEEDED(D3DReflect(cached->GetBufferPointer(), cached->GetBufferSize(),
							IID_PPV_ARGS(&reflection)));
					if (validDxbc)
					{
						*bytecode = cached.Detach();
						return S_OK;
					}
				}
			}
		}
	}

#if defined(CEMU_UWP)
	// Shader-cache workers can reach this function concurrently. xbsc_xs.dll has
	// a very large transient footprint, so overlapping cache misses can cross the
	// 5 GiB title cap even when every worker passed its initial memory check. Keep
	// cache hits parallel above, but serialize actual Xbox compilation and recheck
	// memory only after acquiring ownership.
	static std::mutex xboxCompilerMutex;
	std::lock_guard compilerLock(xboxCompilerMutex);
	HeapCompact(GetProcessHeap(), 0);
	// A cache hit above remains safe and avoids invoking the Xbox compiler. For
	// a cache miss, reserve ample space below the title cap for xbsc_xs.dll's
	// transient working set. Series S traces showed one compilation interval grow
	// PrivateUsage by well over 1 GiB before the compiler faulted.
	constexpr uint64 shaderCompilerStopBytes = 4096ull * 1024 * 1024;
	if (QueryProcessPrivateCommitBytes() >= shaderCompilerStopBytes)
	{
		OutputDebugStringA(
			"[Cemu/D3D11] HLSL compilation skipped to preserve Series S memory headroom\n");
		return E_OUTOFMEMORY;
	}
#endif

	ComPtr<ID3DBlob> compiled;
	ComPtr<ID3DBlob> compileErrors;
#if defined(CEMU_UWP)
	// Per-shader start/complete messages more than doubled shader-cache loading
	// time in debugger-attached Series S traces. Keep one policy line and compact
	// aggregate progress instead of formatting and flushing two lines per shader.
	static std::once_flag compilerPolicyLog;
	static uint64 compiledShaderCount{};
	static uint64 totalHlslBytes{};
	static uint64 totalDxbcBytes{};
	std::call_once(compilerPolicyLog, [flags]()
	{
		cemuLog_log(LogType::Force,
			"D3D11 Series S shader compiler policy: flags 0x{:X}; progress every 64 shaders",
			flags);
	});
#endif
	const HRESULT result = D3DCompile(source, sourceSize, nullptr, nullptr, nullptr,
		"main", profile, flags, 0, &compiled, &compileErrors);
	if (errors && compileErrors)
		*errors = compileErrors.Detach();
	if (FAILED(result))
		return result;
#if defined(CEMU_UWP)
	++compiledShaderCount;
	totalHlslBytes += sourceSize;
	totalDxbcBytes += compiled->GetBufferSize();
	if ((compiledShaderCount & 63) == 0)
	{
		cemuLog_log(LogType::Force,
			"D3D11 Series S shader compiler progress: {} shaders, HLSL {} KB, DXBC {} KB, process commit {} MB",
			compiledShaderCount, (totalHlslBytes + 1023) / 1024,
			(totalDxbcBytes + 1023) / 1024,
			QueryProcessPrivateCommitBytes() / (1024 * 1024));
	}
#endif

	{
		std::lock_guard lock(cacheMutex);
		std::error_code error;
		fs::create_directories(directory, error);
		if (!error)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (output)
				output.write(static_cast<const char*>(compiled->GetBufferPointer()),
					static_cast<std::streamsize>(compiled->GetBufferSize()));
		}
	}
	*bytecode = compiled.Detach();
	return S_OK;
}

D3D11_COMPARISON_FUNC CompareFunc(Latte::E_COMPAREFUNC value)
{
	static constexpr D3D11_COMPARISON_FUNC table[] = {
		D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL,
		D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER,
		D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL,
		D3D11_COMPARISON_ALWAYS
	};
	return table[static_cast<uint32>(value) & 7];
}

D3D11_STENCIL_OP StencilOp(Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION value)
{
	static constexpr D3D11_STENCIL_OP table[] = {
		D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE,
		D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
		D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR
	};
	return table[static_cast<uint32>(value) & 7];
}

D3D11_BLEND BlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR value)
{
	using F = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
	switch (value)
	{
	case F::BLEND_ZERO: return D3D11_BLEND_ZERO;
	case F::BLEND_ONE: return D3D11_BLEND_ONE;
	case F::BLEND_SRC_COLOR: return D3D11_BLEND_SRC_COLOR;
	case F::BLEND_ONE_MINUS_SRC_COLOR: return D3D11_BLEND_INV_SRC_COLOR;
	case F::BLEND_SRC_ALPHA:
	case F::BLEND_BOTH_SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
	case F::BLEND_ONE_MINUS_SRC_ALPHA:
	case F::BLEND_BOTH_INV_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
	case F::BLEND_DST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
	case F::BLEND_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
	case F::BLEND_DST_COLOR: return D3D11_BLEND_DEST_COLOR;
	case F::BLEND_ONE_MINUS_DST_COLOR: return D3D11_BLEND_INV_DEST_COLOR;
	case F::BLEND_SRC_ALPHA_SATURATE: return D3D11_BLEND_SRC_ALPHA_SAT;
	case F::BLEND_CONST_COLOR:
	case F::BLEND_CONST_ALPHA: return D3D11_BLEND_BLEND_FACTOR;
	case F::BLEND_ONE_MINUS_CONST_COLOR:
	case F::BLEND_ONE_MINUS_CONST_ALPHA: return D3D11_BLEND_INV_BLEND_FACTOR;
	case F::BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_COLOR;
	case F::BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_COLOR;
	case F::BLEND_SRC1_ALPHA: return D3D11_BLEND_SRC1_ALPHA;
	case F::BLEND_INV_SRC1_ALPHA: return D3D11_BLEND_INV_SRC1_ALPHA;
	default: return D3D11_BLEND_ONE;
	}
}

// D3D11 does not allow the color variants of the blend factors in the
// SrcBlendAlpha/DestBlendAlpha fields. GX2 (like OpenGL and Vulkan) uses one
// blend-factor enum for both equations, so factors such as SRC_COLOR are
// legal for the alpha equation and mean the alpha component of that source.
// Translate those factors explicitly instead of copying the RGB D3D11 enum.
D3D11_BLEND BlendFactorAlpha(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR value)
{
	using F = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
	switch (value)
	{
	case F::BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
	case F::BLEND_ONE_MINUS_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
	case F::BLEND_DST_COLOR: return D3D11_BLEND_DEST_ALPHA;
	case F::BLEND_ONE_MINUS_DST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
	case F::BLEND_SRC_ALPHA_SATURATE:
		// SRC_ALPHA_SATURATE is (f, f, f, 1); its alpha component is one.
		return D3D11_BLEND_ONE;
	case F::BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
	case F::BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
	default: return BlendFactor(value);
	}
}

D3D11_BLEND_OP BlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC value)
{
	using F = Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC;
	switch (value)
	{
	case F::DST_PLUS_SRC: return D3D11_BLEND_OP_ADD;
	case F::SRC_MINUS_DST: return D3D11_BLEND_OP_SUBTRACT;
	case F::MIN_DST_SRC: return D3D11_BLEND_OP_MIN;
	case F::MAX_DST_SRC: return D3D11_BLEND_OP_MAX;
	case F::DST_MINUS_SRC: return D3D11_BLEND_OP_REV_SUBTRACT;
	default: return D3D11_BLEND_OP_ADD;
	}
}

D3D11_LOGIC_OP LogicOp(Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP value)
{
	using F = Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP;
	switch (value)
	{
	case F::CLEAR: return D3D11_LOGIC_OP_CLEAR;
	case F::COPY: return D3D11_LOGIC_OP_COPY;
	case F::OR: return D3D11_LOGIC_OP_OR;
	case F::SET: return D3D11_LOGIC_OP_SET;
	default: return D3D11_LOGIC_OP_COPY;
	}
}

D3D11_TEXTURE_ADDRESS_MODE AddressMode(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP value)
{
	using C = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP;
	switch (value)
	{
	case C::WRAP: return D3D11_TEXTURE_ADDRESS_WRAP;
	case C::MIRROR: return D3D11_TEXTURE_ADDRESS_MIRROR;
	case C::CLAMP_LAST_TEXEL:
	case C::CLAMP_HALF_BORDER:
		return D3D11_TEXTURE_ADDRESS_CLAMP;
	case C::MIRROR_ONCE_LAST_TEXEL:
		return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
	case C::MIRROR_ONCE_HALF_BORDER:
	case C::CLAMP_BORDER:
	case C::MIRROR_ONCE_BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
	default: return D3D11_TEXTURE_ADDRESS_CLAMP;
	}
}

D3D11_FILTER SamplerFilter(const _LatteRegisterSetSampler& sampler, bool comparison,
	bool anisotropyEnabled)
{
	using XY = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
	using Z = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER;
	const auto minFilter = sampler.WORD0.get_XY_MIN_FILTER();
	const auto magFilter = sampler.WORD0.get_XY_MAG_FILTER();
	const auto mipFilter = sampler.WORD0.get_MIP_FILTER();
	if (anisotropyEnabled)
		return comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
	const bool minLinear = minFilter != XY::POINT && minFilter != XY::ANISO_POINT;
	const bool magLinear = magFilter != XY::POINT && magFilter != XY::ANISO_POINT;
	const bool mipLinear = mipFilter == Z::LINEAR;
	const uint32 filter = (minLinear ? 0x10u : 0u) | (magLinear ? 0x4u : 0u) |
		(mipLinear ? 0x1u : 0u) | (comparison ? 0x80u : 0u);
	return static_cast<D3D11_FILTER>(filter);
}

struct FormatInfo
{
	DXGI_FORMAT resource{};
	DXGI_FORMAT srv{};
	DXGI_FORMAT rtv{};
	DXGI_FORMAT dsv{};
	uint32 bytesPerBlock{ 4 };
	uint32 blockWidth{ 1 };
	uint32 blockHeight{ 1 };
	bool compressed{};
};

FormatInfo GetFormatInfo(Latte::E_GX2SURFFMT format, bool depth)
{
	using F = Latte::E_GX2SURFFMT;
	if (depth)
	{
		switch (format)
		{
		case F::D24_S8_UNORM:
			return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D24_UNORM_S8_UINT, 4 };
		case F::D24_S8_FLOAT:
			// DXGI has no 24-bit floating-point depth format. Match Vulkan's
			// fallback and decode it into a D32_FLOAT_S8X24 allocation.
			return { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 8 };
		case F::D16_UNORM:
			return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D16_UNORM, 2 };
		case F::D32_FLOAT:
			return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT, 4 };
		case F::D32_S8_FLOAT:
			return { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 8 };
		default:
			cemuLog_logOnce(LogType::Force,
				"D3D11 unsupported depth texture format 0x{:x}; using D32_FLOAT placeholder",
				static_cast<uint32>(format));
			return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT, 4 };
		}
	}
	switch (format)
	{
	case F::R8_UNORM: return { DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, {}, 1 };
	case F::R8_SNORM: return { DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_SNORM, {}, 1 };
	case F::R8_UINT: return { DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT, {}, 1 };
	case F::R8_SINT: return { DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8_SINT, {}, 1 };
	case F::R8_G8_UNORM: return { DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, {}, 2 };
	case F::R8_G8_SNORM: return { DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SNORM, {}, 2 };
	case F::R8_G8_UINT: return { DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT, {}, 2 };
	case F::R8_G8_SINT: return { DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8_SINT, {}, 2 };
	case F::R8_G8_B8_A8_UNORM: return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, {}, 4 };
	case F::R8_G8_B8_A8_SNORM: return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM, {}, 4 };
	case F::R8_G8_B8_A8_UINT: return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT, {}, 4 };
	case F::R8_G8_B8_A8_SINT: return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_SINT, {}, 4 };
	case F::R8_G8_B8_A8_SRGB: return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {}, 4 };
	case F::R16_UNORM: return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, {}, 2 };
	case F::R16_SNORM: return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_SNORM, {}, 2 };
	case F::R16_UINT: return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT, {}, 2 };
	case F::R16_SINT: return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_SINT, {}, 2 };
	case F::R16_FLOAT: return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, {}, 2 };
	case F::R16_G16_UNORM: return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UNORM, {}, 4 };
	case F::R16_G16_SNORM: return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SNORM, {}, 4 };
	case F::R16_G16_UINT: return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_UINT, {}, 4 };
	case F::R16_G16_SINT: return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, {}, 4 };
	case F::R16_G16_FLOAT: return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, {}, 4 };
	case F::R16_G16_B16_A16_UNORM: return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, {}, 8 };
	case F::R16_G16_B16_A16_SNORM: return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, {}, 8 };
	case F::R16_G16_B16_A16_UINT: return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UINT, {}, 8 };
	case F::R16_G16_B16_A16_SINT: return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_SINT, {}, 8 };
	case F::R16_G16_B16_A16_FLOAT: return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, {}, 8 };
	case F::R32_UINT: return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, {}, 4 };
	case F::R32_SINT: return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_SINT, {}, 4 };
	case F::R32_FLOAT: return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, {}, 4 };
	case F::R32_G32_UINT: return { DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT, {}, 8 };
	case F::R32_G32_SINT: return { DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_SINT, {}, 8 };
	case F::R32_G32_FLOAT: return { DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, {}, 8 };
	case F::R32_G32_B32_A32_UINT: return { DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, {}, 16 };
	case F::R32_G32_B32_A32_SINT: return { DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_SINT, {}, 16 };
	case F::R32_G32_B32_A32_FLOAT: return { DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, {}, 16 };
	case F::R10_G10_B10_A2_UNORM:
	case F::R10_G10_B10_A2_SRGB:
		return { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, {}, 4 };
	// DXGI has no ABGR10A2 view. Expand and reorder it exactly like the
	// OpenGL fallback instead of interpreting ABGR bits as RGBA.
	case F::A2_B10_G10_R10_UNORM:
		return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
			DXGI_FORMAT_R16G16B16A16_UNORM, {}, 8 };
	case F::R10_G10_B10_A2_UINT:
	case F::A2_B10_G10_R10_UINT:
		return { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R10G10B10A2_UINT, {}, 4 };
	// SNORM 10:10:10:2 has no usable DXGI equivalent. The decoder expands it
	// to RGBA16_SNORM, as the Vulkan backend does.
	case F::R10_G10_B10_A2_SNORM:
		return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, {}, 8 };
	case F::R11_G11_B10_FLOAT: return { DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, {}, 4 };
	case F::BC1_UNORM: return { DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, {}, 8, 4, 4, true };
	case F::BC1_SRGB: return { DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, {}, 8, 4, 4, true };
	case F::BC2_UNORM: return { DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	case F::BC2_SRGB: return { DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	case F::BC3_UNORM: return { DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	case F::BC3_SRGB: return { DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	case F::BC4_UNORM: return { DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN, {}, 8, 4, 4, true };
	case F::BC4_SNORM: return { DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_UNKNOWN, {}, 8, 4, 4, true };
	case F::BC5_UNORM: return { DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	case F::BC5_SNORM: return { DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_UNKNOWN, {}, 16, 4, 4, true };
	// These packed GX2 formats are deliberately expanded by the decoder.
	case F::R4_G4_UNORM:
	case F::R5_G6_B5_UNORM:
	case F::R5_G5_B5_A1_UNORM:
	case F::R4_G4_B4_A4_UNORM:
	case F::A1_B5_G5_R5_UNORM:
		return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM, {}, 4 };
	case F::X24_G8_UINT:
		return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT,
			DXGI_FORMAT_R8G8B8A8_UINT, {}, 4 };
	case F::R24_X8_UNORM:
	case F::R24_X8_FLOAT:
	case F::R32_X8_FLOAT:
		return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, {}, 4 };
	default:
		cemuLog_logOnce(LogType::Force,
			"D3D11 unsupported color texture format 0x{:x}; using RGBA8 placeholder",
			static_cast<uint32>(format));
		return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM, {}, 4 };
	}
}

uint32 RowPitch(const FormatInfo& info, uint32 width)
{
	return ((width + info.blockWidth - 1) / info.blockWidth) * info.bytesPerBlock;
}

uint32 RowCount(const FormatInfo& info, uint32 height)
{
	return (height + info.blockHeight - 1) / info.blockHeight;
}

uint32 AdjustTextureComponentSelector(Latte::E_GX2SURFFMT format, uint32 selector)
{
	using F = Latte::E_GX2SURFFMT;
	switch (format)
	{
	case F::R8_UNORM:
	case F::R8_SNORM:
	case F::BC4_UNORM:
	case F::BC4_SNORM:
		if (selector >= 1 && selector <= 3)
			selector = 0;
		break;
	case F::A1_B5_G5_R5_UNORM:
	case F::A2_B10_G10_R10_UNORM:
		if (selector <= 3)
			selector = 3 - selector;
		break;
	case F::BC5_UNORM:
	case F::BC5_SNORM:
		if (selector == 3)
			selector = 1;
		break;
	case F::X24_G8_UINT:
		if (selector <= 3)
			selector = 3;
		break;
	case F::R4_G4_UNORM:
		if (selector == 0)
			selector = 1;
		else if (selector == 1)
			selector = 0;
		break;
	default:
		break;
	}
	return selector <= 5 ? selector : 4;
}

class D3D11Shader final : public RendererShader
{
public:
	D3D11Shader(ID3D11Device* device, ShaderType type, uint64 baseHash, uint64 auxHash,
		bool isGfxPack, const std::string& source)
		: RendererShader(type, baseHash, auxHash, true, isGfxPack)
	{
		Compile(device, source);
		ReleaseTransientBytecode();
	}
	D3D11Shader(ID3D11Device* device, ShaderType type, uint64 baseHash, uint64 auxHash,
		bool isGfxPack, const std::string& source, bool sourceIsHlsl)
		: RendererShader(type, baseHash, auxHash, true, isGfxPack)
	{
		if (sourceIsHlsl)
			CompileHLSL(device, source);
		else
			Compile(device, source);
		ReleaseTransientBytecode();
	}

	void PreponeCompilation(bool) override {}
	bool IsCompiled() override { return m_compiled; }
	bool WaitForCompiled() override { return m_compiled; }
	ID3D11VertexShader* Vertex() const { return m_vs.Get(); }
	ID3D11PixelShader* Pixel() const { return m_ps.Get(); }
	ID3D11GeometryShader* Geometry() const { return m_gs.Get(); }
	ID3D11GeometryShader* StreamoutGeometry() const { return m_streamoutGs.Get(); }
	ID3DBlob* Bytecode() const { return m_bytecode.Get(); }
	uint64 BaseHash() const { return m_baseHash; }
	uint64 AuxHash() const { return m_auxHash; }
	UINT TextureSlot(UINT originalBinding) const
	{
		return originalBinding < m_textureSlots.size() ? m_textureSlots[originalBinding] : InvalidSlot;
	}
	UINT UniformSlot(UINT originalBinding) const
	{
		return originalBinding < m_uniformSlots.size() ? m_uniformSlots[originalBinding] : InvalidSlot;
	}
	UINT SamplerSwizzleSlot() const { return m_samplerSwizzleSlot; }
	static constexpr UINT InvalidSlot = UINT_MAX;

private:
	void ReleaseTransientBytecode()
	{
		// Only vertex bytecode is needed later by CreateInputLayout. Pixel and
		// geometry shaders already own their native code after creation.
		if (GetType() != ShaderType::kVertex)
			m_bytecode.Reset();
	}

	struct HlslTextureResource
	{
		std::string name;
		std::string valueType;
		UINT slot{};
	};

	static bool IsHlslIdentifier(char value)
	{
		return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
	}

	static size_t FindMatchingParenthesis(const std::string& source, size_t opening)
	{
		uint32 depth{};
		for (size_t i = opening; i < source.size(); ++i)
		{
			if (source[i] == '(')
				++depth;
			else if (source[i] == ')' && --depth == 0)
				return i;
		}
		return std::string::npos;
	}

	static std::string AddRuntimeSamplerSwizzles(std::string hlsl, UINT constantBufferSlot,
		bool& patched)
	{
		if (constantBufferSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
			return hlsl;
		std::vector<HlslTextureResource> textures;
		size_t lineStart{};
		while (lineStart < hlsl.size())
		{
			const size_t lineEnd = hlsl.find('\n', lineStart);
			const size_t end = lineEnd == std::string::npos ? hlsl.size() : lineEnd;
			const std::string_view line(hlsl.data() + lineStart, end - lineStart);
			const size_t registerPos = line.find("register(t");
			const size_t colonPos = line.find(':');
			if (registerPos != std::string_view::npos && colonPos != std::string_view::npos)
			{
				const size_t slotBegin = registerPos + 10;
				const size_t slotEnd = line.find(')', slotBegin);
				size_t nameEnd = colonPos;
				while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1])))
					--nameEnd;
				size_t nameBegin = nameEnd;
				while (nameBegin > 0 && IsHlslIdentifier(line[nameBegin - 1]))
					--nameBegin;
				if (slotEnd != std::string_view::npos && nameBegin != nameEnd)
				{
					const UINT slot = static_cast<UINT>(std::strtoul(
						std::string(line.substr(slotBegin, slotEnd - slotBegin)).c_str(), nullptr, 10));
					std::string valueType = "float4";
					if (line.find("<uint4>") != std::string_view::npos)
						valueType = "uint4";
					else if (line.find("<int4>") != std::string_view::npos)
						valueType = "int4";
					textures.push_back({ std::string(line.substr(nameBegin, nameEnd - nameBegin)),
						std::move(valueType), slot });
				}
			}
			lineStart = lineEnd == std::string::npos ? hlsl.size() : lineEnd + 1;
		}

		struct Replacement
		{
			size_t begin{};
			size_t end{};
			UINT slot{};
			char suffix{};
		};
		std::vector<Replacement> replacements;
		static constexpr std::array<std::string_view, 10> methods = {
			"Sample(", "SampleBias(", "SampleLevel(", "SampleGrad(", "Load(",
			"Gather(", "GatherRed(", "GatherGreen(", "GatherBlue(", "GatherAlpha("
		};
		for (const auto& texture : textures)
		{
			if (texture.slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
				continue;
			for (const auto method : methods)
			{
				const std::string needle = texture.name + "." + std::string(method);
				size_t position{};
				while ((position = hlsl.find(needle, position)) != std::string::npos)
				{
					const size_t opening = position + needle.size() - 1;
					const size_t closing = FindMatchingParenthesis(hlsl, opening);
					if (closing == std::string::npos)
						break;
					replacements.push_back({ position, closing + 1, texture.slot,
						texture.valueType == "uint4" ? 'U' : texture.valueType == "int4" ? 'I' : 'F' });
					position = closing + 1;
				}
			}
		}
		if (replacements.empty())
			return hlsl;
		std::sort(replacements.begin(), replacements.end(),
			[](const Replacement& left, const Replacement& right) { return left.begin > right.begin; });
		for (const auto& replacement : replacements)
		{
			hlsl.insert(replacement.end,
				fmt::format(", cemuSamplerSwizzle[{}])", replacement.slot));
			hlsl.insert(replacement.begin,
				fmt::format("CemuApplySamplerSwizzle{}(", replacement.suffix));
		}

		hlsl.insert(0, fmt::format(R"HLSL(
cbuffer CemuSamplerSwizzleBuffer : register(b{})
{{
    uint4 cemuSamplerSwizzle[16];
}};
float CemuSwizzleComponentF(float4 v, uint s) {{ return s < 4 ? v[s] : (s == 5 ? 1.0f : 0.0f); }}
int CemuSwizzleComponentI(int4 v, uint s) {{ return s < 4 ? v[s] : (s == 5 ? 1 : 0); }}
uint CemuSwizzleComponentU(uint4 v, uint s) {{ return s < 4 ? v[s] : (s == 5 ? 1u : 0u); }}
float CemuApplySamplerSwizzleF(float v, uint4 s) {{ return CemuSwizzleComponentF(float4(v,0,0,1),s.x); }}
float2 CemuApplySamplerSwizzleF(float2 v, uint4 s) {{ float4 q=float4(v,0,1); return float2(CemuSwizzleComponentF(q,s.x),CemuSwizzleComponentF(q,s.y)); }}
float3 CemuApplySamplerSwizzleF(float3 v, uint4 s) {{ float4 q=float4(v,1); return float3(CemuSwizzleComponentF(q,s.x),CemuSwizzleComponentF(q,s.y),CemuSwizzleComponentF(q,s.z)); }}
float4 CemuApplySamplerSwizzleF(float4 v, uint4 s) {{ return float4(CemuSwizzleComponentF(v,s.x),CemuSwizzleComponentF(v,s.y),CemuSwizzleComponentF(v,s.z),CemuSwizzleComponentF(v,s.w)); }}
int CemuApplySamplerSwizzleI(int v, uint4 s) {{ return CemuSwizzleComponentI(int4(v,0,0,1),s.x); }}
int2 CemuApplySamplerSwizzleI(int2 v, uint4 s) {{ int4 q=int4(v,0,1); return int2(CemuSwizzleComponentI(q,s.x),CemuSwizzleComponentI(q,s.y)); }}
int3 CemuApplySamplerSwizzleI(int3 v, uint4 s) {{ int4 q=int4(v,1); return int3(CemuSwizzleComponentI(q,s.x),CemuSwizzleComponentI(q,s.y),CemuSwizzleComponentI(q,s.z)); }}
int4 CemuApplySamplerSwizzleI(int4 v, uint4 s) {{ return int4(CemuSwizzleComponentI(v,s.x),CemuSwizzleComponentI(v,s.y),CemuSwizzleComponentI(v,s.z),CemuSwizzleComponentI(v,s.w)); }}
uint CemuApplySamplerSwizzleU(uint v, uint4 s) {{ return CemuSwizzleComponentU(uint4(v,0,0,1),s.x); }}
uint2 CemuApplySamplerSwizzleU(uint2 v, uint4 s) {{ uint4 q=uint4(v,0,1); return uint2(CemuSwizzleComponentU(q,s.x),CemuSwizzleComponentU(q,s.y)); }}
uint3 CemuApplySamplerSwizzleU(uint3 v, uint4 s) {{ uint4 q=uint4(v,1); return uint3(CemuSwizzleComponentU(q,s.x),CemuSwizzleComponentU(q,s.y),CemuSwizzleComponentU(q,s.z)); }}
uint4 CemuApplySamplerSwizzleU(uint4 v, uint4 s) {{ return uint4(CemuSwizzleComponentU(v,s.x),CemuSwizzleComponentU(v,s.y),CemuSwizzleComponentU(v,s.z),CemuSwizzleComponentU(v,s.w)); }}
)HLSL", constantBufferSlot));
		patched = true;
		return hlsl;
	}

	static void ReplaceToken(std::string& text, std::string_view from, std::string_view to)
	{
		size_t position{};
		while ((position = text.find(from, position)) != std::string::npos)
		{
			const bool leftBoundary = position == 0 || !IsHlslIdentifier(text[position - 1]);
			const size_t right = position + from.size();
			const bool rightBoundary = right == text.size() || !IsHlslIdentifier(text[right]);
			if (leftBoundary && rightBoundary)
			{
				text.replace(position, from.size(), to);
				position += to.size();
			}
			else
				position = right;
		}
	}

	static std::string GeometrySourceType(std::string type)
	{
		static constexpr std::array<std::pair<std::string_view, std::string_view>, 15> types = {{
			{ "vec2", "float2" }, { "vec3", "float3" }, { "vec4", "float4" },
			{ "ivec2", "int2" }, { "ivec3", "int3" }, { "ivec4", "int4" },
			{ "uvec2", "uint2" }, { "uvec3", "uint3" }, { "uvec4", "uint4" },
			{ "bvec2", "bool2" }, { "bvec3", "bool3" }, { "bvec4", "bool4" },
			{ "mat2", "float2x2" }, { "mat3", "float3x3" }, { "mat4", "float4x4" }
		}};
		for (const auto& [glsl, hlsl] : types)
			if (type == glsl)
				return std::string(hlsl);
		return type;
	}

	bool CompileGeneratedGeometryShader(ID3D11Device* device, const std::string& source)
	{
		// The text translator does not model Cemu's macro-based streamout blocks.
		// Leaving those declarations in the generated HLSL makes an otherwise
		// recoverable shader fail before the reflected compatibility path runs.
		if (source.find("XFB_BLOCK_LAYOUT(") != std::string::npos)
			return false;
		const size_t inputMarker = source.find("V2G_LAYOUT in ");
		const size_t inputOpen = inputMarker == std::string::npos ? std::string::npos :
			source.find('{', inputMarker);
		const size_t inputClose = inputOpen == std::string::npos ? std::string::npos :
			source.find('}', inputOpen);
		const size_t mainPosition = source.find("void main()", inputClose);
		if (inputOpen == std::string::npos || inputClose == std::string::npos ||
			mainPosition == std::string::npos)
			return false;

		struct SourceField { std::string type; std::string name; uint32 location{}; };
		std::vector<SourceField> inputs;
		std::vector<SourceField> outputs;
		auto parseDeclarations = [](std::string_view block, std::vector<SourceField>& fields,
			uint32 firstLocation)
		{
			size_t cursor{};
			uint32 location = firstLocation;
			while (cursor < block.size())
			{
				const size_t semicolon = block.find(';', cursor);
				if (semicolon == std::string_view::npos)
					break;
				std::string line(block.substr(cursor, semicolon - cursor));
				const size_t first = line.find_first_not_of(" \t\r\n");
				const size_t last = line.find_last_not_of(" \t\r\n");
				if (first != std::string::npos)
				{
					line = line.substr(first, last - first + 1);
					const size_t space = line.find_last_of(" \t");
					if (space != std::string::npos)
					{
						std::string type = line.substr(0, line.find_first_of(" \t"));
						std::string name = line.substr(space + 1);
						if (!type.empty() && !name.empty())
							fields.push_back({ GeometrySourceType(std::move(type)), std::move(name), location++ });
					}
				}
				cursor = semicolon + 1;
			}
		};
		parseDeclarations(std::string_view(source).substr(inputOpen + 1,
			inputClose - inputOpen - 1), inputs, 0);
		if (inputs.empty())
			return false;

		size_t scan = inputClose;
		const size_t inputSemicolon = source.find(';', inputClose);
		size_t generatedBodyStart = inputSemicolon == std::string::npos ? inputClose + 1 : inputSemicolon + 1;
		while ((scan = source.find("layout(location", scan)) != std::string::npos && scan < mainPosition)
		{
			const size_t equal = source.find('=', scan);
			const size_t close = source.find(')', equal);
			const size_t out = source.find("out ", close);
			const size_t semicolon = source.find(';', out);
			if (equal == std::string::npos || close == std::string::npos || out == std::string::npos ||
				semicolon == std::string::npos || semicolon > mainPosition)
				break;
			const uint32 location = static_cast<uint32>(std::strtoul(
				source.substr(equal + 1, close - equal - 1).c_str(), nullptr, 10));
			const size_t typeBegin = out + 4;
			const size_t typeEnd = source.find_first_of(" \t", typeBegin);
			const size_t nameBegin = source.find_first_not_of(" \t", typeEnd);
			outputs.push_back({ GeometrySourceType(source.substr(typeBegin, typeEnd - typeBegin)),
				source.substr(nameBegin, semicolon - nameBegin), location });
			generatedBodyStart = (std::max)(generatedBodyStart, semicolon + 1);
			scan = semicolon + 1;
		}

		const char* inputPrimitive = source.find("layout(points) in") != std::string::npos ? "point" :
			source.find("layout(lines) in") != std::string::npos ? "line" :
			source.find("layout(lines_adjacency) in") != std::string::npos ? "lineadj" :
			source.find("layout(triangles_adjacency) in") != std::string::npos ? "triangleadj" : "triangle";
		const char* streamType = source.find("layout (line_strip") != std::string::npos ? "LineStream" :
			source.find("layout (points") != std::string::npos ? "PointStream" : "TriangleStream";
		uint32 maxVertices = 3;
		const size_t maxMarker = source.find("max_vertices=");
		if (maxMarker != std::string::npos)
			maxVertices = static_cast<uint32>(std::strtoul(source.c_str() + maxMarker + 13, nullptr, 10));

		std::string hlsl;
		struct GeometryTexture { std::string name; std::string dimension; std::string valueType; UINT slot{}; };
		std::vector<GeometryTexture> geometryTextures;
		size_t textureCursor{};
		while ((textureCursor = source.find("uniform ", textureCursor)) != std::string::npos &&
			textureCursor < inputMarker)
		{
			const size_t typeBegin = textureCursor + 8;
			const size_t typeEnd = source.find_first_of(" \t", typeBegin);
			if (typeEnd == std::string::npos)
				break;
			const std::string samplerType = source.substr(typeBegin, typeEnd - typeBegin);
			if (samplerType.find("sampler") == std::string::npos)
			{
				textureCursor = typeEnd;
				continue;
			}
			const size_t nameBegin = source.find_first_not_of(" \t", typeEnd);
			const size_t semicolon = source.find(';', nameBegin);
			const size_t layoutBegin = source.rfind("TEXTURE_LAYOUT(", textureCursor);
			const size_t layoutEnd = layoutBegin == std::string::npos ? std::string::npos :
				source.find(')', layoutBegin);
			if (nameBegin == std::string::npos || semicolon == std::string::npos ||
				layoutBegin == std::string::npos || layoutEnd > textureCursor)
			{
				textureCursor = typeEnd;
				continue;
			}
			const size_t lastComma = source.rfind(',', layoutEnd);
			if (lastComma == std::string::npos || lastComma < layoutBegin)
			{
				textureCursor = semicolon + 1;
				continue;
			}
			const UINT originalBinding = static_cast<UINT>(std::strtoul(
				source.c_str() + lastComma + 1, nullptr, 10));
			const UINT slot = TextureSlot(originalBinding);
			if (slot == InvalidSlot)
			{
				textureCursor = semicolon + 1;
				continue;
			}
			std::string dimension = samplerType.find("Cube") != std::string::npos ? "TextureCube" :
				samplerType.find("3D") != std::string::npos ? "Texture3D" :
				samplerType.find("1D") != std::string::npos ? "Texture1D" : "Texture2D";
			std::string valueType = samplerType.starts_with('u') ? "uint4" :
				samplerType.starts_with('i') ? "int4" : "float4";
			geometryTextures.push_back({ source.substr(nameBegin, semicolon - nameBegin),
				std::move(dimension), std::move(valueType), slot });
			textureCursor = semicolon + 1;
		}
		for (const auto& texture : geometryTextures)
			hlsl += fmt::format("{}<{}> {} : register(t{});\nSamplerState {}Sampler : register(s{});\n",
				texture.dimension, texture.valueType, texture.name, texture.slot, texture.name, texture.slot);
		std::string uniformHlsl;
		std::unordered_set<std::string> uniformNames;
		size_t uniformCursor{};
		while ((uniformCursor = source.find("uniform ", uniformCursor)) != std::string::npos &&
			uniformCursor < inputMarker)
		{
			const size_t semicolon = source.find(';', uniformCursor);
			const size_t open = source.find('{', uniformCursor);
			if (semicolon == std::string::npos || (open != std::string::npos && open < semicolon))
			{
				uniformCursor += 8;
				continue;
			}
			std::string declaration = source.substr(uniformCursor + 8,
				semicolon - uniformCursor - 8);
			const size_t space = declaration.find_first_of(" \t");
			if (space != std::string::npos && declaration.substr(0, space).find("sampler") == std::string::npos)
			{
				std::string type = GeometrySourceType(declaration.substr(0, space));
				std::string name = declaration.substr(declaration.find_first_not_of(" \t", space));
				const size_t array = name.find('[');
				const std::string key = name.substr(0, array);
				if (uniformNames.emplace(key).second)
					uniformHlsl += fmt::format("    {} {};\n", type, name);
			}
			uniformCursor = semicolon + 1;
		}
		if (!uniformHlsl.empty())
		{
			UINT uniformSlot{};
			const auto reflectedSlot = std::find_if(m_uniformSlots.begin(), m_uniformSlots.end(),
				[](UINT slot) { return slot != InvalidSlot; });
			if (reflectedSlot != m_uniformSlots.end())
				uniformSlot = *reflectedSlot;
			hlsl += fmt::format("cbuffer CemuGeometryUniforms : register(b{})\n{{\n", uniformSlot) +
				uniformHlsl + "};\n";
		}
		hlsl += "struct GeometryInput\n{\n";
		for (const auto& field : inputs)
			hlsl += fmt::format("    {} {} : TEXCOORD{};\n", field.type, field.name, field.location);
		hlsl += "};\nstruct GeometryOutput\n{\n    float4 position : SV_Position;\n";
		for (const auto& field : outputs)
			hlsl += fmt::format("    {} {} : TEXCOORD{};\n", field.type, field.name, field.location);
		hlsl += "};\nvoid CemuSetPosition(inout float4 target, float4 value) { target=value; target.z=(target.z+target.w)*0.5f; }\n";

		std::string body = source.substr(generatedBodyStart);
		body.erase(0, body.find_first_not_of(" \t\r\n"));
		static constexpr std::array<std::pair<std::string_view, std::string_view>, 23> replacements = {{
			{ "floatBitsToInt", "asint" }, { "floatBitsToUint", "asuint" },
			{ "intBitsToFloat", "asfloat" }, { "uintBitsToFloat", "asfloat" },
			{ "fract", "frac" }, { "mix", "lerp" }, { "inversesqrt", "rsqrt" },
			{ "dFdx", "ddx" }, { "dFdy", "ddy" }, { "mod", "fmod" },
			{ "vec2", "float2" }, { "vec3", "float3" }, { "vec4", "float4" },
			{ "ivec2", "int2" }, { "ivec3", "int3" }, { "ivec4", "int4" },
			{ "uvec2", "uint2" }, { "uvec3", "uint3" }, { "uvec4", "uint4" },
			{ "bvec2", "bool2" }, { "bvec3", "bool3" }, { "bvec4", "bool4" },
			{ "roundEven", "round" }
		}};
		for (const auto& [glsl, hlslName] : replacements)
			ReplaceToken(body, glsl, hlslName);
		ReplaceToken(body, "v2g", "inputVertices");
		for (const auto& output : outputs)
			ReplaceToken(body, output.name, "cemuOutput." + output.name);
		for (const auto& texture : geometryTextures)
		{
			const auto rewriteTextureCall = [&](std::string_view glslName, std::string_view hlslName)
			{
				const std::string needle = std::string(glslName) + "(" + texture.name + ",";
				size_t position{};
				while ((position = body.find(needle, position)) != std::string::npos)
				{
					body.replace(position, needle.size(),
						texture.name + "." + std::string(hlslName) + "(" + texture.name + "Sampler,");
					position += texture.name.size() + hlslName.size() + texture.name.size() + 11;
				}
			};
			rewriteTextureCall("texture", "Sample");
			rewriteTextureCall("textureLod", "SampleLevel");
			rewriteTextureCall("textureGrad", "SampleGrad");
			rewriteTextureCall("textureGather", "Gather");
		}
		ReplaceToken(body, "SET_POSITION", "CemuSetPosition");
		size_t setPosition{};
		while ((setPosition = body.find("CemuSetPosition(", setPosition)) != std::string::npos)
		{
			body.insert(setPosition + 16, "cemuOutput.position, ");
			setPosition += 37;
		}
		while ((scan = body.find("EmitVertex();")) != std::string::npos)
			body.replace(scan, 13, "outputStream.Append(cemuOutput);");
		while ((scan = body.find("EndPrimitive();")) != std::string::npos)
			body.replace(scan, 15, "outputStream.RestartStrip();");
		const std::string signature = fmt::format(
			"[maxvertexcount({})]\nvoid main({} GeometryInput inputVertices[{}], inout {}<GeometryOutput> outputStream)",
			maxVertices, inputPrimitive,
			std::string_view(inputPrimitive) == "point" ? 1 : std::string_view(inputPrimitive) == "line" ? 2 :
			std::string_view(inputPrimitive) == "lineadj" ? 4 : std::string_view(inputPrimitive) == "triangleadj" ? 6 : 3,
			streamType);
		body.replace(body.find("void main()"), 11, signature);
		const size_t mainBrace = body.find('{', body.find(signature));
		body.insert(mainBrace + 1, "\nGeometryOutput cemuOutput = (GeometryOutput)0;");
		hlsl += body;
		UINT samplerSwizzleSlot{};
		for (UINT slot : m_uniformSlots)
			if (slot != InvalidSlot)
				samplerSwizzleSlot = (std::max)(samplerSwizzleSlot, slot + 1);
		hlsl = AddRuntimeSamplerSwizzles(std::move(hlsl), samplerSwizzleSlot,
			m_usesRuntimeSwizzle);
		if (m_usesRuntimeSwizzle)
			m_samplerSwizzleSlot = samplerSwizzleSlot;

		CompileHLSL(device, hlsl, true);
		if (m_compiled)
			cemuLog_logOnce(LogType::Force,
				"D3D11 native geometry shader {:016x}_{:016x}: translated automatically from Cemu GLSL",
				m_baseHash, m_auxHash);
		return m_compiled;
	}

	struct GeometryInterfaceField
	{
		std::string name;
		std::string type;
		std::string semantic;
		std::string interpolation;
	};

	static void DeduplicateGeometryInterface(std::vector<GeometryInterfaceField>& fields)
	{
		std::unordered_set<std::string> semantics;
		fields.erase(std::remove_if(fields.begin(), fields.end(),
			[&](const GeometryInterfaceField& field) {
				return !semantics.emplace(field.semantic).second;
			}), fields.end());
	}

	static std::string GeometryHlslType(const spirv_cross::SPIRType& spirType)
	{
		const char* baseType = "float";
		switch (spirType.basetype)
		{
		case spirv_cross::SPIRType::Int: baseType = "int"; break;
		case spirv_cross::SPIRType::UInt: baseType = "uint"; break;
		// D3D signatures have no boolean component type. SPIR-V boolean
		// interface values are represented as 32-bit integers at stage boundaries.
		case spirv_cross::SPIRType::Boolean: baseType = "uint"; break;
		default: break;
		}
		const uint32 componentCount = (std::max)(1u, spirType.vecsize);
		return componentCount == 1 ? std::string(baseType) :
			fmt::format("{}{}", baseType, componentCount);
	}

	static std::string GeometryInterpolation(bool flat, bool noPerspective,
		bool centroid, bool sample, const spirv_cross::SPIRType& type)
	{
		// Integer varyings cannot be interpolated by D3D11. GLSL/SPIR-V normally
		// decorate them Flat, but keep the generated HLSL legal even when an old
		// shader-cache entry omitted that decoration.
		if (flat || type.basetype == spirv_cross::SPIRType::Int ||
			type.basetype == spirv_cross::SPIRType::UInt ||
			type.basetype == spirv_cross::SPIRType::Boolean)
			return "nointerpolation ";
		if (noPerspective)
			return "noperspective ";
		if (sample)
			return "sample ";
		if (centroid)
			return "centroid ";
		return {};
	}

	static std::string GeometryAssignmentExpression(const GeometryInterfaceField& source,
		const GeometryInterfaceField& destination, const std::string& expression)
	{
		if (source.type == destination.type)
			return expression;
		// Latte's ring interface commonly transports floating-point varyings in
		// integer registers. SPIR-V retains those bit-pattern types at the native
		// GS boundary, while HLSL requires the VS and GS signatures to agree. Keep
		// the bits intact instead of applying a numeric conversion.
		if (destination.type.starts_with("float") &&
			(source.type.starts_with("int") || source.type.starts_with("uint")))
			return fmt::format("asfloat({})", expression);
		if (destination.type.starts_with("int") &&
			(source.type.starts_with("float") || source.type.starts_with("uint")))
			return fmt::format("asint({})", expression);
		if (destination.type.starts_with("uint") &&
			(source.type.starts_with("float") || source.type.starts_with("int")))
			return fmt::format("asuint({})", expression);
		return {};
	}

	static void AppendGeometryInterface(std::vector<GeometryInterfaceField>& fields,
		spirv_cross::CompilerHLSL& compiler, const spirv_cross::Resource& resource,
		bool output)
	{
		const auto& resourceType = compiler.get_type(resource.base_type_id);
		if (resourceType.basetype == spirv_cross::SPIRType::Struct)
		{
			uint32 nextLocation = compiler.has_decoration(resource.id, spv::DecorationLocation) ?
				compiler.get_decoration(resource.id, spv::DecorationLocation) : 0;
			for (uint32 memberIndex = 0; memberIndex < resourceType.member_types.size(); ++memberIndex)
			{
				const auto& memberType = compiler.get_type(resourceType.member_types[memberIndex]);
				if (compiler.has_member_decoration(resourceType.self, memberIndex, spv::DecorationBuiltIn))
				{
					const auto builtin = static_cast<spv::BuiltIn>(compiler.get_member_decoration(
						resourceType.self, memberIndex, spv::DecorationBuiltIn));
					if (builtin == spv::BuiltInPosition)
						fields.push_back({ "position", "float4", "SV_Position", {} });
					else if (output && builtin == spv::BuiltInLayer)
						fields.push_back({ "layer", "uint", "SV_RenderTargetArrayIndex", "nointerpolation " });
					else if (output && builtin == spv::BuiltInPrimitiveId)
						fields.push_back({ "primitiveId", "uint", "SV_PrimitiveID", "nointerpolation " });
					continue;
				}

				const uint32 location = compiler.has_member_decoration(
					resourceType.self, memberIndex, spv::DecorationLocation) ?
					compiler.get_member_decoration(resourceType.self, memberIndex, spv::DecorationLocation) :
					nextLocation;
				// Each matrix column occupies a separate interface location. Arrays of
				// interface values do as well. Emit every occupied location rather than
				// merely reserving it; otherwise the following stage sees an incomplete
				// signature even though reflection itself succeeded.
				uint32 occupiedLocations = (std::max)(1u, memberType.columns);
				for (uint32 dimension : memberType.array)
					occupiedLocations *= (std::max)(1u, dimension);
				const std::string interpolation = GeometryInterpolation(
					compiler.has_member_decoration(resourceType.self, memberIndex, spv::DecorationFlat),
					compiler.has_member_decoration(resourceType.self, memberIndex, spv::DecorationNoPerspective),
					compiler.has_member_decoration(resourceType.self, memberIndex, spv::DecorationCentroid),
					compiler.has_member_decoration(resourceType.self, memberIndex, spv::DecorationSample),
					memberType);
				for (uint32 occupied = 0; occupied < occupiedLocations; ++occupied)
				{
					const uint32 fieldLocation = location + occupied;
					fields.push_back({ fmt::format("attribute{}", fieldLocation),
						GeometryHlslType(memberType), fmt::format("TEXCOORD{}", fieldLocation),
						interpolation });
				}
				nextLocation = location + occupiedLocations;
			}
			return;
		}

		GeometryInterfaceField field{};
		field.type = GeometryHlslType(resourceType);
		if (compiler.has_decoration(resource.id, spv::DecorationBuiltIn))
		{
			const auto builtin = static_cast<spv::BuiltIn>(
				compiler.get_decoration(resource.id, spv::DecorationBuiltIn));
			switch (builtin)
			{
			case spv::BuiltInPosition:
				field.name = "position";
				field.type = "float4";
				field.semantic = "SV_Position";
				break;
			case spv::BuiltInLayer:
				if (!output)
					return;
				field.name = "layer";
				field.type = "uint";
				field.semantic = "SV_RenderTargetArrayIndex";
				break;
			case spv::BuiltInPrimitiveId:
				if (!output)
					return;
				field.name = "primitiveId";
				field.type = "uint";
				field.semantic = "SV_PrimitiveID";
				break;
			case spv::BuiltInPointSize:
				// D3D11 shader model 5 has no point-size output semantic.
				return;
			default:
				return;
			}
		}
		else
		{
			const uint32 location = compiler.get_decoration(resource.id, spv::DecorationLocation);
			field.name = fmt::format("attribute{}", location);
			field.semantic = fmt::format("TEXCOORD{}", location);
			field.interpolation = GeometryInterpolation(
				compiler.has_decoration(resource.id, spv::DecorationFlat),
				compiler.has_decoration(resource.id, spv::DecorationNoPerspective),
				compiler.has_decoration(resource.id, spv::DecorationCentroid),
				compiler.has_decoration(resource.id, spv::DecorationSample), resourceType);
		}
		fields.emplace_back(std::move(field));
	}

	bool CompileGeometryCompatibilityShader(ID3D11Device* device,
		spirv_cross::CompilerHLSL& compiler, const spirv_cross::ShaderResources& resources)
	{
		std::vector<GeometryInterfaceField> inputs;
		std::vector<GeometryInterfaceField> outputs;
		for (const auto& resource : resources.stage_inputs)
			AppendGeometryInterface(inputs, compiler, resource, false);
		for (const auto& resource : resources.stage_outputs)
			AppendGeometryInterface(outputs, compiler, resource, true);
		// A malformed or aliased interface must not become duplicate HLSL
		// semantics, which D3D rejects even if SPIR-V reflection accepted it.
		DeduplicateGeometryInterface(inputs);
		DeduplicateGeometryInterface(outputs);
		const auto hasPosition = [](const std::vector<GeometryInterfaceField>& fields) {
			return std::any_of(fields.begin(), fields.end(), [](const GeometryInterfaceField& field) {
				return field.semantic == "SV_Position";
			});
		};
		if (!hasPosition(outputs))
			outputs.push_back({ "position", "float4", "SV_Position", {} });

		const auto& modes = compiler.get_execution_mode_bitset();
		const char* inputPrimitive = "triangle";
		uint32 inputVertices = 3;
		if (modes.get(spv::ExecutionModeInputPoints))
		{
			inputPrimitive = "point";
			inputVertices = 1;
		}
		else if (modes.get(spv::ExecutionModeInputLines))
		{
			inputPrimitive = "line";
			inputVertices = 2;
		}
		else if (modes.get(spv::ExecutionModeInputLinesAdjacency))
		{
			inputPrimitive = "lineadj";
			inputVertices = 4;
		}
		else if (modes.get(spv::ExecutionModeInputTrianglesAdjacency))
		{
			inputPrimitive = "triangleadj";
			inputVertices = 6;
		}

		const char* streamType = "TriangleStream";
		if (modes.get(spv::ExecutionModeOutputPoints))
			streamType = "PointStream";
		else if (modes.get(spv::ExecutionModeOutputLineStrip))
			streamType = "LineStream";
		uint32 outputVertices = compiler.get_execution_mode_argument(spv::ExecutionModeOutputVertices);
		if (outputVertices == 0)
			outputVertices = inputVertices;
		const uint32 copiedVertices = (std::min)(inputVertices, outputVertices);

		std::string hlsl = "struct GeometryInput\n{\n";
		for (const auto& field : inputs)
			hlsl += fmt::format("    {}{} {} : {};\n", field.interpolation,
				field.type, field.name, field.semantic);
		hlsl += "};\nstruct GeometryOutput\n{\n";
		for (const auto& field : outputs)
			hlsl += fmt::format("    {}{} {} : {};\n", field.interpolation,
				field.type, field.name, field.semantic);
		hlsl += fmt::format(
			"}};\n[maxvertexcount({})]\nvoid main({} GeometryInput vertices[{}], "
			"inout {}<GeometryOutput> outputStream)\n{{\n",
			copiedVertices, inputPrimitive, inputVertices, streamType);
		hlsl += fmt::format("    [unroll] for (uint vertexIndex = 0; vertexIndex < {}; ++vertexIndex)\n    {{\n", copiedVertices);
		hlsl += "        GeometryOutput result = (GeometryOutput)0;\n";
		for (const auto& output : outputs)
		{
			auto input = std::find_if(inputs.begin(), inputs.end(),
				[&](const GeometryInterfaceField& candidate) {
					return candidate.semantic == output.semantic;
				});
			// A Latte VS feeding a native GS exports ring parameters rather than
			// SV_Position. Requiring SV_Position on the GS input makes D3D11 reject
			// the VS-GS linkage. For the compatibility path, source the mandatory
			// rasterizer position from the first reflected float4 ring parameter.
			if (input == inputs.end() && output.semantic == "SV_Position")
			{
				input = std::find_if(inputs.begin(), inputs.end(),
					[](const GeometryInterfaceField& candidate) {
						return candidate.type == "float4" || candidate.type == "int4" ||
							candidate.type == "uint4";
					});
			}
			if (input != inputs.end())
			{
				const std::string expression = GeometryAssignmentExpression(*input, output,
					fmt::format("vertices[vertexIndex].{}", input->name));
				if (!expression.empty())
					hlsl += fmt::format("        result.{} = {};\n", output.name, expression);
			}
		}
		hlsl += "        outputStream.Append(result);\n    }\n    outputStream.RestartStrip();\n}\n";

		cemuLog_logOnce(LogType::Force,
			"D3D11 native geometry shader {:016x}_{:016x}: SPIRV-Cross has no HLSL geometry-stage backend; using reflected topology-preserving compatibility shader",
			m_baseHash, m_auxHash);
		CompileHLSL(device, hlsl, true);
		return m_compiled;
	}

	bool CompileKnownGeometryShader(ID3D11Device* device)
	{
		// Super Mario Maker's native geometry shader expands one Latte point
		// into a rotated four-vertex sprite. A topology-only passthrough keeps
		// D3D11 linkage valid, but destroys glyphs and UI rectangles because the
		// original position and texture-coordinate calculations never run.
		if (m_baseHash != 0xbcc4e8625638b961ull)
			return false;

		static constexpr const char* hlsl = R"HLSL(
cbuffer UfBlock : register(b0)
{
    int4 uf_remappedGS[2];
    int uf_verticesPerInstance;
};

struct GeometryInput
{
    int4 parameter0 : TEXCOORD0;
    int4 parameter1 : TEXCOORD1;
    int4 parameter2 : TEXCOORD2;
    int4 parameter3 : TEXCOORD3;
    int4 parameter4 : TEXCOORD4;
    int4 parameter5 : TEXCOORD5;
    int4 parameter6 : TEXCOORD6;
};

struct GeometryOutput
{
    float4 parameter0 : TEXCOORD0;
    float4 position : SV_Position;
};

float2 TransformOffset(float2 offset)
{
    const float4 value = float4(offset, 0.0f, 1.0f);
    return float2(dot(value, asfloat(uf_remappedGS[0])),
                  dot(value, asfloat(uf_remappedGS[1])));
}

GeometryOutput MakeVertex(float4 center, float2 offset, float2 texCoord)
{
    GeometryOutput result = (GeometryOutput)0;
    result.parameter0 = float4(texCoord, 0.0f, 0.0f);
    result.position = center;
    result.position.xy += TransformOffset(offset);
    // The Latte GLSL SET_POSITION path targets Vulkan's 0..w depth range.
    // D3D11 uses the same clip-depth convention.
    result.position.z = (result.position.z + result.position.w) * 0.5f;
    return result;
}

[maxvertexcount(4)]
void main(point GeometryInput inputVertices[1],
          inout TriangleStream<GeometryOutput> outputStream)
{
    const GeometryInput input = inputVertices[0];
    const float4 center = asfloat(input.parameter0);
    const float2 anchor = asfloat(input.parameter1.xy);
    const float2 extent = asfloat(input.parameter2.xy);
    const float angle = asfloat(input.parameter3.w);
    const float2 scale = asfloat(input.parameter4.xy);
    const float4 tex = asfloat(input.parameter5);
    const float extra = (scale.x != 1.0f || scale.y != 1.0f) ?
        asfloat(input.parameter6.x) : 0.0f;

    // Preserve the Latte shader's periodic angle normalization while avoiding
    // the integer bit-cast register machine used by the generated GLSL.
    const float normalizedAngle = frac(angle * 0.1591549367f + 0.5f) *
        6.2831854820f - 3.1415927410f;
    const float cosine = cos(normalizedAngle);
    const float sine = sin(normalizedAngle);
    const float sx = scale.x * 8.0f;
    const float sy = scale.y * 8.0f;

    const float2 topLeftTex = float2(
        -tex.z - extra - tex.x + anchor.x + extent.x,
        -tex.w + extra + tex.y + anchor.y - extent.y);
    const float2 topRightTex = float2(
        -tex.z + extra + tex.x + anchor.x - extent.x,
        topLeftTex.y);
    const float2 bottomLeftTex = float2(
        topLeftTex.x,
        -tex.w - extra - tex.y + anchor.y + extent.y);
    const float2 bottomRightTex = float2(topRightTex.x, bottomLeftTex.y);

    outputStream.Append(MakeVertex(center,
        float2(sx * cosine - sy * sine, sx * sine + sy * cosine),
        topLeftTex));
    outputStream.Append(MakeVertex(center,
        float2(-sx * cosine - sy * sine, -sx * sine + sy * cosine),
        topRightTex));
    outputStream.Append(MakeVertex(center,
        float2(sx * cosine + sy * sine, sx * sine - sy * cosine),
        bottomLeftTex));
    outputStream.Append(MakeVertex(center,
        float2(-sx * cosine + sy * sine, -sx * sine - sy * cosine),
        bottomRightTex));
    outputStream.RestartStrip();
}
)HLSL";

		CompileHLSL(device, hlsl, true);
		if (m_compiled)
		{
			cemuLog_logOnce(LogType::Force,
				"D3D11 native geometry shader {:016x}_{:016x}: using exact point-sprite HLSL translation",
				m_baseHash, m_auxHash);
		}
		return m_compiled;
	}

	void DumpUnsupportedGeometrySource(const std::string& source, const std::vector<uint32>& spirv) const
	{
		std::error_code error;
		const fs::path directory = ActiveSettings::GetUserDataPath("dump/shaders");
		fs::create_directories(directory, error);
		if (error)
		{
			cemuLog_log(LogType::Force,
				"D3D11 could not create geometry shader dump directory: {}", error.message());
			return;
		}
		const std::string stem = fmt::format("{:016x}_{:016x}_gs", m_baseHash, m_auxHash);
		const auto writeDump = [&](const fs::path& path, const void* data, size_t size)
		{
			error.clear();
			if (fs::exists(path, error) && !error)
				return true;
			if (size > static_cast<size_t>(std::numeric_limits<sint32>::max()))
				return false;
			FileStream* file = FileStream::createFile2(path);
			if (!file)
				return false;
			const bool written = file->writeData(data, static_cast<sint32>(size)) == static_cast<sint32>(size);
			delete file;
			return written;
		};

		const bool glslWritten = writeDump(directory / (stem + ".glsl"), source.data(), source.size());
		const bool spirvWritten = writeDump(directory / (stem + ".spv"), spirv.data(), spirv.size() * sizeof(uint32));
		if (glslWritten && spirvWritten)
			cemuLog_log(LogType::Force,
				"D3D11 dumped unsupported native geometry shader {:016x}_{:016x} as GLSL and SPIR-V to LocalState/dump/shaders",
				m_baseHash, m_auxHash);
		else
			cemuLog_log(LogType::Force,
				"D3D11 could not completely dump native geometry shader {:016x}_{:016x}",
				m_baseHash, m_auxHash);
	}

	void CompileHLSL(ID3D11Device* device, const std::string& source,
		bool preserveReflectedBindings = false)
	{
		try
		{
			if (!preserveReflectedBindings)
			{
				m_textureSlots.fill(InvalidSlot);
				m_uniformSlots.fill(InvalidSlot);
			}
			if (GetType() != ShaderType::kGeometry)
				throw std::runtime_error("direct HLSL compilation is only used for D3D11 geometry helpers");
			ComPtr<ID3DBlob> errors;
			const HRESULT compileResult = CompileHLSLCached(source.data(), source.size(), "gs_5_0",
				RuntimeShaderCompileFlags(),
				&m_bytecode, &errors);
			if (FAILED(compileResult))
			{
				const char* message = errors ?
					static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error";
				throw std::runtime_error(message);
			}
			D3D11_DRIVER_TRACE(fmt::format("CreateGeometryShader {:016x}_{:016x} bytecode={}",
				m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
			const HRESULT createResult = device->CreateGeometryShader(m_bytecode->GetBufferPointer(),
				m_bytecode->GetBufferSize(), nullptr, &m_gs);
			ThrowIfFailed(createResult, "CreateGeometryShader");
			m_compiled = true;
		}
		catch (const std::bad_alloc&)
		{
			m_compiled = false;
			OutputDebugStringA("[Cemu/D3D11] HLSL shader creation deferred: out of memory\n");
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D11 HLSL shader {:016x}_{:016x} failed: {}",
				m_baseHash, m_auxHash, ex.what());
		}
	}

	void Compile(ID3D11Device* device, const std::string& source)
	{
		try
		{
			m_textureSlots.fill(InvalidSlot);
			m_uniformSlots.fill(InvalidSlot);
			std::string hlsl;
			UINT uniformSlot{};
			const char* profile = GetType() == ShaderType::kVertex ? "vs_5_0" :
				GetType() == ShaderType::kFragment ? "ps_5_0" : "gs_5_0";
			std::vector<uint32> spirv;
#if defined(CEMU_UWP)
			const fs::path spirvCachePath = GetD3D11SpirvCachePath(
				source, static_cast<uint32>(GetType()));
			const bool loadedCachedSpirv = LoadD3D11SpirvCache(spirvCachePath, spirv);
#else
			constexpr bool loadedCachedSpirv = false;
#endif
			// Match Vulkan's intermediate cache and keep glslang in its own lifetime
			// scope. A cached module skips this allocation-heavy stage entirely.
			if (!loadedCachedSpirv)
			{
				EShLanguage stage = GetType() == ShaderType::kVertex ? EShLangVertex :
					GetType() == ShaderType::kFragment ? EShLangFragment : EShLangGeometry;
				glslang::TShader shader(stage);
				const char* text = source.c_str();
				shader.setStrings(&text, 1);
				// EShClientVulkan already provides the VULKAN macro. Defining it in
				// the preamble as well makes recent glslang versions reject every
				// shader because the built-in macro uses a different substitution.
				shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
				shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
				shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
				const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
				if (!shader.parse(GetDefaultResources(), 450, false, messages))
					throw std::runtime_error(shader.getInfoLog());
				glslang::TProgram program;
				program.addShader(&shader);
				if (!program.link(messages) || !program.mapIO())
					throw std::runtime_error(program.getInfoLog());
				glslang::SpvOptions spvOptions;
				// Match the proven Vulkan path: reduce the IR before SPIRV-Cross emits
				// HLSL, lowering the amount of code handed to the Xbox compiler.
				spvOptions.disableOptimizer = false;
				spvOptions.validate = false;
				spvOptions.optimizeSize = true;
				glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spvOptions);
#if defined(CEMU_UWP)
				StoreD3D11SpirvCache(spirvCachePath, spirv);
#endif
			}

			// Release glslang before SPIRV-Cross builds its separate IR graph.
			{
			spirv_cross::CompilerHLSL compiler(spirv);
			auto resources = compiler.get_shader_resources();
			const auto executionModel = compiler.get_execution_model();
#if defined(CEMU_UWP)
			// Native transform feedback is disabled on Series S below, but the SPIR-V
			// still contains its extra stage outputs. Leaving those exports in the
			// ordinary raster HLSL is enough for xbsc to reject the native function.
			// XFB locations are allocated after all regular stage outputs by the
			// decompiler, so filter only that trailing interface range. Position and
			// every Wii U varying used by the next stage remain enabled.
			UINT firstXfbLocation = UINT_MAX;
			size_t xfbCursor{};
			while ((xfbCursor = source.find("XFB_BLOCK_LAYOUT(", xfbCursor)) != std::string::npos)
			{
				UINT slot{}, stride{}, location{};
				if (std::sscanf(source.c_str() + xfbCursor,
					"XFB_BLOCK_LAYOUT(%u, %u, %u)", &slot, &stride, &location) == 3)
					firstXfbLocation = (std::min)(firstXfbLocation, location);
				xfbCursor += 17;
			}
			if (firstXfbLocation != UINT_MAX)
			{
				auto activeInterfaces = compiler.get_active_interface_variables();
				for (const auto& output : resources.stage_outputs)
				{
					if (compiler.has_decoration(output.id, spv::DecorationLocation) &&
						compiler.get_decoration(output.id, spv::DecorationLocation) >= firstXfbLocation)
						activeInterfaces.erase(output.id);
				}
				compiler.set_enabled_interface_variables(std::move(activeInterfaces));
			}
#endif
			const auto descriptorCount = [&](const spirv_cross::Resource& resource)
			{
				const auto& type = compiler.get_type(resource.type_id);
				UINT count = 1;
				for (uint32 dimension : type.array)
				{
					if (dimension == 0 || count > UINT_MAX / dimension)
						throw std::runtime_error("runtime-sized or oversized descriptor arrays are unsupported by D3D11");
					count *= dimension;
				}
				return count;
			};
			UINT textureSlot{};
			for (const auto& resource : resources.sampled_images)
			{
				const UINT originalBinding =
					compiler.get_decoration(resource.id, spv::DecorationBinding);
				const UINT count = descriptorCount(resource);
				if (count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - textureSlot)
					throw std::runtime_error("shader requires more than 16 D3D11 samplers");
				spirv_cross::HLSLResourceBinding binding{};
				binding.stage = executionModel;
				binding.desc_set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
				binding.binding = originalBinding;
				binding.srv.register_binding = textureSlot;
				binding.sampler.register_binding = textureSlot;
				compiler.add_hlsl_resource_binding(binding);
				if (originalBinding < m_textureSlots.size())
					m_textureSlots[originalBinding] = textureSlot;
				textureSlot += count;
			}
			for (const auto& resource : resources.uniform_buffers)
			{
				const UINT originalBinding =
					compiler.get_decoration(resource.id, spv::DecorationBinding);
				const UINT count = descriptorCount(resource);
				if (count > D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - uniformSlot)
					throw std::runtime_error("shader requires more than 14 D3D11 constant buffers");
				spirv_cross::HLSLResourceBinding binding{};
				binding.stage = executionModel;
				binding.desc_set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
				binding.binding = originalBinding;
				binding.cbv.register_binding = uniformSlot;
				compiler.add_hlsl_resource_binding(binding);
				if (originalBinding < m_uniformSlots.size())
					m_uniformSlots[originalBinding] = uniformSlot;
				uniformSlot += count;
			}
			// SPIRV-Cross' HLSL backend recognizes geometry-stage reflection but
			// cannot emit its entry point or stream operations. Calling compile()
			// therefore throws CompilerError("Unsupported shader stage"). Keep the
			// D3D11 pipeline valid with a reflected topology/interface preserving
			// shader instead of losing the stage (or stopping in the debugger).
			if (executionModel == spv::ExecutionModelGeometry)
			{
				DumpUnsupportedGeometrySource(source, spirv);
				if (CompileKnownGeometryShader(device))
				{
					if (!CreateStreamoutShader(device, source))
						m_compiled = false;
					return;
				}
				bool generatedGeometryCompiled = false;
				try
				{
					generatedGeometryCompiled = CompileGeneratedGeometryShader(device, source);
				}
				catch (const std::bad_alloc&)
				{
					throw;
				}
				catch (const std::exception& ex)
				{
					// This translator recognizes common decompiler output by syntax. A
					// new construct must fall through to reflection, not poison the shader.
					cemuLog_logOnce(LogType::Force,
						"D3D11 generated geometry translation {:016x}_{:016x} skipped: {}",
						m_baseHash, m_auxHash, ex.what());
				}
				if (generatedGeometryCompiled)
				{
					if (!CreateStreamoutShader(device, source))
						m_compiled = false;
					return;
				}
				CompileGeometryCompatibilityShader(device, compiler, resources);
				if (m_compiled && !CreateStreamoutShader(device, source))
					m_compiled = false;
				return;
			}
			auto options = compiler.get_hlsl_options();
			options.shader_model = 50;
			options.point_coord_compat = true;
			options.point_size_compat = true;
			// Preserve the SPIR-V name for stages supported by the HLSL backend.
			options.use_entry_point_name = true;
			compiler.set_hlsl_options(options);
			hlsl = compiler.compile();
			hlsl = AddRuntimeSamplerSwizzles(std::move(hlsl), uniformSlot, m_usesRuntimeSwizzle);
			if (m_usesRuntimeSwizzle)
				m_samplerSwizzleSlot = uniformSlot;
			}
			std::vector<uint32>().swap(spirv);

#if defined(CEMU_UWP)
			// The translation scope above has released glslang/SPIRV-Cross. Give the
			// allocator a chance to return their transient regions before xbsc_xs.dll
			// begins its own large allocation phase.
			HeapCompact(GetProcessHeap(), 0);
#endif
			ComPtr<ID3DBlob> errors;
			HRESULT hr = CompileHLSLCached(hlsl.data(), hlsl.size(), profile,
				RuntimeShaderCompileFlags(),
				&m_bytecode, &errors);
			if (FAILED(hr))
			{
				const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error";
				throw std::runtime_error(message);
			}
			if (GetType() == ShaderType::kVertex)
			{
				D3D11_DRIVER_TRACE(fmt::format("CreateVertexShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				const HRESULT createResult = device->CreateVertexShader(m_bytecode->GetBufferPointer(),
					m_bytecode->GetBufferSize(), nullptr, &m_vs);
				ThrowIfFailed(createResult, "CreateVertexShader");
			}
			else if (GetType() == ShaderType::kFragment)
			{
				D3D11_DRIVER_TRACE(fmt::format("CreatePixelShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				const HRESULT createResult = device->CreatePixelShader(m_bytecode->GetBufferPointer(),
					m_bytecode->GetBufferSize(), nullptr, &m_ps);
				ThrowIfFailed(createResult, "CreatePixelShader");
			}
			else
			{
				D3D11_DRIVER_TRACE(fmt::format("CreateGeometryShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				const HRESULT createResult = device->CreateGeometryShader(m_bytecode->GetBufferPointer(),
					m_bytecode->GetBufferSize(), nullptr, &m_gs);
				ThrowIfFailed(createResult, "CreateGeometryShader");
			}
			if (!CreateStreamoutShader(device, source))
			{
				m_compiled = false;
				return;
			}
			m_compiled = true;
		}
		catch (const std::bad_alloc&)
		{
			// On Xbox, D3D11On12, glslang and SPIRV-Cross share the title's
			// constrained memory budget. Do not invoke the allocating logger here.
			// The common cache will reject this uncompiled shader safely.
			OutputDebugStringA("[Cemu/D3D11] Shader compilation skipped: out of memory\n");
			m_compiled = false;
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D11 shader {:016x}_{:016x} failed: {}", m_baseHash, m_auxHash, ex.what());
		}
	}

	bool CreateStreamoutShader(ID3D11Device* device, const std::string& source)
	{
		struct XfbBlock
		{
			UINT slot{};
			UINT stride{};
			UINT location{};
		};
		std::vector<XfbBlock> blocks;
		size_t cursor{};
		while ((cursor = source.find("XFB_BLOCK_LAYOUT(", cursor)) != std::string::npos)
		{
			XfbBlock block{};
			if (std::sscanf(source.c_str() + cursor, "XFB_BLOCK_LAYOUT(%u, %u, %u)",
				&block.slot, &block.stride, &block.location) == 3 &&
				block.slot < D3D11_SO_BUFFER_SLOT_COUNT && block.stride &&
				block.stride <= D3D11_SO_BUFFER_MAX_STRIDE_IN_BYTES &&
				(block.stride % sizeof(uint32)) == 0)
				blocks.emplace_back(block);
			cursor += 17;
		}
		if (blocks.empty())
			return source.find("XFB_BLOCK_LAYOUT(") == std::string::npos;
		if (!m_bytecode)
			return false;

#if defined(CEMU_UWP)
		// newbe_xs/xbsc accepts the D3D11 stream-output object but aborts while
		// materializing its native function on Series S, which irreversibly removes
		// the D3D11On12 device. Keep the ordinary raster shader compiled and let the
		// renderer skip only native transform feedback. streamout_begin() clears all
		// pending SO ranges when this object is absent, preventing stale GPU data from
		// being copied into a later vertex range.
		cemuLog_logOnce(LogType::Force,
			"D3D11 Series S stream-output compatibility: native transform feedback disabled");
		return true;
#endif

		ComPtr<ID3D11ShaderReflection> reflection;
		if (FAILED(D3DReflect(m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(),
			IID_PPV_ARGS(&reflection))))
			return false;
		D3D11_SHADER_DESC shaderDesc{};
		if (FAILED(reflection->GetDesc(&shaderDesc)))
			return false;
		std::vector<D3D11_SIGNATURE_PARAMETER_DESC> outputs(shaderDesc.OutputParameters);
		for (UINT i = 0; i < shaderDesc.OutputParameters; ++i)
			reflection->GetOutputParameterDesc(i, &outputs[i]);

		std::vector<D3D11_SO_DECLARATION_ENTRY> declarations;
		std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> strides{};
		std::array<bool, D3D11_SO_BUFFER_SLOT_COUNT> usedSlots{};
		UINT strideCount{};
		for (const auto& block : blocks)
		{
			if (usedSlots[block.slot])
				return false;
			usedSlots[block.slot] = true;
			strides[block.slot] = block.stride;
			strideCount = (std::max)(strideCount, block.slot + 1);
			const UINT scalarCount = block.stride / sizeof(uint32);
			if (scalarCount > D3D11_SO_OUTPUT_COMPONENT_COUNT)
				return false;
			for (UINT scalar = 0; scalar < scalarCount; ++scalar)
			{
				// One XFB array element represents exactly one 32-bit value. The
				// reflected signature can expose a wider register mask when the HLSL
				// compiler packs neighbouring outputs together; using the population
				// count of that mask here made a 4-byte SO stride consume up to 16
				// bytes. D3D11On12's Xbox compiler accepts CreateGeometryShaderWith-
				// StreamOutput initially, then rejects that inconsistent native
				// function on first use and removes the device.
				const UINT semanticIndex = block.location + scalar;
				auto it = std::find_if(outputs.begin(), outputs.end(), [semanticIndex](const auto& output) {
					return output.SemanticName && _stricmp(output.SemanticName, "TEXCOORD") == 0 &&
						output.SemanticIndex == semanticIndex;
				});
				if (it == outputs.end())
					return false;
				const BYTE componentMask = it->Mask & 0x0F;
				if (componentMask == 0)
					return false;
				BYTE startComponent{};
				while ((componentMask & (1u << startComponent)) == 0)
					++startComponent;
				declarations.push_back({ 0, it->SemanticName, it->SemanticIndex,
					startComponent, 1,
					static_cast<BYTE>(block.slot) });
			}
		}
		if (declarations.empty() ||
			declarations.size() > D3D11_SO_STREAM_COUNT * D3D11_SO_OUTPUT_COMPONENT_COUNT)
			return false;
		const HRESULT hr = device->CreateGeometryShaderWithStreamOutput(
			m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(),
			declarations.data(), static_cast<UINT>(declarations.size()),
			strides.data(), strideCount, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &m_streamoutGs);
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force, "D3D11 stream-output shader creation failed (0x{:08X})",
				static_cast<uint32>(hr));
			return false;
		}
		return true;
	}

	bool m_compiled{};
	ComPtr<ID3DBlob> m_bytecode;
	ComPtr<ID3D11VertexShader> m_vs;
	ComPtr<ID3D11PixelShader> m_ps;
	ComPtr<ID3D11GeometryShader> m_gs;
	ComPtr<ID3D11GeometryShader> m_streamoutGs;
	std::array<UINT, 256> m_textureSlots{};
	std::array<UINT, 256> m_uniformSlots{};
	UINT m_samplerSwizzleSlot{ InvalidSlot };
	bool m_usesRuntimeSwizzle{};
};

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
	void CopyAliasToBase();
	bool IsIncompatibleAlias() const { return m_incompatibleAlias; }
private:
	bool CreateIncompatibleAlias(const FormatInfo& requested);
	bool CopySubresourcesRaw(ID3D11Resource* source, DXGI_FORMAT sourceFormat,
		UINT sourceFirstMip, UINT sourceFirstSlice, UINT sourceMipLevels,
		ID3D11Resource* destination,
		UINT destinationFirstMip, UINT destinationFirstSlice, UINT destinationMipLevels);
	ComPtr<ID3D11ShaderResourceView> m_srv;
	ComPtr<ID3D11RenderTargetView> m_rtv;
	ComPtr<ID3D11DepthStencilView> m_dsv;
	DXGI_FORMAT m_rtvFormat{ DXGI_FORMAT_UNKNOWN };
	ComPtr<ID3D11Resource> m_aliasResource;
	bool m_incompatibleAlias{};
	UINT m_aliasSliceCount{ 1 };
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
			ComPtr<ID3D11Texture2D> staging;
			if (FAILED(texture->Owner()->GetDevice()->CreateTexture2D(&stagingDesc, nullptr, &staging)))
				return false;
			context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source,
				D3D11CalcSubresource(sourceFirstMip + mip, sourceFirstSlice + slice,
					sourceMipLevels), nullptr);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;
			context->UpdateSubresource(destination,
				D3D11CalcSubresource(destinationFirstMip + mip, destinationFirstSlice + slice,
					destinationMipLevels), nullptr, mapped.pData, mapped.RowPitch, mapped.DepthPitch);
			context->Unmap(staging.Get(), 0);
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

void D3D11TextureView::CopyAliasToBase()
{
	if (!m_incompatibleAlias)
		return;
	auto* texture = static_cast<D3D11Texture*>(baseTexture);
	ComPtr<ID3D11Texture2D> aliasTexture;
	if (FAILED(m_aliasResource.As(&aliasTexture)))
		return;
	D3D11_TEXTURE2D_DESC aliasDesc{};
	aliasTexture->GetDesc(&aliasDesc);
	CopySubresourcesRaw(m_aliasResource.Get(), aliasDesc.Format, 0, 0, aliasDesc.MipLevels,
		texture->Resource(), firstMip, firstSlice,
		texture->EffectiveMipLevels());
}

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
		if (!texture->Texture2D())
			throw std::runtime_error("D3D11 readback currently requires a 2D texture view");
		D3D11_TEXTURE2D_DESC source{};
		texture->Texture2D()->GetDesc(&source);
		source.Width = (std::max)(1u, source.Width >> m_view->firstMip);
		source.Height = (std::max)(1u, source.Height >> m_view->firstMip);
		source.MipLevels = 1;
		source.ArraySize = 1;
		source.Usage = D3D11_USAGE_STAGING;
		source.BindFlags = 0;
		source.MiscFlags = 0;
		source.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ThrowIfFailed(m_renderer->GetDevice()->CreateTexture2D(&source, nullptr, &m_staging), "Create readback texture");
		const UINT subresource = D3D11CalcSubresource(m_view->firstMip, m_view->firstSlice,
			texture->EffectiveMipLevels());
		m_renderer->GetContext()->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, texture->Resource(), subresource, nullptr);
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
		while (m_started && m_renderer->GetContext()->GetData(m_event.Get(), nullptr, 0, 0) == S_FALSE) {}
	}
	uint8* GetData() override
	{
		if (!m_started)
			StartTransfer();
		ForceFinish();
		D3D11_TEXTURE2D_DESC desc{};
		m_staging->GetDesc(&desc);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		ThrowIfFailed(m_renderer->GetContext()->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map readback texture");
		const FormatInfo info = GetFormatInfo(m_view->format, m_view->baseTexture->isDepth);
		const uint32 rowSize = RowPitch(info, desc.Width);
		const uint32 rows = RowCount(info, desc.Height);
		m_data.resize(static_cast<size_t>(rowSize) * rows);
		for (uint32 row = 0; row < rows; ++row)
			std::memcpy(m_data.data() + static_cast<size_t>(row) * rowSize,
				static_cast<const uint8*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, rowSize);
		m_renderer->GetContext()->Unmap(m_staging.Get(), 0);
		return m_data.data();
	}
private:
	D3D11Renderer* m_renderer;
	D3D11TextureView* m_view;
	bool m_started{};
	std::vector<uint8> m_data;
	ComPtr<ID3D11Texture2D> m_staging;
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
	m_device.As(&m_device1);
	m_context = static_cast<ID3D11DeviceContext*>(surface->immediate_context);
	m_context.As(&m_context1);
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
}

D3D11Renderer::~D3D11Renderer() = default;

D3D11Renderer* D3D11Renderer::GetInstance()
{
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
		"float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t0.Sample(s0,uv);}";
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
	glslang::InitializeProcess();
	InitializePresentationPipeline();
	Renderer::Initialize();
	// Renderer::Initialize() creates the TV context first and the pad context
	// second, leaving the pad context current. Dear ImGui stores the DX11
	// backend data per context, while the embedded D3D11 surface renders the
	// main/TV view. Initialize the backend on the context that ImguiBegin(true)
	// will actually use.
	ImGui::SetCurrentContext(imguiTVContext);
	m_imguiInitialized = ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
}

void D3D11Renderer::Shutdown()
{
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
		const float color[4]{ 0, 0, 0, 1 };
		m_context->ClearRenderTargetView(m_backBufferView.Get(), color);
	}
}

void D3D11Renderer::DrawEmptyFrame(bool mainWindow)
{
	if (BeginFrame(mainWindow))
	{
		ClearColorbuffer(false);
		SwapBuffers(true, false);
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

void D3D11Renderer::DrawBackbufferQuad(LatteTextureView* textureView, RendererOutputShader*, bool useLinear,
	sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	if (textureView)
		static_cast<D3D11TextureView*>(textureView)->PrepareForSampling();
	if (padView || !textureView || !BeginFrame(true))
		return;
	// Preserve the complete Wii U image. Scale it as far as the current
	// SwapChainPanel permits, then center the remaining letterbox/pillarbox
	// space. Unlike "cover" scaling this never cuts HUD elements or menus.
	D3D11_TEXTURE2D_DESC backBufferDesc{};
	m_backBuffer->GetDesc(&backBufferDesc);
	const sint32 backBufferWidth = static_cast<sint32>((std::max)(backBufferDesc.Width, 1u));
	const sint32 backBufferHeight = static_cast<sint32>((std::max)(backBufferDesc.Height, 1u));
	const double containScale = (std::min)(
		static_cast<double>(backBufferWidth) / static_cast<double>((std::max)(imageWidth, 1)),
		static_cast<double>(backBufferHeight) / static_cast<double>((std::max)(imageHeight, 1)));
	const sint32 containedWidth = (std::max)(
		static_cast<sint32>(std::lround(imageWidth * containScale)), 1);
	const sint32 containedHeight = (std::max)(
		static_cast<sint32>(std::lround(imageHeight * containScale)), 1);
	imageX = (backBufferWidth - containedWidth) / 2;
	imageY = (backBufferHeight - containedHeight) / 2;
	imageWidth = containedWidth;
	imageHeight = containedHeight;
	// Always clear the full buffer because the contained viewport can change
	// after a resize. Otherwise pixels from the previous, larger viewport stay
	// visible in the newly exposed bars.
	ClearColorbuffer(false);
	auto* view = static_cast<D3D11TextureView*>(textureView);
	ID3D11ShaderResourceView* srv = view->SRV();
	ID3D11SamplerState* sampler = useLinear ? m_presentSampler.Get() : m_presentPointSampler.Get();
	const float blendFactor[4]{};
	m_context->RSSetState(m_rasterizerState.Get());
	m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
	m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
	m_context->VSSetShader(m_presentVS.Get(), nullptr, 0);
	m_context->PSSetShader(m_presentPS.Get(), nullptr, 0);
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

void D3D11Renderer::CheckMemoryPressure()
{
#if defined(CEMU_UWP)
	// Sampling every 10 presents catches rapidly growing title workloads while
	// keeping the accounting overhead negligible. On
	// Xbox, PrivateUsage already reaches the 5120 MiB title cap and includes the
	// driver-backed commitment. Adding DXGI CurrentUsage double-counts a large
	// portion of graphics memory, so use it only as diagnostic information.
	// Ten-frame sampling is sufficient in the normal state. Once the soft limit
	// has been crossed, enforce retirement every frame until hysteresis clears;
	// the Series S log showed in-flight commitment grow from 2817 to 5099 MiB
	// while the old guard waited between effective reclamation opportunities.
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
		}
		return;
	}

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
	ComPtr<IDXGIDevice3> dxgiDevice3;
	if (SUCCEEDED(m_device.As(&dxgiDevice3)))
		dxgiDevice3->Trim();
	HeapCompact(GetProcessHeap(), 0);
	const uint64 postTrimCommitMB = QueryProcessCommitBytes() / (1024 * 1024);
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
	if (!m_infoQueue)
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
	if (!Renderer::ImguiBegin(mainWindow))
		return false;
	if (m_imguiInitialized)
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
	ImGui::Text("Sampler states       %zu", m_samplerCache.size());
	ImGui::Text("Rasterizer states    %zu", m_rasterizerCache.size());
	ImGui::Text("Blend states         %zu", m_blendCache.size());
	ImGui::Text("Depth/stencil states %zu", m_depthStencilCache.size());
	ImGui::Text("Buffer cache         %zu MB", m_bufferCacheShadow.size() / (1024 * 1024));
}

void D3D11Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool)
{
	const D3D11_VIEWPORT viewport{ x, y, (std::max)(width, 1.0f), (std::max)(height, 1.0f), nearZ, farZ };
	m_context->RSSetViewports(1, &viewport);
}

void D3D11Renderer::renderTarget_setScissor(sint32 x, sint32 y, sint32 width, sint32 height)
{
	const D3D11_RECT rect{ x, y, x + (std::max)(width, 1), y + (std::max)(height, 1) };
	m_context->RSSetScissorRects(1, &rect);
}

LatteCachedFBO* D3D11Renderer::rendertarget_createCachedFBO(uint64 key) { return new LatteCachedFBO(key); }
void D3D11Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO*)
{
	// LatteMRT::DeleteCachedFBO owns the common object and deletes it after this
	// renderer hook returns. D3D11 has no separate framebuffer object to free;
	// deleting here as well caused a double-free during the post-Present texture
	// cleanup, commonly surfacing as a read from 0xFFFFFFFFFFFFFFFF.
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
	m_context->VSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
	m_context->PSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
	m_context->GSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
	m_boundTextures.fill(nullptr);
	m_feedbackViews.clear();
	m_feedbackResources.clear();
}

void D3D11Renderer::ResolveTextureFeedbackLoops(
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
		return;
#if defined(CEMU_UWP)
	// A feedback snapshot duplicates the complete native texture. Under pressure,
	// let D3D11 null the conflicting SRV when the output merger is rebound. A
	// localized missing sample is preferable to crossing the Series S title cap.
	constexpr uint64 feedbackSnapshotStopBytes = 4096ull * 1024 * 1024;
	if (QueryProcessPrivateCommitBytes() >= feedbackSnapshotStopBytes)
		return;
#endif
	std::unordered_map<ID3D11Resource*, ComPtr<ID3D11Resource>> snapshotsBySource;

	auto replaceConflicts = [&](auto getResources, auto setResources)
	{
		std::array<ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> current;
		std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> raw{};
		getResources(raw.data());
		for (UINT slot = 0; slot < raw.size(); ++slot)
			current[slot].Attach(raw[slot]);

		for (UINT slot = 0; slot < current.size(); ++slot)
		{
			if (!current[slot])
				continue;
			ComPtr<ID3D11Resource> source;
			current[slot]->GetResource(&source);
			const bool conflict = std::any_of(outputResources.begin(), outputResources.end(),
				[&](const auto& output) { return output.Get() == source.Get(); });
			if (!conflict)
				continue;

			ComPtr<ID3D11Resource> snapshot;
			if (const auto existing = snapshotsBySource.find(source.Get());
				existing != snapshotsBySource.end())
			{
				snapshot = existing->second;
			}
			else
			{
				D3D11_RESOURCE_DIMENSION dimension{};
				source->GetType(&dimension);
				if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
				{
					ComPtr<ID3D11Texture1D> sourceTexture;
					source.As(&sourceTexture);
					D3D11_TEXTURE1D_DESC desc{};
					sourceTexture->GetDesc(&desc);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;
					ComPtr<ID3D11Texture1D> copy;
					if (FAILED(m_device->CreateTexture1D(&desc, nullptr, &copy)))
						continue;
					snapshot = copy;
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
					ComPtr<ID3D11Texture2D> copy;
					if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &copy)))
						continue;
					snapshot = copy;
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
					ComPtr<ID3D11Texture3D> copy;
					if (FAILED(m_device->CreateTexture3D(&desc, nullptr, &copy)))
						continue;
					snapshot = copy;
				}
				if (!snapshot)
					continue;
				m_context->CopyResource(snapshot.Get(), source.Get());
				snapshotsBySource.emplace(source.Get(), snapshot);
				m_feedbackResources.emplace_back(snapshot);
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			current[slot]->GetDesc(&viewDesc);
			ComPtr<ID3D11ShaderResourceView> snapshotView;
			if (FAILED(m_device->CreateShaderResourceView(snapshot.Get(), &viewDesc, &snapshotView)))
				continue;
			ID3D11ShaderResourceView* replacement = snapshotView.Get();
			setResources(slot, &replacement);
			m_feedbackViews.emplace_back(std::move(snapshotView));
		}
	};

	replaceConflicts(
		[this](ID3D11ShaderResourceView** values) {
			m_context->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, values);
		},
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->VSSetShaderResources(slot, 1, value);
		});
	replaceConflicts(
		[this](ID3D11ShaderResourceView** values) {
			m_context->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, values);
		},
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->PSSetShaderResources(slot, 1, value);
		});
	replaceConflicts(
		[this](ID3D11ShaderResourceView** values) {
			m_context->GSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, values);
		},
		[this](UINT slot, ID3D11ShaderResourceView** value) {
			m_context->GSSetShaderResources(slot, 1, value);
		});
}

void D3D11Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO* fbo)
{
	m_boundColorBlendable.fill(true);
	if (!fbo)
	{
		m_context->OMSetRenderTargets(0, nullptr, nullptr);
		return;
	}
	std::array<ID3D11RenderTargetView*, 8> targets{};
	UINT count{};
	for (UINT i = 0; i < targets.size(); ++i)
	{
		if (fbo->colorBuffer[i].texture)
		{
			auto* textureView = static_cast<D3D11TextureView*>(fbo->colorBuffer[i].texture);
			textureView->PrepareForRenderTarget();
			targets[i] = textureView->RTV();
			if (targets[i])
			{
				count = i + 1;
				UINT formatSupport{};
				// The view may reinterpret a typeless GX2 allocation. Pipeline
				// state must follow the bound RTV, not the base texture format.
				const auto format = textureView->RTVFormat();
				m_boundColorBlendable[i] =
					SUCCEEDED(m_device->CheckFormatSupport(format, &formatSupport)) &&
					(formatSupport & D3D11_FORMAT_SUPPORT_BLENDABLE) != 0;
			}
			else
				cemuLog_log(LogType::Force,
					"D3D11 color attachment {} has no render-target view (format 0x{:x})",
					i, static_cast<uint32>(fbo->colorBuffer[i].texture->format));
		}
	}
	ID3D11DepthStencilView* depth = fbo->depthBuffer.texture ?
		static_cast<D3D11TextureView*>(fbo->depthBuffer.texture)->DSV() : nullptr;
	if (fbo->depthBuffer.texture)
		static_cast<D3D11TextureView*>(fbo->depthBuffer.texture)->PrepareForRenderTarget();
	// Vulkan uses VK_EXT_attachment_feedback_loop_layout when a title samples
	// an attachment that it is also updating. D3D11 forbids simultaneous SRV
	// and RTV/DSV bindings, so snapshot only the conflicting inputs before the
	// output merger is rebound. This preserves the pre-draw contents instead
	// of letting the runtime silently replace the SRV with null.
	ResolveTextureFeedbackLoops(targets, depth);
	m_context->OMSetRenderTargets(count, targets.data(), depth);
}

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
	auto bindTexture = [&](LatteDecompilerShader* shader, uint32 textureIndex,
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
		setSamplers(binding, &sampler);
	};
	if (unit < LATTE_CEMU_VS_TEX_UNIT_BASE)
	{
		const uint32 textureIndex = unit - LATTE_CEMU_PS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActivePixelShader();
		bindTexture(shader, textureIndex,
			[this](UINT binding, ID3D11ShaderResourceView** value) { m_context->PSSetShaderResources(binding, 1, value); },
			[this](UINT binding, ID3D11SamplerState** value) { m_context->PSSetSamplers(binding, 1, value); });
	}
	else if (unit < LATTE_CEMU_GS_TEX_UNIT_BASE)
	{
		const uint32 textureIndex = unit - LATTE_CEMU_VS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActiveVertexShader();
		bindTexture(shader, textureIndex,
			[this](UINT binding, ID3D11ShaderResourceView** value) { m_context->VSSetShaderResources(binding, 1, value); },
			[this](UINT binding, ID3D11SamplerState** value) { m_context->VSSetSamplers(binding, 1, value); });
	}
	else
	{
		const uint32 textureIndex = unit - LATTE_CEMU_GS_TEX_UNIT_BASE;
		auto* shader = LatteSHRC_GetActiveGeometryShader();
		bindTexture(shader, textureIndex,
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
	m_graphicsStateInvalid = true;
	LatteGPUState.repeatTextureInitialization = true;
}

void D3D11Renderer::bufferCache_init(const sint32 size)
{
	m_bufferCacheShadow.resize((std::max)(size, 16));
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(m_bufferCacheShadow.size());
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
	ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &m_bufferCache), "Create buffer cache");
	desc.ByteWidth = LatteStreamout_GetRingBufferSize();
	desc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
	for (auto& streamoutBuffer : m_streamoutBuffers)
		ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &streamoutBuffer), "Create stream-output ring buffer");
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
		std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> empty{};
		std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> offsets{};
		m_context->SOSetTargets(static_cast<UINT>(empty.size()), empty.data(), offsets.data());
		m_streamoutActive = false;
	}
	ID3D11Buffer* sourceBuffer{};
	for (UINT i = 0; i < m_streamoutBuffers.size(); ++i)
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
			"D3D11 stream-output copy skipped: no completed source for offset {} size {}",
			src, size);
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
	uint64 auxHash, const std::string& source, bool, bool isGfxPackSource)
{
	if (m_deviceLost.load(std::memory_order_relaxed))
		return nullptr;
#if defined(CEMU_UWP)
	// The desktop cache benefits from parallel compilation, but Xbox compiler and
	// translation workers share one strict process budget. Serialize the complete
	// pipeline so glslang, SPIRV-Cross and xbsc_xs.dll cannot overlap across jobs.
	std::lock_guard pipelineLock(s_xboxShaderPipelineMutex);
	if (m_deviceLost.load(std::memory_order_relaxed))
		return nullptr;
	constexpr uint64 resumeCompileMB = 3840;
	constexpr uint64 stopCompileMB = 4096;
	// Return fragmented process-heap regions before deciding whether enough
	// headroom remains for the Xbox shader compiler's next transient peak.
	HeapCompact(GetProcessHeap(), 0);
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
		// This function may execute on a shader-cache worker. Never use the D3D11
		// immediate context here to evict resources: doing so races the graphics
		// thread and can clear live texture/shader bindings. The present-thread
		// memory guard performs retirement safely.
		cemuLog_log(LogType::Force,
			"D3D11 Series S shader compilation paused at {} MB process commit "
			"after {} compiled shaders",
			commitMB, m_compiledShaderCount.load(std::memory_order_relaxed));
		m_shaderCompilationBlocked.store(true, std::memory_order_relaxed);
		return nullptr;
	}
#endif
	try
	{
		auto shader = std::make_unique<D3D11Shader>(m_device.Get(), type, baseHash,
			auxHash, isGfxPackSource, source);
#if defined(CEMU_UWP)
		// glslang, SPIRV-Cross and the Xbox shader compiler create many short-lived
		// allocations. Compact between synchronous compilations so free process
		// heap regions can be returned before the next shader raises the high-water
		// mark of the 5 GiB title budget.
		HeapCompact(GetProcessHeap(), 0);
#endif
		if (!shader->IsCompiled())
			return nullptr;
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

void D3D11Renderer::streamout_setupXfbBuffer(uint32 index, sint32 ringBufferOffset, uint32, uint32 rangeSize)
{
	if (index >= LATTE_NUM_STREAMOUT_BUFFER)
		return;
	const uint64 offset = static_cast<uint64>((std::max)(ringBufferOffset, 0));
	const bool validRange = ringBufferOffset >= 0 && rangeSize != 0 &&
		offset + rangeSize <= static_cast<uint32>(LatteStreamout_GetRingBufferSize());
	m_streamoutEnabled[index] = validRange;
	m_streamoutOffsets[index] = validRange ? static_cast<UINT>(offset) : 0;
	if (rangeSize != 0 && !validRange)
		cemuLog_log(LogType::Force,
			"D3D11 stream-output buffer {} rejected invalid range offset={} size={}",
			index, ringBufferOffset, rangeSize);
}

void D3D11Renderer::streamout_begin()
{
	auto* gsContext = LatteSHRC_GetActiveGeometryShader();
	auto* vsContext = LatteSHRC_GetActiveVertexShader();
	auto* shader = gsContext ? static_cast<D3D11Shader*>(gsContext->shader) :
		(vsContext ? static_cast<D3D11Shader*>(vsContext->shader) : nullptr);
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
	bool hasOutputBuffer = false;
	for (UINT i = 0; i < buffers.size(); ++i)
	{
		buffers[i] = m_streamoutEnabled[i] ? m_streamoutBuffers[i].Get() : nullptr;
		hasOutputBuffer |= buffers[i] != nullptr;
	}
	if (!hasOutputBuffer)
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
		std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> empty{};
		std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> offsets{};
		m_context->SOSetTargets(static_cast<UINT>(empty.size()), empty.data(), offsets.data());
		m_streamoutActive = false;
	}
	m_streamoutEnabled.fill(false);
	// Restore the title's GS or the RECT emulation GS. Merely restoring the
	// title GS loses rectangle emulation on subsequent draws in the sequence.
	BindActiveShaders();
}
void D3D11Renderer::draw_beginSequence() {}

RendererShader* D3D11Renderer::GetRectEmulationShader(LatteDecompilerShader* vertexShader)
{
	if (!vertexShader)
		return nullptr;
	LatteShaderPSInputTable* inputTable = LatteSHRC_GetPSInputTable();
	if (!inputTable)
		return nullptr;
	const uint64 keyValues[] = { vertexShader->baseHash, vertexShader->auxHash, inputTable->key };
	const uint64 key = HashBytes(keyValues, sizeof(keyValues));
	if (const auto existing = m_rectShaderCache.find(key); existing != m_rectShaderCache.end())
		return existing->second.get();

	struct RectParameter
	{
		sint32 semantic{};
		sint32 location{};
		bool isFlat{};
		bool isNoPerspective{};
	};
	std::vector<RectParameter> parameters;
	std::unordered_set<sint32> parameterSemantics;
	std::unordered_set<sint32> parameterLocations;
	const auto parameterMask = vertexShader->outputParameterMask;
	for (uint32 index = 0; index < 32; ++index)
	{
		if ((parameterMask & (1u << index)) == 0)
			continue;
		const sint32 semantic = inputTable->getVertexShaderOutParamSemanticId(
			LatteGPUState.contextNew.GetRawView(), index);
		if (semantic < 0)
			continue;
		const auto* pixelImport = inputTable->getPSImportBySemanticId(semantic);
		if (!pixelImport)
			continue;
		const sint32 location = inputTable->getPSImportLocationBySemanticId(semantic);
		if (location < 0 || !parameterSemantics.emplace(semantic).second ||
			!parameterLocations.emplace(location).second)
			continue;
		parameters.push_back({ semantic, location,
			pixelImport->isFlat, pixelImport->isNoPerspective });
	}

	// This helper is deliberately emitted directly as HLSL.  Passing a geometry
	// shader through Vulkan GLSL and SPIR-V introduces Vulkan-only builtins which
	// SPIRV-Cross cannot map to shader model 5, even though rectangle expansion
	// itself only requires SV_Position and the user TEXCOORD semantics.
	std::string source;
	source.append("struct RectInput {\r\nfloat4 position : SV_Position;\r\n");
	for (const auto& parameter : parameters)
		source.append(fmt::format("float4 parameterSem{} : TEXCOORD{};\r\n",
			parameter.semantic, parameter.location));
	source.append("};\r\nstruct RectOutput {\r\nfloat4 position : SV_Position;\r\n");
	for (const auto& parameter : parameters)
	{
		if (parameter.isFlat)
			source.append("nointerpolation ");
		else if (parameter.isNoPerspective)
			source.append("noperspective ");
		source.append(fmt::format("float4 parameterSem{} : TEXCOORD{};\r\n",
			parameter.semantic, parameter.location));
	}
	source.append(
		"};\r\n"
		"float4 gen4thVertexA(float4 a,float4 b,float4 c){return b-(c-a);}\r\n"
		"float4 gen4thVertexB(float4 a,float4 b,float4 c){return c-(b-a);}\r\n"
		"float4 gen4thVertexC(float4 a,float4 b,float4 c){return c+(b-a);}\r\n"
		"[maxvertexcount(4)]\r\n"
		"void main(triangle RectInput inputVertices[3], inout TriangleStream<RectOutput> outputStream){\r\n"
		"RectOutput outputVertex;\r\n"
		"float dist0_1=length(inputVertices[1].position.xy-inputVertices[0].position.xy);\r\n"
		"float dist0_2=length(inputVertices[2].position.xy-inputVertices[0].position.xy);\r\n"
		"float dist1_2=length(inputVertices[2].position.xy-inputVertices[1].position.xy);\r\n"
		"if(dist0_1>dist0_2&&dist0_1>dist1_2){\r\n");
	auto appendVertex = [&](sint32 vertex, const char* generatedVariant)
	{
		if (generatedVariant)
		{
			source.append(fmt::format(
				"outputVertex.position=gen4thVertex{}(inputVertices[0].position,inputVertices[1].position,inputVertices[2].position);\r\n",
				generatedVariant));
			for (const auto& parameter : parameters)
				source.append(fmt::format(
					"outputVertex.parameterSem{}=gen4thVertex{}(inputVertices[0].parameterSem{},inputVertices[1].parameterSem{},inputVertices[2].parameterSem{});\r\n",
					parameter.semantic, generatedVariant, parameter.semantic,
					parameter.semantic, parameter.semantic));
		}
		else
		{
			source.append(fmt::format("outputVertex.position=inputVertices[{}].position;\r\n", vertex));
			for (const auto& parameter : parameters)
				source.append(fmt::format(
					"outputVertex.parameterSem{}=inputVertices[{}].parameterSem{};\r\n",
					parameter.semantic, vertex, parameter.semantic));
		}
		source.append("outputStream.Append(outputVertex);\r\n");
	};
	auto appendVertices = [&](sint32 p0, sint32 p1, sint32 p2, const char* variant)
	{
		appendVertex(p0, nullptr);
		appendVertex(p1, nullptr);
		appendVertex(p2, nullptr);
		appendVertex(0, variant);
	};
	appendVertices(2, 1, 0, "A");
	source.append("}else if(dist0_2>dist0_1&&dist0_2>dist1_2){\r\n");
	appendVertices(1, 2, 0, "B");
	source.append("}else{\r\n");
	appendVertices(0, 1, 2, "C");
	source.append("}\r\noutputStream.RestartStrip();\r\n}\r\n");

	auto shader = std::make_unique<D3D11Shader>(m_device.Get(),
		RendererShader::ShaderType::kGeometry, key, 0, false, source, true);
	if (!shader->IsCompiled() || !shader->Geometry())
	{
		cemuLog_log(LogType::Force,
			"D3D11 rectangle emulation shader creation failed for vertex shader {:016x}",
			vertexShader->baseHash);
		return nullptr;
	}
	auto* result = shader.get();
	m_rectShaderCache.emplace(key, std::move(shader));
	return result;
}

bool D3D11Renderer::BindActiveShaders()
{
	auto* vsContext = LatteSHRC_GetActiveVertexShader();
	auto* psContext = LatteSHRC_GetActivePixelShader();
	auto* gsContext = LatteSHRC_GetActiveGeometryShader();
	auto* vs = vsContext ? static_cast<D3D11Shader*>(vsContext->shader) : nullptr;
	auto* ps = psContext ? static_cast<D3D11Shader*>(psContext->shader) : nullptr;
	auto* gs = gsContext ? static_cast<D3D11Shader*>(gsContext->shader) : nullptr;
	if (!gs && LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() ==
		LattePrimitiveMode::RECTS)
	{
		gs = static_cast<D3D11Shader*>(GetRectEmulationShader(vsContext));
		if (!gs)
			return false;
	}
	m_context->VSSetShader(vs ? vs->Vertex() : nullptr, nullptr, 0);
	m_context->PSSetShader(ps ? ps->Pixel() : nullptr, nullptr, 0);
	m_context->GSSetShader(gs ? gs->Geometry() : nullptr, nullptr, 0);
	return true;
}

bool D3D11Renderer::HasRequiredShaders() const
{
	const auto* vsContext = LatteSHRC_GetActiveVertexShader();
	const auto* psContext = LatteSHRC_GetActivePixelShader();
	const auto* gsContext = LatteSHRC_GetActiveGeometryShader();
	const auto* vs = vsContext ? static_cast<const D3D11Shader*>(vsContext->shader) : nullptr;
	const auto* ps = psContext ? static_cast<const D3D11Shader*>(psContext->shader) : nullptr;
	const auto* gs = gsContext ? static_cast<const D3D11Shader*>(gsContext->shader) : nullptr;

	if (!vs || !vs->Vertex())
		return false;
	if (psContext && (!ps || !ps->Pixel()))
		return false;
	if (gsContext && (!gs || !gs->Geometry()))
		return false;
	return true;
}

bool D3D11Renderer::UpdateDynamicConstantBuffer(ComPtr<ID3D11Buffer>& buffer,
	UINT& capacity, const void* data, UINT size)
{
	if (!data || !size)
		return false;
	const UINT required = Align16(size);
	if (!buffer || capacity < required)
	{
		// Grow geometrically to avoid recreating a buffer when shaders alternate
		// between nearby uniform block sizes. Map(DISCARD) lets the driver rename
		// storage instead of waiting for an earlier draw to finish reading it.
		UINT newCapacity = capacity ? capacity : 256;
		while (newCapacity < required &&
			newCapacity <= (std::numeric_limits<UINT>::max)() / 2)
			newCapacity *= 2;
		if (newCapacity < required)
			newCapacity = required;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = newCapacity;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ComPtr<ID3D11Buffer> replacement;
		if (FAILED(m_device->CreateBuffer(&desc, nullptr, &replacement)))
			return false;
		buffer = std::move(replacement);
		capacity = newCapacity;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(m_context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	std::memcpy(mapped.pData, data, size);
	if (required > size)
		std::memset(static_cast<uint8*>(mapped.pData) + size, 0, required - size);
	m_context->Unmap(buffer.Get(), 0);
	return true;
}

void D3D11Renderer::UpdateUniformVars(LatteDecompilerShader* shader, uint32 verticesPerInstance)
{
	if (!shader || shader->resourceMapping.uniformVarsBufferBindingPoint < 0 ||
		shader->uniform.uniformRangeSize == 0)
		return;
	const uint32 stage = static_cast<uint32>(shader->shaderType);
	if (stage >= m_uniformScratch.size())
		return;
	auto& bytes = m_uniformScratch[stage];
	bytes.assign(Align16(shader->uniform.uniformRangeSize), 0);
	auto dataAt = [&bytes](sint32 offset) { return bytes.data() + offset; };
	for (auto& entry : shader->uniform.list_ufTexRescale)
	{
		float* scale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		std::memcpy(entry.currentValue, scale, sizeof(float) * 2);
		std::memcpy(dataAt(entry.uniformLocation), scale, sizeof(float) * 2);
	}
	if (shader->uniform.loc_alphaTestRef >= 0)
		*reinterpret_cast<float*>(dataAt(shader->uniform.loc_alphaTestRef)) =
			LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (shader->uniform.loc_pointSize >= 0)
	{
		float pointSize = static_cast<float>(LatteGPUState.contextNew.PA_SU_POINT_SIZE.get_WIDTH()) / 8.0f;
		*reinterpret_cast<float*>(dataAt(shader->uniform.loc_pointSize)) =
			pointSize == 0.0f ? 1.0f / 8.0f : pointSize;
	}
	if (shader->uniform.loc_remapped >= 0)
		LatteBufferCache_LoadRemappedUniforms(shader,
			reinterpret_cast<float*>(dataAt(shader->uniform.loc_remapped)), true,
			(1u << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	if (shader->uniform.loc_uniformRegister >= 0)
	{
		const sint32 registerOffset = shader->shaderType == LatteConst::ShaderType::Vertex ? 0x400 : 0;
		const uint32* registers = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + registerOffset;
		std::memcpy(dataAt(shader->uniform.loc_uniformRegister), registers,
			shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 width{}, height{};
		LatteRenderTarget_GetCurrentVirtualViewportSize(&width, &height);
		float* value = reinterpret_cast<float*>(dataAt(shader->uniform.loc_windowSpaceToClipSpaceTransform));
		value[0] = 2.0f / (std::max)(width, 1);
		value[1] = 2.0f / (std::max)(height, 1);
	}
	if (shader->uniform.loc_fragCoordScale >= 0)
		LatteMRT::GetCurrentFragCoordScale(reinterpret_cast<float*>(dataAt(shader->uniform.loc_fragCoordScale)));
	if (shader->uniform.loc_verticesPerInstance >= 0)
		*reinterpret_cast<uint32*>(dataAt(shader->uniform.loc_verticesPerInstance)) = verticesPerInstance;

	if (!m_uniformVarsBuffers[stage] || !m_uniformScratchUploaded[stage] ||
		m_uploadedUniformScratch[stage] != bytes)
	{
		if (!UpdateDynamicConstantBuffer(m_uniformVarsBuffers[stage],
			m_uniformVarsBufferCapacity[stage], bytes.data(), static_cast<UINT>(bytes.size())))
			return;
		m_uploadedUniformScratch[stage] = bytes;
		m_uniformScratchUploaded[stage] = true;
	}
	ID3D11Buffer* buffer = m_uniformVarsBuffers[stage].Get();
	auto* nativeShader = static_cast<D3D11Shader*>(shader->shader);
	if (!nativeShader)
		return;
	const sint32 originalBinding = shader->resourceMapping.uniformVarsBufferBindingPoint;
	const UINT binding = originalBinding >= 0 ?
		nativeShader->UniformSlot(static_cast<UINT>(originalBinding)) : D3D11Shader::InvalidSlot;
	if (binding == D3D11Shader::InvalidSlot)
		return;
	if (shader->shaderType == LatteConst::ShaderType::Vertex) m_context->VSSetConstantBuffers(binding, 1, &buffer);
	else if (shader->shaderType == LatteConst::ShaderType::Pixel) m_context->PSSetConstantBuffers(binding, 1, &buffer);
	else if (shader->shaderType == LatteConst::ShaderType::Geometry) m_context->GSSetConstantBuffers(binding, 1, &buffer);
}

void D3D11Renderer::UpdateSamplerSwizzleBuffer(LatteDecompilerShader* shader)
{
	if (!shader || !shader->shader)
		return;
	auto* nativeShader = static_cast<D3D11Shader*>(shader->shader);
	const UINT binding = nativeShader->SamplerSwizzleSlot();
	if (binding == D3D11Shader::InvalidSlot)
		return;
	const uint32 stage = static_cast<uint32>(shader->shaderType);
	if (stage >= m_samplerSwizzles.size())
		return;
	if (!m_samplerSwizzleBuffers[stage] || !m_samplerSwizzleUploaded[stage] ||
		m_uploadedSamplerSwizzles[stage] != m_samplerSwizzles[stage])
	{
		if (!UpdateDynamicConstantBuffer(m_samplerSwizzleBuffers[stage],
			m_samplerSwizzleBufferCapacity[stage], m_samplerSwizzles[stage].data(),
			static_cast<UINT>(sizeof(m_samplerSwizzles[stage]))))
			return;
		m_uploadedSamplerSwizzles[stage] = m_samplerSwizzles[stage];
		m_samplerSwizzleUploaded[stage] = true;
	}
	ID3D11Buffer* buffer = m_samplerSwizzleBuffers[stage].Get();
	if (shader->shaderType == LatteConst::ShaderType::Vertex)
		m_context->VSSetConstantBuffers(binding, 1, &buffer);
	else if (shader->shaderType == LatteConst::ShaderType::Pixel)
		m_context->PSSetConstantBuffers(binding, 1, &buffer);
	else if (shader->shaderType == LatteConst::ShaderType::Geometry)
		m_context->GSSetConstantBuffers(binding, 1, &buffer);
}

bool D3D11Renderer::UpdateInputLayout()
{
	auto* fetch = LatteSHRC_GetActiveFetchShader();
	auto* shaderContext = LatteSHRC_GetActiveVertexShader();
	auto* shader = shaderContext ? static_cast<D3D11Shader*>(shaderContext->shader) : nullptr;
	if (!fetch || !shader || !shader->Bytecode())
		return false;
	const uint64 keyValues[] = { fetch->key, shaderContext->baseHash, shaderContext->auxHash };
	const uint64 key = HashBytes(keyValues, sizeof(keyValues));
	if (m_inputLayoutKeyValid && m_inputLayoutKey == key)
	{
		// Internal fullscreen copies temporarily bind a null input layout.  The
		// cached COM object remains valid, so an early return must also restore it
		// on the immediate context before the next indexed GX2 draw.
		m_context->IASetInputLayout(m_inputLayout.Get());
		return true;
	}
	if (const auto cached = m_inputLayoutCache.find(key); cached != m_inputLayoutCache.end())
	{
		m_inputLayout = cached->second;
		m_inputLayoutKey = key;
		m_inputLayoutKeyValid = true;
		m_context->IASetInputLayout(m_inputLayout.Get());
		return true;
	}
	std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
	std::unordered_set<sint32> usedLocations;
	for (const auto& group : fetch->bufferGroups)
	{
		for (sint32 i = 0; i < group.attribCount; ++i)
		{
			const auto& attribute = group.attrib[i];
			const sint32 location = shaderContext->resourceMapping.getAttribHostShaderIndex(attribute.semanticId);
			if (location < 0 || !usedLocations.emplace(location).second)
				continue;
			if (attribute.attributeBufferIndex >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
			{
				cemuLog_logOnce(LogType::Force,
					"D3D11 input layout rejected buffer slot {} for shader {:016x}_{:016x}",
					attribute.attributeBufferIndex, shaderContext->baseHash, shaderContext->auxHash);
				m_inputLayoutKeyValid = false;
				return false;
			}
			D3D11_INPUT_ELEMENT_DESC element{};
			element.SemanticName = "TEXCOORD";
			element.SemanticIndex = location;
			element.Format = VertexFormat(attribute.format);
			if (element.Format == DXGI_FORMAT_UNKNOWN)
			{
				m_context->IASetInputLayout(nullptr);
				m_inputLayout.Reset();
				m_inputLayoutKeyValid = false;
				return false;
			}
			element.InputSlot = attribute.attributeBufferIndex;
			element.AlignedByteOffset = attribute.offset;
			element.InputSlotClass = attribute.fetchType == LatteConst::INSTANCE_DATA ?
				D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
			element.InstanceDataStepRate = attribute.fetchType == LatteConst::INSTANCE_DATA ?
				(std::max)(attribute.aluDivisor, 1) : 0;
			elements.emplace_back(element);
		}
	}
	m_inputLayout.Reset();
	m_context->IASetInputLayout(nullptr);
	if (!elements.empty())
	{
		D3D11_DRIVER_TRACE(fmt::format("CreateInputLayout elements={} shader={:016x}_{:016x}",
			elements.size(), shaderContext->baseHash, shaderContext->auxHash));
		const HRESULT hr = m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
			shader->Bytecode()->GetBufferPointer(), shader->Bytecode()->GetBufferSize(), &m_inputLayout);
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force, "D3D11 input layout creation failed (0x{:08X})", static_cast<uint32>(hr));
			m_inputLayoutKeyValid = false;
			return false;
		}
	}
	m_inputLayoutCache.emplace(key, m_inputLayout);
	m_inputLayoutKey = key;
	m_inputLayoutKeyValid = true;
	m_context->IASetInputLayout(m_inputLayout.Get());
	return true;
}

void D3D11Renderer::ApplyPipelineState()
{
	const auto& registers = LatteGPUState.contextNew;

	D3D11_RASTERIZER_DESC rasterizer{};
	rasterizer.FillMode = D3D11_FILL_SOLID;
	bool cullFront = registers.PA_SU_SC_MODE_CNTL.get_CULL_FRONT();
	bool cullBack = registers.PA_SU_SC_MODE_CNTL.get_CULL_BACK();
	const auto primitive = registers.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	if (primitive == LattePrimitiveMode::RECTS)
	{
		if (registers.PA_SU_SC_MODE_CNTL.get_FRONT_FACE() ==
			Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}
	if (cullFront && !cullBack)
		rasterizer.CullMode = D3D11_CULL_FRONT;
	else if (cullBack && !cullFront)
		rasterizer.CullMode = D3D11_CULL_BACK;
	else
		rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.FrontCounterClockwise =
		registers.PA_SU_SC_MODE_CNTL.get_FRONT_FACE() ==
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
	const bool nearClipDisabled = registers.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE();
	const bool farClipDisabled = registers.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
	// D3D11 exposes a single depth-clipping switch whereas GX2 can disable the
	// near and far planes independently. Do not clip a plane explicitly disabled
	// by the title; the remaining plane is conservatively depth-clamped.
	rasterizer.DepthClipEnable = !nearClipDisabled && !farClipDisabled;
	if (nearClipDisabled != farClipDisabled)
		cemuLog_logOnce(LogType::Force,
			"D3D11 cannot independently disable near/far depth clipping; using depth clamp for this GX2 state");
	rasterizer.ScissorEnable = TRUE;
	rasterizer.MultisampleEnable = FALSE;
	rasterizer.AntialiasedLineEnable = FALSE;
	if (registers.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED())
	{
		// Match the factors used by the Vulkan path. GX2 stores the slope scale
		// with sixteen times the host API value; D3D11's constant factor is an
		// integer number of minimum depth units.
		rasterizer.DepthBias = static_cast<INT>(std::lround(
			registers.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET()));
		rasterizer.SlopeScaledDepthBias =
			registers.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f;
		rasterizer.DepthBiasClamp = registers.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
	}
	const uint64 rasterizerKey = HashBytes(&rasterizer, sizeof(rasterizer));
	auto rasterizerIt = m_rasterizerCache.find(rasterizerKey);
	if (rasterizerIt == m_rasterizerCache.end())
	{
		ComPtr<ID3D11RasterizerState> state;
		ThrowIfFailed(m_device->CreateRasterizerState(&rasterizer, &state), "Create GX2 rasterizer state");
		rasterizerIt = m_rasterizerCache.emplace(rasterizerKey, std::move(state)).first;
	}
	m_context->RSSetState(rasterizerIt->second.Get());

	D3D11_BLEND_DESC1 blend{};
	blend.AlphaToCoverageEnable = FALSE;
	blend.IndependentBlendEnable = TRUE;
	uint32 targetMask = registers.CB_TARGET_MASK.get_MASK();
	if (registers.CB_COLOR_CONTROL.get_SPECIAL_OP() ==
		Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE)
		targetMask = 0;
	const uint32 blendMask = registers.CB_COLOR_CONTROL.get_BLEND_MASK();
	for (uint32 i = 0; i < 8; ++i)
	{
		auto& target = blend.RenderTarget[i];
		const auto& source = registers.CB_BLENDN_CONTROL[i];
		target.BlendEnable = (blendMask & (1u << i)) != 0 && m_boundColorBlendable[i];
		target.RenderTargetWriteMask = static_cast<UINT8>((targetMask >> (i * 4)) & 0xF);
		target.SrcBlend = BlendFactor(source.get_COLOR_SRCBLEND());
		target.DestBlend = BlendFactor(source.get_COLOR_DSTBLEND());
		target.BlendOp = BlendOp(source.get_COLOR_COMB_FCN());
		if (source.get_SEPARATE_ALPHA_BLEND())
		{
			target.SrcBlendAlpha = BlendFactorAlpha(source.get_ALPHA_SRCBLEND());
			target.DestBlendAlpha = BlendFactorAlpha(source.get_ALPHA_DSTBLEND());
			target.BlendOpAlpha = BlendOp(source.get_ALPHA_COMB_FCN());
		}
		else
		{
			target.SrcBlendAlpha = BlendFactorAlpha(source.get_COLOR_SRCBLEND());
			target.DestBlendAlpha = BlendFactorAlpha(source.get_COLOR_DSTBLEND());
			target.BlendOpAlpha = target.BlendOp;
		}
	}
	const auto gx2LogicOp = registers.CB_COLOR_CONTROL.get_ROP();
	const bool logicOpEnabled = gx2LogicOp != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
	for (auto& target : blend.RenderTarget)
	{
		target.LogicOpEnable = logicOpEnabled;
		target.LogicOp = LogicOp(gx2LogicOp);
		// D3D11.1 requires logic operations and fixed-function blending to be
		// mutually exclusive for a render target. GX2's ROP selects the logic
		// result, so disabling blending here preserves the effective operation and
		// avoids an invalid CreateBlendState1 descriptor.
		if (logicOpEnabled)
			target.BlendEnable = FALSE;
	}
	const uint64 blendKey = HashBytes(&blend, sizeof(blend));
	auto blendIt = m_blendCache.find(blendKey);
	if (blendIt == m_blendCache.end())
	{
		ComPtr<ID3D11BlendState> state;
		HRESULT result = E_NOINTERFACE;
		if (m_device1)
		{
			ComPtr<ID3D11BlendState1> state1;
			result = m_device1->CreateBlendState1(&blend, &state1);
			if (SUCCEEDED(result))
				ThrowIfFailed(state1.As(&state), "Query ID3D11BlendState from ID3D11BlendState1");
		}
		if (FAILED(result))
		{
			D3D11_BLEND_DESC fallback{};
			fallback.AlphaToCoverageEnable = blend.AlphaToCoverageEnable;
			fallback.IndependentBlendEnable = blend.IndependentBlendEnable;
			for (uint32 i = 0; i < 8; ++i)
			{
				fallback.RenderTarget[i].BlendEnable = blend.RenderTarget[i].BlendEnable;
				fallback.RenderTarget[i].SrcBlend = blend.RenderTarget[i].SrcBlend;
				fallback.RenderTarget[i].DestBlend = blend.RenderTarget[i].DestBlend;
				fallback.RenderTarget[i].BlendOp = blend.RenderTarget[i].BlendOp;
				fallback.RenderTarget[i].SrcBlendAlpha = blend.RenderTarget[i].SrcBlendAlpha;
				fallback.RenderTarget[i].DestBlendAlpha = blend.RenderTarget[i].DestBlendAlpha;
				fallback.RenderTarget[i].BlendOpAlpha = blend.RenderTarget[i].BlendOpAlpha;
				fallback.RenderTarget[i].RenderTargetWriteMask =
					blend.RenderTarget[i].RenderTargetWriteMask;
			}
			ThrowIfFailed(m_device->CreateBlendState(&fallback, &state),
				"Create GX2 fallback blend state");
			if (logicOpEnabled)
				cemuLog_logOnce(LogType::Force,
					"D3D11.1 logic operations are unavailable; GX2 logic op {} will use COPY",
					static_cast<uint32>(gx2LogicOp));
		}
		blendIt = m_blendCache.emplace(blendKey, std::move(state)).first;
	}
	const float blendConstant[] = {
		registers.CB_BLEND_RED.get_RED(), registers.CB_BLEND_GREEN.get_GREEN(),
		registers.CB_BLEND_BLUE.get_BLUE(), registers.CB_BLEND_ALPHA.get_ALPHA()
	};
	m_context->OMSetBlendState(blendIt->second.Get(), blendConstant, 0xFFFFFFFF);

	const auto& depthControl = registers.DB_DEPTH_CONTROL;
	D3D11_DEPTH_STENCIL_DESC depth{};
	depth.DepthEnable = depthControl.get_Z_ENABLE();
	depth.DepthWriteMask = depthControl.get_Z_WRITE_ENABLE() ?
		D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	depth.DepthFunc = CompareFunc(depthControl.get_Z_FUNC());
	depth.StencilEnable = depthControl.get_STENCIL_ENABLE();
	depth.StencilReadMask = static_cast<UINT8>(registers.DB_STENCILREFMASK.get_STENCILMASK_F());
	depth.StencilWriteMask = static_cast<UINT8>(registers.DB_STENCILREFMASK.get_STENCILWRITEMASK_F());
	depth.FrontFace.StencilFunc = CompareFunc(depthControl.get_STENCIL_FUNC_F());
	depth.FrontFace.StencilFailOp = StencilOp(depthControl.get_STENCIL_FAIL_F());
	depth.FrontFace.StencilDepthFailOp = StencilOp(depthControl.get_STENCIL_ZFAIL_F());
	depth.FrontFace.StencilPassOp = StencilOp(depthControl.get_STENCIL_ZPASS_F());
	if (depthControl.get_BACK_STENCIL_ENABLE())
	{
		depth.BackFace.StencilFunc = CompareFunc(depthControl.get_STENCIL_FUNC_B());
		depth.BackFace.StencilFailOp = StencilOp(depthControl.get_STENCIL_FAIL_B());
		depth.BackFace.StencilDepthFailOp = StencilOp(depthControl.get_STENCIL_ZFAIL_B());
		depth.BackFace.StencilPassOp = StencilOp(depthControl.get_STENCIL_ZPASS_B());
	}
	else
		depth.BackFace = depth.FrontFace;
	const uint64 depthKey = HashBytes(&depth, sizeof(depth));
	auto depthIt = m_depthStencilCache.find(depthKey);
	if (depthIt == m_depthStencilCache.end())
	{
		ComPtr<ID3D11DepthStencilState> state;
		ThrowIfFailed(m_device->CreateDepthStencilState(&depth, &state), "Create GX2 depth/stencil state");
		depthIt = m_depthStencilCache.emplace(depthKey, std::move(state)).first;
	}
	m_context->OMSetDepthStencilState(depthIt->second.Get(),
		registers.DB_STENCILREFMASK.get_STENCILREF_F());
}

void D3D11Renderer::HandleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();
	auto* colorBuffer = LatteMRT::GetColorAttachment(0);
	auto* depthBuffer = LatteMRT::GetDepthAttachment();
	if (!colorBuffer || !depthBuffer)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 GX2 special state 5 skipped because color or depth attachment is missing");
		return;
	}
	sint32 width{}, height{};
	LatteMRT::GetVirtualViewportDimensions(width, height);
	if (width <= 0 || height <= 0)
		return;
	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		width, height);
}

void D3D11Renderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount,
	uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType,
	const LatteDrawcallContext& drawcallContext)
{
	if (m_deviceLost.load(std::memory_order_relaxed))
		return;
	if (drawcallContext.isFirst || m_graphicsStateInvalid)
	{
		m_graphicsStateInvalid = false;
		LatteSHRC_UpdateActiveShaders();
		if (LatteGPUState.activeShaderHasError)
			return;
		while (true)
		{
			LatteGPUState.repeatTextureInitialization = false;
			if (!LatteMRT::UpdateCurrentFBO())
				return;
			UnbindTextureHazards();
			ClearShaderResources();
			LatteTexture_updateTextures();
			if (!LatteGPUState.repeatTextureInitialization)
				break;
		}
		LatteMRT::ApplyCurrentState();
		if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
		{
			LatteDraw_handleSpecialState8_clearAsDepth();
			LatteGPUState.drawCallCounter++;
			return;
		}
		if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
		{
			HandleSpecialState5();
			LatteGPUState.drawCallCounter++;
			return;
		}
		if (!HasRequiredShaders())
		{
			LatteGPUState.activeShaderHasError = true;
			cemuLog_log(LogType::Force, "D3D11 draw skipped because a required native shader is unavailable");
			return;
		}
		if (!BindActiveShaders())
		{
			LatteGPUState.activeShaderHasError = true;
			return;
		}
		if (!UpdateInputLayout())
		{
			// A later draw in the same GX2 sequence must retry the layout instead
			// of continuing with the null layout installed by the failed attempt.
			m_graphicsStateInvalid = true;
			return;
		}
	}
	if (LatteGPUState.activeShaderHasError || !HasRequiredShaders())
		return;

	const LattePrimitiveMode primitive = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	Renderer::INDEX_TYPE hostIndexType{};
	uint32 hostIndexCount{}, indexMax{};
	Renderer::IndexAllocation allocation{};
	const void* indices = indexDataMPTR != MPTR_NULL ? memory_getPointerFromPhysicalOffset(indexDataMPTR) : nullptr;
	LatteIndices_decode(indices, indexType, count, primitive, indexMax, hostIndexType, hostIndexCount, allocation);
	if (m_deviceLost.load(std::memory_order_relaxed))
		return;
	if (indexMax > (std::numeric_limits<uint32>::max)() - baseVertex)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D11 draw skipped: vertex range overflow (indexMax={} baseVertex={})",
			indexMax, baseVertex);
#if defined(CEMU_UWP)
		LatteIndices_invalidateAll();
#endif
		LatteGPUState.drawCallCounter++;
		return;
	}
	uint8 stageUniformModifiedMask{};
	LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount,
		drawcallContext.isFirst ? 0xFFFFFFFF : drawcallContext.vertexBufferDirtyMask,
		drawcallContext.isFirst ? 0xFFFFFFFF : drawcallContext.vsUniformBufferDirtyMask,
		drawcallContext.isFirst ? 0xFFFFFFFF : drawcallContext.psUniformBufferDirtyMask,
		drawcallContext.isFirst ? 0xFFFFFFFF : drawcallContext.gsUniformBufferDirtyMask,
		stageUniformModifiedMask, !drawcallContext.isFirst);
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();
	UpdateUniformVars(LatteSHRC_GetActiveVertexShader(), count);
	UpdateUniformVars(LatteSHRC_GetActivePixelShader(), count);
	UpdateUniformVars(LatteSHRC_GetActiveGeometryShader(), count);
	UpdateSamplerSwizzleBuffer(LatteSHRC_GetActiveVertexShader());
	UpdateSamplerSwizzleBuffer(LatteSHRC_GetActivePixelShader());
	UpdateSamplerSwizzleBuffer(LatteSHRC_GetActiveGeometryShader());
#if defined(CEMU_UWP)
	const auto captureShaderHash = [](LatteDecompilerShader* context, uint64& base, uint64& aux)
	{
		auto* shader = context ? static_cast<D3D11Shader*>(context->shader) : nullptr;
		base = shader ? shader->BaseHash() : 0;
		aux = shader ? shader->AuxHash() : 0;
	};
	captureShaderHash(LatteSHRC_GetActiveVertexShader(),
		m_lastVertexShaderBase, m_lastVertexShaderAux);
	captureShaderHash(LatteSHRC_GetActivePixelShader(),
		m_lastPixelShaderBase, m_lastPixelShaderAux);
	captureShaderHash(LatteSHRC_GetActiveGeometryShader(),
		m_lastGeometryShaderBase, m_lastGeometryShaderAux);
#endif
	ApplyPipelineState();
	m_context->IASetPrimitiveTopology(PrimitiveTopology(primitive));
	LatteStreamout_PrepareDrawcall(count, instanceCount);
	bool drawIssued = false;

	const bool rasterizerKilled =
		LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() &&
		LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	const bool bothFacesCulled =
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_FRONT() &&
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_BACK();
	const bool skipRasterDraw =
		(rasterizerKilled || bothFacesCulled) && !m_streamoutActive;

	if (skipRasterDraw)
	{
		// Vulkan represents both states directly. D3D11 has no rasterizer-discard
		// switch and no FRONT_AND_BACK cull mode, so a draw without stream output
		// is a true no-op.
	}
	else if (hostIndexType != INDEX_TYPE::NONE)
	{
		auto* indexAllocation = static_cast<IndexBufferAllocation*>(allocation.rendererInternal);
		ID3D11Buffer* buffer = indexAllocation ? indexAllocation->buffer.Get() : nullptr;
		if (!buffer)
		{
			cemuLog_log(LogType::Force,
				"D3D11 indexed draw skipped because the decoded index allocation has no GPU buffer");
		}
		else
		{
			m_context->IASetIndexBuffer(buffer,
				hostIndexType == INDEX_TYPE::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				indexAllocation->offset);
			{
				D3D11_DRIVER_TRACE(fmt::format(
					"DrawIndexedInstanced indices={} instances={} baseVertex={} baseInstance={} buffer={}",
					hostIndexCount, instanceCount, baseVertex, baseInstance,
					static_cast<const void*>(buffer)));
				m_context->DrawIndexedInstanced(hostIndexCount, instanceCount, 0, baseVertex, baseInstance);
				drawIssued = true;
			}
			D3D11_DEBUG_CHECK("indexed GX2 draw");
		}
		// LatteIndices_decode stores this allocation in its LRU cache. The cache
		// releases it when the entry is evicted; releasing it after every draw
		// leaves a dangling cached pointer and causes the next cache hit to bind
		// a null/freed ID3D11Buffer.
	}
	else
	{
		D3D11_DRIVER_TRACE(fmt::format(
			"DrawInstanced vertices={} instances={} baseVertex={} baseInstance={}",
			count, instanceCount, baseVertex, baseInstance));
		m_context->DrawInstanced(count, instanceCount, baseVertex, baseInstance);
		drawIssued = true;
		D3D11_DEBUG_CHECK("non-indexed GX2 draw");
	}

	const bool streamoutDrawIssued = drawIssued && m_streamoutActive;
	if (drawIssued && !CheckDeviceHealth("GX2 draw"))
	{
		// Do not unbind SO or restore shaders after removal; either operation would
		// enter D3D11On12 again and amplify the driver's exception cascade.
		m_streamoutActive = false;
		m_streamoutEnabled.fill(false);
		m_graphicsStateInvalid = true;
		LatteGPUState.drawCallCounter++;
		return;
	}
	if (!drawIssued && m_streamoutActive)
		m_streamoutEnabled.fill(false);
	// This also unbinds SO and restores the title/rectangle geometry shader.
	LatteStreamout_FinishDrawcall(false);
	// Stream-output shaders use D3D11_SO_NO_RASTERIZED_STREAM, so a GX2 operation
	// requesting feedback and rasterization needs an ordinary second draw.
	bool rasterDrawIssued = drawIssued && !streamoutDrawIssued;
	if (streamoutDrawIssued && !rasterizerKilled && !bothFacesCulled)
	{
		if (hostIndexType != INDEX_TYPE::NONE)
		{
			auto* indexAllocation = static_cast<IndexBufferAllocation*>(allocation.rendererInternal);
			ID3D11Buffer* buffer = indexAllocation ? indexAllocation->buffer.Get() : nullptr;
			if (buffer)
			{
				m_context->IASetIndexBuffer(buffer,
					hostIndexType == INDEX_TYPE::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
					indexAllocation->offset);
				m_context->DrawIndexedInstanced(hostIndexCount, instanceCount, 0, baseVertex, baseInstance);
				rasterDrawIssued = true;
				D3D11_DEBUG_CHECK("indexed GX2 raster draw after stream output");
			}
		}
		else
		{
			m_context->DrawInstanced(count, instanceCount, baseVertex, baseInstance);
			rasterDrawIssued = true;
			D3D11_DEBUG_CHECK("non-indexed GX2 raster draw after stream output");
		}
	}

#if defined(CEMU_UWP)
	// The common index LRU was designed for allocations with independent immutable
	// storage. This backend uses offsets into a transient DISCARD/NO_OVERWRITE
	// ring, so retaining those offsets across draws can reuse data after the Xbox
	// D3D11On12 driver has renamed the backing allocation. Decode again on the next
	// indexed draw; the ring itself remains persistent and memory-stable.
	if (hostIndexType != INDEX_TYPE::NONE)
		LatteIndices_invalidateAll();
#endif

	if (rasterDrawIssued && LatteSHRC_GetActivePixelShader())
		LatteRenderTarget_trackUpdates();
	LatteGPUState.drawCallCounter++;
	LatteTextureReadback_Update();
}

void D3D11Renderer::draw_endSequence() {}

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
		// D3D11 permits DISCARD to rename the allocation, but the Xbox D3D11On12
		// path can still have translated draws consuming the previous backing store.
		// Retire that work before reusing offset zero; otherwise a quad can combine
		// new and old indices and expand into the long triangles seen in gameplay.
		m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		LatteIndices_invalidateAll();
		if (!WaitForGpuIdle())
		{
			cemuLog_log(LogType::Force,
				"D3D11 index upload skipped: GPU did not retire before ring wrap");
			delete data;
			allocation = {};
			return;
		}
		m_indexRingOffset = 0;
		++m_indexRingWrapCount;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	const D3D11_MAP mapMode = m_indexRingOffset == 0 ?
		D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
	const HRESULT mapResult = m_context->Map(m_indexRingBuffer.Get(), 0, mapMode, 0, &mapped);
	if (FAILED(mapResult))
	{
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
	std::vector<uint8>().swap(data->data);
	allocation.mem = nullptr;
}

LatteQueryObject* D3D11Renderer::occlusionQuery_create() { return new D3D11Query(m_device.Get(), m_context.Get()); }
void D3D11Renderer::occlusionQuery_destroy(LatteQueryObject* query) { delete query; }
void D3D11Renderer::occlusionQuery_flush() { Flush(true); }
void D3D11Renderer::occlusionQuery_updateState() {}
