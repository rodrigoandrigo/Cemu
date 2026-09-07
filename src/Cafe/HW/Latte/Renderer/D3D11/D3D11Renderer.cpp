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
#include "util/helpers/helpers.h"

#include <backends/imgui_impl_dx11.h>
#include <d3dcompiler.h>
#include <Psapi.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_hlsl.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
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
#include "util/helpers/Semaphore.h"

using Microsoft::WRL::ComPtr;

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;
extern std::atomic_int g_compiling_pipelines;

// Implemented by the common renderer. GX2 uses this special state to clear
// color aliases of a depth surface without issuing a normal shader draw.
void LatteDraw_handleSpecialState8_clearAsDepth();

// The implementation is grouped by responsibility while remaining in one
// translation unit. Several native resource types intentionally live in an
// anonymous namespace and are shared across these implementation sections.
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererUtilities.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererShader.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererResources.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererCore.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererRenderTarget.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererTexture.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererBuffer.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererStreamout.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererDraw.inl"
#include "Cafe/HW/Latte/Renderer/D3D11/D3D11RendererQuery.inl"
