namespace
{
// Feature level 11.0 permits UAV writes from the pixel shader, but not from
// VS/GS.  Xbox UWP exposes exactly that feature level.  The compatibility
// path below therefore replays the vertex stream as points and stores the XFB
// words from a pixel shader through this ordinary OM UAV slot.
constexpr UINT StreamoutPixelCaptureUavSlot = 1;
constexpr UINT StreamoutPixelCaptureWidth = 2048;
constexpr UINT StreamoutPixelCaptureHeight = 1024;
constexpr UINT StreamoutPixelCaptureCapacity =
	StreamoutPixelCaptureWidth * StreamoutPixelCaptureHeight;
constexpr UINT StreamoutPixelCaptureWordsPerPass = 32;

#if defined(CEMU_UWP)
// Serialize Cemu shader translation with the first draw that makes D3D11On12
// compile a native Xbox pipeline. Both paths have large transient footprints.
std::mutex s_xboxShaderPipelineMutex;
#endif

class D3D11Shader;
void D3D11ShaderQueueStart();
void D3D11ShaderQueueStop();
void D3D11ShaderQueueSubmit(D3D11Shader* shader);
bool D3D11ShaderQueuePromote(D3D11Shader* shader);
void D3D11ShaderQueueCancel(D3D11Shader* shader);

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

#define D3D11_DEBUG_CHECK(scope) CheckDebugMessages(scope)

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

std::string ForceDynamicHlslLoops(const std::string& source)
{
	// FXC requires dynamic flow control when implicit texture gradients occur in
	// a loop whose trip count varies per pixel. Without [loop], it tries to
	// unroll the loop and can reject otherwise valid Wii U shaders with X3511.
	// SPIRV-Cross emits control-flow statements at the start of a line, which
	// lets us annotate them without touching expressions, comments or macros.
	std::string result;
	result.reserve(source.size() + 256);
	size_t cursor{};
	bool previousLineIsLoopAttribute{};
	while (cursor < source.size())
	{
		const size_t lineEnd = source.find('\n', cursor);
		const size_t length = (lineEnd == std::string::npos ? source.size() : lineEnd) - cursor;
		const std::string_view line(source.data() + cursor, length);
		const size_t first = line.find_first_not_of(" \t\r");
		const std::string_view trimmed = first == std::string_view::npos ?
			std::string_view{} : line.substr(first);
		const bool isUnrollAttribute = trimmed.starts_with("[unroll") && trimmed.ends_with(']');
		const bool isLoop = trimmed.starts_with("for (") ||
			trimmed.starts_with("while (") || trimmed == "do";
		if (isLoop && !previousLineIsLoopAttribute)
		{
			result.append(line.data(), first);
			result += "[loop]\n";
		}
		if (isUnrollAttribute)
		{
			result.append(line.data(), first);
			result += "[loop]";
		}
		else
		{
			result.append(line.data(), line.size());
		}
		if (!trimmed.empty())
			previousLineIsLoopAttribute = isUnrollAttribute || trimmed == "[loop]";
		if (lineEnd == std::string::npos)
			break;
		result.push_back('\n');
		cursor = lineEnd + 1;
	}
	return result;
}

// Persistent driver-cache budget. This is disk-backed and does not reserve the
// same amount in the Series S process address space.
constexpr uint64 D3D11ShaderCacheBudgetBytes = 512ull * 1024 * 1024;

// Xbox grants the title roughly 5120 MiB. Keep the renderer's coordinated
// process-memory guards below that hard boundary while allowing substantially
// more of the available budget to be used.
constexpr uint64 D3D11ProcessMemoryLimitMB = 5020;
constexpr uint64 D3D11ShaderResumeMemoryMB = 4706;
constexpr uint64 D3D11MemoryReleaseLimitMB = 4863;
constexpr uint64 D3D11EmergencyMemoryMB = 5080;
constexpr uint64 D3D11CompilerCompactMemoryMB = 3765;

bool ValidateDxbcContainer(const void* data, size_t size)
{
	if (!data || size < 32 || size > D3D11ShaderCacheBudgetBytes)
		return false;
	const auto* bytes = static_cast<const uint8*>(data);
	if (std::memcmp(bytes, "DXBC", 4) != 0)
		return false;
	auto readU32 = [bytes](size_t offset)
	{
		uint32 value{};
		std::memcpy(&value, bytes + offset, sizeof(value));
		return value;
	};
	const uint32 declaredSize = readU32(24);
	const uint32 chunkCount = readU32(28);
	if (declaredSize != size || chunkCount == 0 || chunkCount > 64 ||
		32ull + static_cast<uint64>(chunkCount) * sizeof(uint32) > size)
		return false;
	for (uint32 index = 0; index < chunkCount; ++index)
	{
		const uint32 chunkOffset = readU32(32 + index * sizeof(uint32));
		if (static_cast<uint64>(chunkOffset) + 8 > size)
			return false;
		const uint32 chunkSize = readU32(static_cast<size_t>(chunkOffset) + 4);
		if (static_cast<uint64>(chunkOffset) + 8 + chunkSize > size)
			return false;
	}
	return true;
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
		length > static_cast<std::streamoff>(D3D11ShaderCacheBudgetBytes) ||
		(length % sizeof(uint32)) != 0)
		return false;
	spirv.resize(static_cast<size_t>(length) / sizeof(uint32));
	input.seekg(0);
	input.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(length));
	bool valid = input && spirv.size() > 5 && spirv[0] == 0x07230203u &&
		spirv[3] != 0 && spirv[4] == 0;
	bool hasMemoryModel = false;
	bool hasEntryPoint = false;
	uint32 functionCount = 0;
	uint32 functionEndCount = 0;
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
		const uint32 opcode = spirv[word] & 0xFFFFu;
		hasMemoryModel |= opcode == 14; // OpMemoryModel
		hasEntryPoint |= opcode == 15; // OpEntryPoint
		functionCount += opcode == 54; // OpFunction
		functionEndCount += opcode == 56; // OpFunctionEnd
		word += instructionWords;
		if (word == spirv.size())
			break;
	}
	valid = valid && hasMemoryModel && hasEntryPoint && functionCount != 0 &&
		functionCount == functionEndCount;
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
	const fs::path temporaryPath = path.string() + ".tmp";
	{
		std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
		if (!output)
			return;
		output.write(reinterpret_cast<const char*>(spirv.data()),
			static_cast<std::streamsize>(spirv.size() * sizeof(uint32)));
		output.flush();
		if (!output)
		{
			output.close();
			fs::remove(temporaryPath, error);
			return;
		}
	}
	// Never expose a partially written module to the next run. Windows does not
	// replace an existing destination with rename(), so remove the old cache entry
	// only after the complete temporary file has been closed.
	fs::rename(temporaryPath, path, error);
	if (error)
	{
		error.clear();
		fs::remove(path, error);
		error.clear();
		fs::rename(temporaryPath, path, error);
		if (error)
		{
			error.clear();
			fs::remove(temporaryPath, error);
		}
	}
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
			if (length >= 32 &&
				length <= static_cast<std::streamoff>(D3D11ShaderCacheBudgetBytes))
			{
				ComPtr<ID3DBlob> cached;
				if (SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(length), &cached)))
				{
					input.seekg(0);
					input.read(static_cast<char*>(cached->GetBufferPointer()),
						static_cast<std::streamsize>(length));
					const bool validDxbc = input && ValidateDxbcContainer(
						cached->GetBufferPointer(), cached->GetBufferSize());
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
	static uint32 xboxCacheMissCount{};
	const uint64 preCompileCommitBytes = QueryProcessPrivateCommitBytes();
	// HeapCompact is itself a global stop-the-world operation. At low pressure,
	// running it for every newly encountered shader makes scene transitions much
	// slower than the compilation. Keep periodic compaction and immediately use it
	// whenever commitment approaches the compiler's transient-risk range.
	if (((++xboxCacheMissCount & 15u) == 1u) ||
		preCompileCommitBytes >= D3D11CompilerCompactMemoryMB * 1024 * 1024)
		HeapCompact(GetProcessHeap(), 0);
	// A cache hit above remains safe and avoids invoking the Xbox compiler. For
	// a cache miss, reserve ample space below the title cap for xbsc_xs.dll's
	// transient working set. Series S traces showed one compilation interval grow
	// PrivateUsage by well over 1 GiB before the compiler faulted.
	constexpr uint64 shaderCompilerStopBytes = D3D11ProcessMemoryLimitMB * 1024 * 1024;
	if (QueryProcessPrivateCommitBytes() >= shaderCompilerStopBytes)
	{
		OutputDebugStringA(
			"[Cemu/D3D11] HLSL compilation continuing after emergency heap compaction; shader will not be discarded\n");
		HeapCompact(GetProcessHeap(), 0);
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
			const fs::path temporaryPath = path.string() + ".tmp";
			bool cacheWritten = false;
			{
				std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
				if (output)
				{
					output.write(static_cast<const char*>(compiled->GetBufferPointer()),
						static_cast<std::streamsize>(compiled->GetBufferSize()));
					output.flush();
					cacheWritten = output.good();
				}
			}
			if (cacheWritten)
			{
				fs::rename(temporaryPath, path, error);
				if (error)
				{
					error.clear();
					fs::remove(path, error);
					error.clear();
					fs::rename(temporaryPath, path, error);
				}
			}
			else
			{
				error.clear();
				fs::remove(temporaryPath, error);
			}
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
