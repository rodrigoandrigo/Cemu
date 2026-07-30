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
#include "interface/WindowSystem.h"

#include <backends/imgui_impl_dx11.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_hlsl.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include "imgui/imgui_extension.h"
#include <mutex>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

// Implemented by the common renderer. GX2 uses this special state to clear
// color aliases of a depth surface without issuing a normal shader draw.
void LatteDraw_handleSpecialState8_clearAsDepth();

namespace
{
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

void ThrowIfFailed(HRESULT result, const char* operation)
{
	if (FAILED(result))
		throw std::runtime_error(fmt::format("{} failed with HRESULT 0x{:08X}", operation, static_cast<uint32>(result)));
}

uint32 Align16(uint32 value)
{
	return (value + 15u) & ~15u;
}

uint64 HashBytes(const void* data, size_t size, uint64 hash = 1469598103934665603ull)
{
	const auto* bytes = static_cast<const uint8*>(data);
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
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
	const bool minLinear = minFilter != XY::POINT;
	const bool magLinear = magFilter != XY::POINT;
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

class D3D11Shader final : public RendererShader
{
public:
	D3D11Shader(ID3D11Device* device, ShaderType type, uint64 baseHash, uint64 auxHash,
		bool isGfxPack, const std::string& source)
		: RendererShader(type, baseHash, auxHash, true, isGfxPack)
	{
		Compile(device, source);
	}

	void PreponeCompilation(bool) override {}
	bool IsCompiled() override { return m_compiled; }
	bool WaitForCompiled() override { return m_compiled; }
	ID3D11VertexShader* Vertex() const { return m_vs.Get(); }
	ID3D11PixelShader* Pixel() const { return m_ps.Get(); }
	ID3D11GeometryShader* Geometry() const { return m_gs.Get(); }
	ID3D11GeometryShader* StreamoutGeometry() const { return m_streamoutGs.Get(); }
	ID3DBlob* Bytecode() const { return m_bytecode.Get(); }
	UINT TextureSlot(UINT originalBinding) const
	{
		return originalBinding < m_textureSlots.size() ? m_textureSlots[originalBinding] : InvalidSlot;
	}
	UINT UniformSlot(UINT originalBinding) const
	{
		return originalBinding < m_uniformSlots.size() ? m_uniformSlots[originalBinding] : InvalidSlot;
	}
	static constexpr UINT InvalidSlot = UINT_MAX;

private:
	void Compile(ID3D11Device* device, const std::string& source)
	{
		try
		{
			m_textureSlots.fill(InvalidSlot);
			m_uniformSlots.fill(InvalidSlot);
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
			std::vector<uint32> spirv;
			glslang::SpvOptions spvOptions;
			spvOptions.optimizeSize = true;
			glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spvOptions);

			spirv_cross::CompilerHLSL compiler(spirv);
			const auto resources = compiler.get_shader_resources();
			const auto executionModel = compiler.get_execution_model();
			UINT textureSlot{};
			for (const auto& resource : resources.sampled_images)
			{
				const UINT originalBinding =
					compiler.get_decoration(resource.id, spv::DecorationBinding);
				if (textureSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
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
				++textureSlot;
			}
			UINT uniformSlot{};
			for (const auto& resource : resources.uniform_buffers)
			{
				const UINT originalBinding =
					compiler.get_decoration(resource.id, spv::DecorationBinding);
				if (uniformSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
					throw std::runtime_error("shader requires more than 14 D3D11 constant buffers");
				spirv_cross::HLSLResourceBinding binding{};
				binding.stage = executionModel;
				binding.desc_set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
				binding.binding = originalBinding;
				binding.cbv.register_binding = uniformSlot;
				compiler.add_hlsl_resource_binding(binding);
				if (originalBinding < m_uniformSlots.size())
					m_uniformSlots[originalBinding] = uniformSlot;
				++uniformSlot;
			}
			auto options = compiler.get_hlsl_options();
			options.shader_model = 50;
			options.point_coord_compat = true;
			options.point_size_compat = true;
			compiler.set_hlsl_options(options);
			const std::string hlsl = compiler.compile();

			const char* profile = GetType() == ShaderType::kVertex ? "vs_5_0" :
				GetType() == ShaderType::kFragment ? "ps_5_0" : "gs_5_0";
			ComPtr<ID3DBlob> errors;
			HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main", profile,
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &m_bytecode, &errors);
			if (FAILED(hr))
			{
				const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error";
				throw std::runtime_error(message);
			}
			if (GetType() == ShaderType::kVertex)
			{
				DriverCallTrace trace(fmt::format("CreateVertexShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				ThrowIfFailed(device->CreateVertexShader(m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(), nullptr, &m_vs), "CreateVertexShader");
			}
			else if (GetType() == ShaderType::kFragment)
			{
				DriverCallTrace trace(fmt::format("CreatePixelShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				ThrowIfFailed(device->CreatePixelShader(m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(), nullptr, &m_ps), "CreatePixelShader");
			}
			else
			{
				DriverCallTrace trace(fmt::format("CreateGeometryShader {:016x}_{:016x} bytecode={}",
					m_baseHash, m_auxHash, m_bytecode->GetBufferSize()));
				ThrowIfFailed(device->CreateGeometryShader(m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(), nullptr, &m_gs), "CreateGeometryShader");
			}
			CreateStreamoutShader(device, source);
			m_compiled = true;
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D11 shader {:016x}_{:016x} failed: {}", m_baseHash, m_auxHash, ex.what());
		}
	}

	void CreateStreamoutShader(ID3D11Device* device, const std::string& source)
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
				block.slot < D3D11_SO_BUFFER_SLOT_COUNT && block.stride)
				blocks.emplace_back(block);
			cursor += 17;
		}
		if (blocks.empty())
			return;

		ComPtr<ID3D11ShaderReflection> reflection;
		if (FAILED(D3DReflect(m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(),
			IID_PPV_ARGS(&reflection))))
			return;
		D3D11_SHADER_DESC shaderDesc{};
		if (FAILED(reflection->GetDesc(&shaderDesc)))
			return;
		std::vector<D3D11_SIGNATURE_PARAMETER_DESC> outputs(shaderDesc.OutputParameters);
		for (UINT i = 0; i < shaderDesc.OutputParameters; ++i)
			reflection->GetOutputParameterDesc(i, &outputs[i]);

		std::vector<D3D11_SO_DECLARATION_ENTRY> declarations;
		std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> strides{};
		UINT strideCount{};
		for (const auto& block : blocks)
		{
			strides[block.slot] = block.stride;
			strideCount = (std::max)(strideCount, block.slot + 1);
			const UINT scalarCount = block.stride / sizeof(uint32);
			for (UINT scalar = 0; scalar < scalarCount; ++scalar)
			{
				const UINT semanticIndex = block.location + scalar;
				auto it = std::find_if(outputs.begin(), outputs.end(), [semanticIndex](const auto& output) {
					return output.SemanticName && _stricmp(output.SemanticName, "TEXCOORD") == 0 &&
						output.SemanticIndex == semanticIndex;
				});
				if (it == outputs.end())
					continue;
				UINT componentCount{};
				for (UINT mask = it->Mask; mask; mask >>= 1)
					componentCount += mask & 1;
				declarations.push_back({ 0, "TEXCOORD", semanticIndex, 0,
					static_cast<BYTE>((std::max)(componentCount, 1u)),
					static_cast<BYTE>(block.slot) });
			}
		}
		if (declarations.empty())
			return;
		const HRESULT hr = device->CreateGeometryShaderWithStreamOutput(
			m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(),
			declarations.data(), static_cast<UINT>(declarations.size()),
			strides.data(), strideCount, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &m_streamoutGs);
		if (FAILED(hr))
			cemuLog_log(LogType::Force, "D3D11 stream-output shader creation failed (0x{:08X})",
				static_cast<uint32>(hr));
	}

	bool m_compiled{};
	ComPtr<ID3DBlob> m_bytecode;
	ComPtr<ID3D11VertexShader> m_vs;
	ComPtr<ID3D11PixelShader> m_ps;
	ComPtr<ID3D11GeometryShader> m_gs;
	ComPtr<ID3D11GeometryShader> m_streamoutGs;
	std::array<UINT, 256> m_textureSlots{};
	std::array<UINT, 256> m_uniformSlots{};
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
	ID3D11ShaderResourceView* SRV() const { return m_srv.Get(); }
	ID3D11RenderTargetView* RTV() const { return m_rtv.Get(); }
	ID3D11DepthStencilView* DSV() const { return m_dsv.Get(); }
	DXGI_FORMAT RTVFormat() const { return m_rtvFormat; }
private:
	ComPtr<ID3D11ShaderResourceView> m_srv;
	ComPtr<ID3D11RenderTargetView> m_rtv;
	ComPtr<ID3D11DepthStencilView> m_dsv;
	DXGI_FORMAT m_rtvFormat{ DXGI_FORMAT_UNKNOWN };
};

class D3D11Texture final : public LatteTexture
{
public:
	D3D11Texture(D3D11Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress,
		Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch,
		uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
		: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch,
			mipLevels, swizzle, tileMode, isDepth), m_renderer(renderer), m_format(GetFormatInfo(format, isDepth)) {}

	void AllocateOnHost() override
	{
		if (m_resource)
			return;
		const uint32 logicalWidth = (std::max)(width, 1);
		const uint32 logicalHeight = (std::max)(height, 1);
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
			desc.MipLevels = (std::max)(mipLevels, 1);
			desc.ArraySize = dim == Latte::E_DIM::DIM_1D_ARRAY ? (std::max)(depth, 1) : 1;
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
			desc.Depth = (std::max)(depth, 1);
			desc.MipLevels = (std::max)(mipLevels, 1);
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
		desc.MipLevels = (std::max)(mipLevels, 1);
		desc.ArraySize = dim == Latte::E_DIM::DIM_CUBEMAP ? (std::max)(depth, 6) :
			(dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA ? (std::max)(depth, 1) : 1);
		desc.Format = m_format.resource;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		desc.MiscFlags = dim == Latte::E_DIM::DIM_CUBEMAP ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		ThrowIfFailed(m_renderer->GetDevice()->CreateTexture2D(&desc, nullptr, &m_texture2D), "CreateTexture2D");
		m_resource = m_texture2D;
	}

	ID3D11Resource* Resource() const { return m_resource.Get(); }
	ID3D11Texture1D* Texture1D() const { return m_texture1D.Get(); }
	ID3D11Texture2D* Texture2D() const { return m_texture2D.Get(); }
	ID3D11Texture3D* Texture3D() const { return m_texture3D.Get(); }
	const FormatInfo& NativeFormat() const { return m_format; }
	D3D11Renderer* Owner() const { return m_renderer; }

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
};

D3D11TextureView::D3D11TextureView(D3D11Texture* texture, Latte::E_DIM dim,
	Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format)
{
	texture->AllocateOnHost();
	const auto& baseNative = texture->NativeFormat();
	const auto viewNative = GetFormatInfo(format, texture->isDepth);
	// GX2 aliases the same allocation through views with different numeric
	// interpretations. Match Vulkan's mutable images by using the requested
	// LatteTextureView format instead of silently inheriting the base format.
	// D3D11 only accepts reinterpretation inside the same typeless family.
	const auto& native = texture->isDepth ? baseNative : viewNative;
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
		if (texture->depth > 6 || firstSlice >= 6 || sliceCount > 6)
		{
			const UINT firstFace = static_cast<UINT>((std::max)(firstSlice, 0));
			const UINT availableFaces = static_cast<UINT>((std::max)(
				static_cast<sint32>(texture->depth) - firstSlice, 0));
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
	else if (texture->depth > 1 || dim == Latte::E_DIM::DIM_2D_ARRAY)
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
		// Block-compressed aliases and a few packed format pairs have no legal
		// D3D11 view reinterpretation. Keep the resource usable and let Cemu's
		// explicit surface-conversion path handle those pairs.
		srv.Format = baseNative.srv;
		srvResult = texture->Owner()->GetDevice()->CreateShaderResourceView(
			texture->Resource(), &srv, &m_srv);
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
		else if (texture->depth > 1)
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
	else if (native.rtv != DXGI_FORMAT_UNKNOWN)
	{
		D3D11_RENDER_TARGET_VIEW_DESC rtv{};
		rtv.Format = native.rtv;
		if (dim == Latte::E_DIM::DIM_1D_ARRAY)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
			rtv.Texture1DArray.MipSlice = firstMip;
			rtv.Texture1DArray.FirstArraySlice = firstSlice;
			rtv.Texture1DArray.ArraySize = sliceCount;
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
			rtv.Texture3D.WSize = sliceCount;
		}
		else if (texture->depth > 1)
		{
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.MipSlice = firstMip;
			rtv.Texture2DArray.FirstArraySlice = firstSlice;
			rtv.Texture2DArray.ArraySize = sliceCount;
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

class D3D11Readback final : public LatteTextureReadbackInfo
{
public:
	D3D11Readback(D3D11Renderer* renderer, D3D11TextureView* view)
		: LatteTextureReadbackInfo(view), m_renderer(renderer), m_view(view) {}
	void StartTransfer() override
	{
		auto* texture = static_cast<D3D11Texture*>(m_view->baseTexture);
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
		const UINT subresource = D3D11CalcSubresource(m_view->firstMip, m_view->firstSlice, texture->mipLevels);
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
	default: return DXGI_FORMAT_R8G8B8A8_UINT;
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
	HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
	if (FAILED(hr))
		throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed");
	return blob;
}
}

D3D11Renderer::D3D11Renderer() : Renderer(RendererAPI::D3D11)
{
	const auto* surface = static_cast<const CemuEmbedD3D11Surface*>(WindowSystem::GetWindowInfo().canvas_main.surface);
	if (!surface || surface->struct_size < sizeof(CemuEmbedD3D11Surface) ||
		surface->abi_version != CEMU_EMBED_D3D11_SURFACE_VERSION ||
		!surface->device || !surface->immediate_context || !surface->swap_chain)
		throw std::runtime_error("The host did not provide a valid Direct3D 11 SwapChainPanel surface.");
	m_device = static_cast<ID3D11Device*>(surface->device);
	m_device.As(&m_device1);
	m_context = static_cast<ID3D11DeviceContext*>(surface->immediate_context);
	m_context.As(&m_context1);
	ComPtr<ID3D11Multithread> multithread;
	if (SUCCEEDED(m_context.As(&multithread)))
		multithread->SetMultithreadProtected(TRUE);
	if (SUCCEEDED(m_device.As(&m_infoQueue)))
	{
		// The debug layer raises exception 0x87A when break-on-error is enabled.
		// An embedded UWP host cannot treat that debugger-only notification as a
		// recoverable application exception, so collect the messages in log.txt.
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
		m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
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
	if (!mainWindow)
		return false;
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
	if (!swapTV)
		return;
	CheckDebugMessages("before Present");
	{
		DriverCallTrace trace("IDXGISwapChain::Present");
		ThrowIfFailed(m_swapChain->Present(1, 0), "IDXGISwapChain::Present");
	}
	CheckDebugMessages("after Present");
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_backBufferView.Reset();
	m_backBuffer.Reset();
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
	if (padView || !textureView || !BeginFrame(true))
		return;
	// Fill the complete SwapChainPanel while preserving the game's aspect ratio.
	// The regular Cemu rectangle uses "contain" scaling and therefore produces
	// black bars when the host window and the Wii U output have different aspect
	// ratios. Embedded presentation uses "cover" scaling instead: the excess is
	// centered and clipped by the full-back-buffer scissor rectangle.
	D3D11_TEXTURE2D_DESC backBufferDesc{};
	m_backBuffer->GetDesc(&backBufferDesc);
	const sint32 backBufferWidth = static_cast<sint32>((std::max)(backBufferDesc.Width, 1u));
	const sint32 backBufferHeight = static_cast<sint32>((std::max)(backBufferDesc.Height, 1u));
	int surfaceWidth = 1;
	int surfaceHeight = 1;
	WindowSystem::GetWindowPhysSize(surfaceWidth, surfaceHeight);
	const double coverScale = (std::max)(
		static_cast<double>(surfaceWidth) / static_cast<double>((std::max)(imageWidth, 1)),
		static_cast<double>(surfaceHeight) / static_cast<double>((std::max)(imageHeight, 1)));
	const double coveredWidth = imageWidth * coverScale;
	const double coveredHeight = imageHeight * coverScale;
	const double coveredX = (surfaceWidth - coveredWidth) * 0.5;
	const double coveredY = (surfaceHeight - coveredHeight) * 0.5;
	const double compositionScaleX = static_cast<double>(backBufferWidth) /
		static_cast<double>((std::max)(surfaceWidth, 1));
	const double compositionScaleY = static_cast<double>(backBufferHeight) /
		static_cast<double>((std::max)(surfaceHeight, 1));
	imageX = static_cast<sint32>(std::lround(coveredX * compositionScaleX));
	imageY = static_cast<sint32>(std::lround(coveredY * compositionScaleY));
	imageWidth = static_cast<sint32>(std::lround(coveredWidth * compositionScaleX));
	imageHeight = static_cast<sint32>(std::lround(coveredHeight * compositionScaleY));
	if (clearBackground)
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
	m_context->Flush();
	if (waitIdle)
	{
		D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		ComPtr<ID3D11Query> event;
		if (SUCCEEDED(m_device->CreateQuery(&desc, &event)))
		{
			m_context->End(event.Get());
			while (m_context->GetData(event.Get(), nullptr, 0, 0) == S_FALSE) {}
		}
	}
}
void D3D11Renderer::NotifyLatteCommandProcessorIdle() { m_context->Flush(); }

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
		CheckDebugMessages("ImGui render");
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
		DriverCallTrace trace(fmt::format(
			"CreateTexture2D ImGui {}x{} RGB={} RGBA={}",
			size.x, size.y, data.size(), rgba.size()));
		if (FAILED(m_device->CreateTexture2D(&desc, &initial, &texture)))
			return nullptr;
	}
	ID3D11ShaderResourceView* view{};
	if (FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
		return nullptr;
	CheckDebugMessages("ImGui texture creation");
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
void D3D11Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) { delete fbo; }

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

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			current[slot]->GetDesc(&viewDesc);
			ComPtr<ID3D11ShaderResourceView> snapshotView;
			m_context->CopyResource(snapshot.Get(), source.Get());
			if (FAILED(m_device->CreateShaderResourceView(snapshot.Get(), &viewDesc, &snapshotView)))
				continue;
			ID3D11ShaderResourceView* replacement = snapshotView.Get();
			setResources(slot, &replacement);
			m_feedbackResources.emplace_back(std::move(snapshot));
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
void D3D11Renderer::texture_releaseTextureUploadBuffer(uint8*) {}

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
		DriverCallTrace trace(fmt::format(
			"UpdateSubresource texture={} sub={} mip={} slice={} box={}x{}{} rowPitch={} bytes={}",
			static_cast<const void*>(d3d->Resource()), subresource, mipIndex, sliceIndex,
			uploadWidth, uploadHeight,
			info.compressed ? " full-bc" : (destinationBoxPtr ? "" : " full-depth"),
			uploadRowPitch, uploadSize));
		m_context->UpdateSubresource(d3d->Resource(), subresource, destinationBoxPtr, uploadPixels,
			uploadRowPitch, static_cast<UINT>((std::min<uint64>)(uploadSize, UINT_MAX)));
	}
}

void D3D11Renderer::texture_clearColorSlice(LatteTexture* texture, sint32 slice, sint32 mip,
	float r, float g, float b, float a)
{
	auto* view = static_cast<D3D11TextureView*>(texture->GetOrCreateView(mip, 1, slice, 1));
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
	if (width <= 0 || height <= 0 || depth <= 0)
		return;
	const bool source3D = src->Is3DTexture();
	const bool destination3D = dst->Is3DTexture();
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
			D3D11CalcSubresource(srcMip, srcSlice + layer, src->mipLevels);
		const UINT destinationSubresource = destination3D ? static_cast<UINT>(dstMip) :
			D3D11CalcSubresource(dstMip, dstSlice + layer, dst->mipLevels);
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
	CheckDebugMessages(destination->isDepth ?
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
	if (!m_bufferCache || !buffer || size <= 0 || offset + size > m_bufferCacheShadow.size())
		return;
	std::memcpy(m_bufferCacheShadow.data() + offset, buffer, size);
	D3D11_BOX box{ offset, 0, 0, offset + static_cast<UINT>(size), 1, 1 };
	{
		DriverCallTrace trace(fmt::format(
			"UpdateSubresource buffer-cache offset={} size={} source={}",
			offset, size, static_cast<const void*>(buffer)));
		m_context->UpdateSubresource(m_bufferCache.Get(), 0, &box, buffer, 0, 0);
	}
}

void D3D11Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	if (srcOffset + size > m_bufferCacheShadow.size() || dstOffset + size > m_bufferCacheShadow.size())
		return;
	std::memmove(m_bufferCacheShadow.data() + dstOffset, m_bufferCacheShadow.data() + srcOffset, size);
	D3D11_BOX box{ dstOffset, 0, 0, dstOffset + size, 1, 1 };
	{
		DriverCallTrace trace(fmt::format(
			"UpdateSubresource buffer-copy src={} dst={} size={}", srcOffset, dstOffset, size));
		m_context->UpdateSubresource(m_bufferCache.Get(), 0, &box,
			m_bufferCacheShadow.data() + dstOffset, 0, 0);
	}
}
void D3D11Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 src, uint32 dst, uint32 size)
{
	if (!m_bufferCache || !size ||
		src + size > LatteStreamout_GetRingBufferSize() ||
		dst + size > m_bufferCacheShadow.size())
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
		return;
	D3D11_BOX box{ src, 0, 0, src + size, 1, 1 };
	m_context->CopySubresourceRegion(m_bufferCache.Get(), 0, dst, 0, 0,
		sourceBuffer, 0, &box);
}

void D3D11Renderer::buffer_bindVertexBuffer(uint32 index, uint32 offset, uint32 size)
{
	if (index >= m_vertexBuffers.size())
		return;
	m_vertexBuffers[index] = size ? m_bufferCache : nullptr;
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
	if (size && offset + size <= m_bufferCacheShadow.size())
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = Align16(size);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		std::vector<uint8> initial(desc.ByteWidth);
		std::memcpy(initial.data(), m_bufferCacheShadow.data() + offset, size);
		D3D11_SUBRESOURCE_DATA data{ initial.data(), 0, 0 };
		if (SUCCEEDED(m_device->CreateBuffer(&desc, &data, &m_uniformBuffers[stageIndex][index])))
			native = m_uniformBuffers[stageIndex][index].Get();
	}
	else
		m_uniformBuffers[stageIndex][index].Reset();
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
	auto* shader = new D3D11Shader(m_device.Get(), type, baseHash, auxHash, isGfxPackSource, source);
	if (!shader->IsCompiled())
	{
		delete shader;
		return nullptr;
	}
	return shader;
}

void D3D11Renderer::streamout_setupXfbBuffer(uint32 index, sint32 ringBufferOffset, uint32, uint32 rangeSize)
{
	if (index >= LATTE_NUM_STREAMOUT_BUFFER)
		return;
	m_streamoutEnabled[index] = rangeSize != 0;
	m_streamoutOffsets[index] = static_cast<UINT>((std::max)(ringBufferOffset, 0));
}

void D3D11Renderer::streamout_begin()
{
	auto* gsContext = LatteSHRC_GetActiveGeometryShader();
	auto* vsContext = LatteSHRC_GetActiveVertexShader();
	auto* shader = gsContext ? static_cast<D3D11Shader*>(gsContext->shader) :
		(vsContext ? static_cast<D3D11Shader*>(vsContext->shader) : nullptr);
	if (!shader || !shader->StreamoutGeometry())
	{
		cemuLog_log(LogType::Force, "D3D11 stream output was requested but its shader signature is unavailable");
		return;
	}
	std::array<ID3D11Buffer*, LATTE_NUM_STREAMOUT_BUFFER> buffers{};
	for (UINT i = 0; i < buffers.size(); ++i)
		buffers[i] = m_streamoutEnabled[i] ? m_streamoutBuffers[i].Get() : nullptr;
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

	std::string source;
	source.append("#version 450\r\n");
	source.append("layout(triangles) in;\r\n");
	source.append("layout(triangle_strip) out;\r\n");
	source.append("layout(max_vertices = 4) out;\r\n");
	const auto parameterMask = vertexShader->outputParameterMask;
	for (sint32 direction = 0; direction < 2; ++direction)
	{
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
			source.append(fmt::format("layout(location = {}) ",
				inputTable->getPSImportLocationBySemanticId(semantic)));
			if (pixelImport->isFlat)
				source.append("flat ");
			if (pixelImport->isNoPerspective)
				source.append("noperspective ");
			source.append(direction == 0 ? "in" : "out");
			source.append(direction == 0 ?
				fmt::format(" vec4 passParameterSem{}In[];\r\n", semantic) :
				fmt::format(" vec4 passParameterSem{}Out;\r\n", semantic));
		}
	}
	source.append(
		"vec4 gen4thVertexA(vec4 a,vec4 b,vec4 c){return b-(c-a);}\r\n"
		"vec4 gen4thVertexB(vec4 a,vec4 b,vec4 c){return c-(b-a);}\r\n"
		"vec4 gen4thVertexC(vec4 a,vec4 b,vec4 c){return c+(b-a);}\r\n"
		"void main(){\r\n"
		"float dist0_1=length(gl_in[1].gl_Position.xy-gl_in[0].gl_Position.xy);\r\n"
		"float dist0_2=length(gl_in[2].gl_Position.xy-gl_in[0].gl_Position.xy);\r\n"
		"float dist1_2=length(gl_in[2].gl_Position.xy-gl_in[1].gl_Position.xy);\r\n"
		"if(dist0_1>dist0_2&&dist0_1>dist1_2){\r\n");
	rectsEmulationGS_outputVerticesCode(source, vertexShader, inputTable,
		2, 1, 0, 3, "A", LatteGPUState.contextNew);
	source.append("}else if(dist0_2>dist0_1&&dist0_2>dist1_2){\r\n");
	rectsEmulationGS_outputVerticesCode(source, vertexShader, inputTable,
		1, 2, 0, 3, "B", LatteGPUState.contextNew);
	source.append("}else{\r\n");
	rectsEmulationGS_outputVerticesCode(source, vertexShader, inputTable,
		0, 1, 2, 3, "C", LatteGPUState.contextNew);
	source.append("}\r\n}\r\n");

	auto shader = std::make_unique<D3D11Shader>(m_device.Get(),
		RendererShader::ShaderType::kGeometry, key, 0, false, source);
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

void D3D11Renderer::UpdateUniformVars(LatteDecompilerShader* shader, uint32 verticesPerInstance)
{
	if (!shader || shader->resourceMapping.uniformVarsBufferBindingPoint < 0 ||
		shader->uniform.uniformRangeSize == 0)
		return;
	std::vector<uint8> bytes(Align16(shader->uniform.uniformRangeSize));
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

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(bytes.size());
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA initial{ bytes.data(), 0, 0 };
	const uint32 stage = static_cast<uint32>(shader->shaderType);
	if (FAILED(m_device->CreateBuffer(&desc, &initial, &m_uniformVarsBuffers[stage])))
		return;
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

void D3D11Renderer::UpdateInputLayout()
{
	auto* fetch = LatteSHRC_GetActiveFetchShader();
	auto* shaderContext = LatteSHRC_GetActiveVertexShader();
	auto* shader = shaderContext ? static_cast<D3D11Shader*>(shaderContext->shader) : nullptr;
	if (!fetch || !shader || !shader->Bytecode())
		return;
	const uint64 key = fetch->key ^ shaderContext->baseHash ^ (shaderContext->auxHash << 1);
	if (m_inputLayout && m_inputLayoutKey == key)
		return;
	std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
	for (const auto& group : fetch->bufferGroups)
	{
		for (sint32 i = 0; i < group.attribCount; ++i)
		{
			const auto& attribute = group.attrib[i];
			const sint32 location = shaderContext->resourceMapping.getAttribHostShaderIndex(attribute.semanticId);
			if (location < 0)
				continue;
			D3D11_INPUT_ELEMENT_DESC element{};
			element.SemanticName = "TEXCOORD";
			element.SemanticIndex = location;
			element.Format = VertexFormat(attribute.format);
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
	if (!elements.empty())
	{
		DriverCallTrace trace(fmt::format("CreateInputLayout elements={} shader={:016x}_{:016x}",
			elements.size(), shaderContext->baseHash, shaderContext->auxHash));
		const HRESULT hr = m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
			shader->Bytecode()->GetBufferPointer(), shader->Bytecode()->GetBufferSize(), &m_inputLayout);
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force, "D3D11 input layout creation failed (0x{:08X})", static_cast<uint32>(hr));
			return;
		}
	}
	m_inputLayoutKey = key;
	m_context->IASetInputLayout(m_inputLayout.Get());
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
	rasterizer.DepthClipEnable = !registers.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
	rasterizer.ScissorEnable = TRUE;
	rasterizer.MultisampleEnable = FALSE;
	rasterizer.AntialiasedLineEnable = FALSE;
	if (registers.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED())
	{
		rasterizer.DepthBias = static_cast<INT>(
			registers.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET() * 16.0f);
		rasterizer.SlopeScaledDepthBias =
			registers.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
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
			target.SrcBlendAlpha = BlendFactor(source.get_ALPHA_SRCBLEND());
			target.DestBlendAlpha = BlendFactor(source.get_ALPHA_DSTBLEND());
			target.BlendOpAlpha = BlendOp(source.get_ALPHA_COMB_FCN());
		}
		else
		{
			target.SrcBlendAlpha = target.SrcBlend;
			target.DestBlendAlpha = target.DestBlend;
			target.BlendOpAlpha = target.BlendOp;
		}
	}
	const auto gx2LogicOp = registers.CB_COLOR_CONTROL.get_ROP();
	const bool logicOpEnabled = gx2LogicOp != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
	for (auto& target : blend.RenderTarget)
	{
		target.LogicOpEnable = logicOpEnabled;
		target.LogicOp = LogicOp(gx2LogicOp);
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
		UpdateInputLayout();
	}
	if (LatteGPUState.activeShaderHasError || !HasRequiredShaders())
		return;

	const LattePrimitiveMode primitive = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	Renderer::INDEX_TYPE hostIndexType{};
	uint32 hostIndexCount{}, indexMax{};
	Renderer::IndexAllocation allocation{};
	const void* indices = indexDataMPTR != MPTR_NULL ? memory_getPointerFromPhysicalOffset(indexDataMPTR) : nullptr;
	LatteIndices_decode(indices, indexType, count, primitive, indexMax, hostIndexType, hostIndexCount, allocation);
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
	ApplyPipelineState();
	m_context->IASetPrimitiveTopology(PrimitiveTopology(primitive));
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	const bool rasterizerKilled =
		LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() &&
		LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	const bool bothFacesCulled =
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_FRONT() &&
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_BACK();
	const bool skipRasterDraw = (rasterizerKilled || bothFacesCulled) && !m_streamoutActive;

	if (skipRasterDraw)
	{
		// Vulkan represents both states directly. D3D11 has no rasterizer-discard
		// switch and no FRONT_AND_BACK cull mode, so a draw without stream output
		// is a true no-op. Stream-output shaders use NO_RASTERIZED_STREAM above.
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
				hostIndexType == INDEX_TYPE::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
			{
				DriverCallTrace trace(fmt::format(
					"DrawIndexedInstanced indices={} instances={} baseVertex={} baseInstance={} buffer={}",
					hostIndexCount, instanceCount, baseVertex, baseInstance,
					static_cast<const void*>(buffer)));
				m_context->DrawIndexedInstanced(hostIndexCount, instanceCount, 0, baseVertex, baseInstance);
			}
			CheckDebugMessages("indexed GX2 draw");
		}
		// LatteIndices_decode stores this allocation in its LRU cache. The cache
		// releases it when the entry is evicted; releasing it after every draw
		// leaves a dangling cached pointer and causes the next cache hit to bind
		// a null/freed ID3D11Buffer.
	}
	else
	{
		DriverCallTrace trace(fmt::format(
			"DrawInstanced vertices={} instances={} baseVertex={} baseInstance={}",
			count, instanceCount, baseVertex, baseInstance));
		m_context->DrawInstanced(count, instanceCount, baseVertex, baseInstance);
		CheckDebugMessages("non-indexed GX2 draw");
	}

	if (LatteSHRC_GetActivePixelShader())
		LatteRenderTarget_trackUpdates();
	LatteStreamout_FinishDrawcall(false);
	LatteGPUState.drawCallCounter++;
	LatteTextureReadback_Update();
}

void D3D11Renderer::draw_endSequence() {}

Renderer::IndexAllocation D3D11Renderer::indexData_reserveIndexMemory(uint32 size)
{
	auto* allocation = new IndexBufferAllocation();
	allocation->data.resize(size);
	return { allocation->data.data(), allocation };
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
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(data->data.size());
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	D3D11_SUBRESOURCE_DATA initial{ data->data.data(), 0, 0 };
	DriverCallTrace trace(fmt::format("CreateBuffer index bytes={} source={}",
		data->data.size(), static_cast<const void*>(data->data.data())));
	ThrowIfFailed(m_device->CreateBuffer(&desc, &initial, &data->buffer), "Create index buffer");
}

LatteQueryObject* D3D11Renderer::occlusionQuery_create() { return new D3D11Query(m_device.Get(), m_context.Get()); }
void D3D11Renderer::occlusionQuery_destroy(LatteQueryObject* query) { delete query; }
void D3D11Renderer::occlusionQuery_flush() { Flush(true); }
void D3D11Renderer::occlusionQuery_updateState() {}
