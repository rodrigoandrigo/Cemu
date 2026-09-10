class D3D11Shader final : public RendererShader
{
public:
	D3D11Shader(ID3D11Device* device, ShaderType type, uint64 baseHash, uint64 auxHash,
		bool isGameShader, bool isGfxPack, const std::string& source, bool compileAsync)
		: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPack), m_device(device),
		  m_source(source), m_isCacheRestore(compileAsync)
	{
		StartCompilation(compileAsync);
	}
	D3D11Shader(ID3D11Device* device, ShaderType type, uint64 baseHash, uint64 auxHash,
		bool isGameShader, bool isGfxPack, const std::string& source, bool sourceIsHlsl, bool compileAsync)
		: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPack), m_device(device),
		  m_source(source), m_sourceIsHlsl(sourceIsHlsl), m_isCacheRestore(compileAsync)
	{
		StartCompilation(compileAsync);
	}
	~D3D11Shader() override { D3D11ShaderQueueCancel(this); }

	void PreponeCompilation(bool) override
	{
		if (m_compilationState.hasState(CompilationState::Done))
		{
			if (m_retryableCompilationFailure && !m_source.empty())
			{
				m_compilationState.setValue(CompilationState::Compiling);
				CompileNow();
				m_compilationState.setValue(CompilationState::Done);
			}
			return;
		}
		if (D3D11ShaderQueuePromote(this))
		{
			CompileNow();
			m_compilationState.setValue(CompilationState::Done);
			return;
		}
		m_compilationState.waitUntilValue(CompilationState::Done);
		if (m_isCacheRestore && m_compiled.load(std::memory_order_acquire))
			--g_compiled_shaders_async;
	}
	bool IsCompiled() override
	{
		return m_compilationState.hasState(CompilationState::Done) &&
			m_compiled.load(std::memory_order_acquire);
	}
	bool WaitForCompiled() override
	{
		m_compilationState.waitUntilValue(CompilationState::Done);
		return m_compiled.load(std::memory_order_acquire);
	}
	ID3D11VertexShader* Vertex() const { return m_vs.Get(); }
	ID3D11PixelShader* Pixel() const { return m_ps.Get(); }
	ID3D11GeometryShader* Geometry() const { return m_gs.Get(); }
	ID3D11GeometryShader* StreamoutGeometry(bool rasterized = false) const
	{
		return rasterized && m_streamoutRasterizedGs ?
			m_streamoutRasterizedGs.Get() : m_streamoutGs.Get();
	}
	bool HasRasterizedStreamoutGeometry() const { return m_streamoutRasterizedGs != nullptr; }
	bool HasPixelStreamoutCapture() const
	{
		return m_pixelStreamoutCaptureVs && !m_pixelStreamoutCapturePasses.empty();
	}
	ID3D11VertexShader* PixelStreamoutCaptureVertex() const
	{
		return m_pixelStreamoutCaptureVs.Get();
	}
	UINT PixelStreamoutCapturePassCount() const
	{
		return static_cast<UINT>(m_pixelStreamoutCapturePasses.size());
	}
	ID3D11GeometryShader* PixelStreamoutCaptureGeometry(UINT index) const
	{
		return index < m_pixelStreamoutCapturePasses.size() ?
			m_pixelStreamoutCapturePasses[index].geometry.Get() : nullptr;
	}
	ID3D11PixelShader* PixelStreamoutCapturePixel(UINT index) const
	{
		return index < m_pixelStreamoutCapturePasses.size() ?
			m_pixelStreamoutCapturePasses[index].pixel.Get() : nullptr;
	}
	UINT PixelStreamoutCaptureStride(UINT buffer) const
	{
		return buffer < m_pixelStreamoutCaptureStrides.size() ?
			m_pixelStreamoutCaptureStrides[buffer] : 0;
	}
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
	friend class D3D11ShaderCompilerQueue;

	enum class CompilationState : uint8
	{
		None,
		Queued,
		Compiling,
		Done,
	};

	void StartCompilation(bool compileAsync)
	{
		if (compileAsync)
		{
			m_compilationState.setValue(CompilationState::Queued);
			D3D11ShaderQueueSubmit(this);
			return;
		}
		m_compilationState.setValue(CompilationState::Compiling);
		CompileNow();
		m_compilationState.setValue(CompilationState::Done);
	}

	void CompileNow()
	{
#if defined(CEMU_UWP)
		// Xbox exposes one compiler/translation memory budget to the whole title.
		// This lock is shared by foreground promotion and low-priority cache work.
		std::lock_guard pipelineLock(s_xboxShaderPipelineMutex);
#endif
		m_retryableCompilationFailure = false;
		try
		{
			if (m_sourceIsHlsl)
				CompileHLSL(m_device.Get(), m_source);
			else
				Compile(m_device.Get(), m_source);
		}
		catch (const std::exception& ex)
		{
			m_compiled.store(false, std::memory_order_release);
			cemuLog_log(LogType::Force,
				"D3D11 shader {:016x}_{:016x} failed outside the compiler boundary: {}",
				m_baseHash, m_auxHash, ex.what());
		}
		catch (...)
		{
			m_compiled.store(false, std::memory_order_release);
			OutputDebugStringA("[Cemu/D3D11] unexpected shader compiler exception\n");
		}
		ReleaseTransientBytecode();
		if (m_compiled.load(std::memory_order_acquire) && m_isGameShader && !m_isCacheRestore)
			++g_compiled_shaders_total;
		if (!m_retryableCompilationFailure)
		{
			m_source.clear();
			m_source.shrink_to_fit();
		}
	}

	struct PixelStreamoutCapturePass
	{
		ComPtr<ID3D11GeometryShader> geometry;
		ComPtr<ID3D11PixelShader> pixel;
	};
	struct StreamoutBlock
	{
		UINT slot{};
		UINT stride{};
		UINT location{};
	};

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
		// XFB declarations live outside main() and do not participate in the raster
		// geometry program translated below. Older code rejected the complete shader
		// when it saw one, unnecessarily replacing valid GS logic with a topology-only
		// compatibility shader. Streamout itself is handled independently.
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
		// Preserve every GLSL uniform block as its own HLSL constant buffer. The
		// previous syntax path skipped blocks (it only copied scalar uniforms), so
		// otherwise valid geometry programs lost their cbank data during conversion.
		size_t blockCursor{};
		UINT geometryBlockIndex{};
		while ((blockCursor = source.find("UNIFORM_BUFFER_LAYOUT(", blockCursor)) !=
			std::string::npos && blockCursor < inputMarker)
		{
			UINT glBinding{}, ignoredSet{}, ignoredBinding{};
			if (std::sscanf(source.c_str() + blockCursor,
				"UNIFORM_BUFFER_LAYOUT(%u, %u, %u)", &glBinding, &ignoredSet,
				&ignoredBinding) != 3)
			{
				blockCursor += 22;
				continue;
			}
			const size_t open = source.find('{', blockCursor);
			const size_t close = open == std::string::npos ? std::string::npos :
				source.find('}', open);
			if (open == std::string::npos || close == std::string::npos || close > inputMarker)
				break;
			const UINT slot = UniformSlot(glBinding);
			if (slot != InvalidSlot)
			{
				std::string declarations = source.substr(open + 1, close - open - 1);
				static constexpr std::array<std::pair<std::string_view, std::string_view>, 12>
					blockTypes = {{ { "vec2", "float2" }, { "vec3", "float3" },
					{ "vec4", "float4" }, { "ivec2", "int2" }, { "ivec3", "int3" },
					{ "ivec4", "int4" }, { "uvec2", "uint2" }, { "uvec3", "uint3" },
					{ "uvec4", "uint4" }, { "mat2", "float2x2" },
					{ "mat3", "float3x3" }, { "mat4", "float4x4" } }};
				for (const auto& [glsl, hlslType] : blockTypes)
					ReplaceToken(declarations, glsl, hlslType);
				hlsl += fmt::format("cbuffer CemuGeometryBlock{} : register(b{})\n{{{}\n}};\n",
					geometryBlockIndex++, slot, declarations);
			}
			blockCursor = close + 1;
		}
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
		const bool writesLayer = source.find("gl_Layer") != std::string::npos;
		if (writesLayer)
			hlsl += "    nointerpolation uint layer : SV_RenderTargetArrayIndex;\n";
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
		ReplaceToken(body, "gl_PrimitiveIDIn", "cemuPrimitiveId");
		ReplaceToken(body, "gl_InvocationID", "cemuInvocationId");
		if (writesLayer)
			ReplaceToken(body, "gl_Layer", "cemuOutput.layer");
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
		uint32 invocationCount = 1;
		const bool usesInvocationId = source.find("gl_InvocationID") != std::string::npos;
		const size_t invocationsMarker = source.find("invocations=");
		if (invocationsMarker != std::string::npos)
			invocationCount = (std::max)(1u, static_cast<uint32>(std::strtoul(
				source.c_str() + invocationsMarker + 12, nullptr, 10)));
		const std::string instanceAttribute = usesInvocationId && invocationCount > 1 ?
			fmt::format("[instance({})]\n", invocationCount) : std::string{};
		const std::string invocationParameter = usesInvocationId ?
			", uint cemuInvocationId : SV_GSInstanceID" : std::string{};
		const std::string signature = fmt::format(
			"{}[maxvertexcount({})]\nvoid main({} GeometryInput inputVertices[{}], "
			"uint cemuPrimitiveId : SV_PrimitiveID{}, inout {}<GeometryOutput> outputStream)",
			instanceAttribute,
			maxVertices, inputPrimitive,
			std::string_view(inputPrimitive) == "point" ? 1 : std::string_view(inputPrimitive) == "line" ? 2 :
			std::string_view(inputPrimitive) == "lineadj" ? 4 : std::string_view(inputPrimitive) == "triangleadj" ? 6 : 3,
			invocationParameter, streamType);
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
		if (m_compiled.load(std::memory_order_acquire))
			cemuLog_logOnce(LogType::Force,
				"D3D11 native geometry shader {:016x}_{:016x}: translated automatically from Cemu GLSL",
				m_baseHash, m_auxHash);
		return m_compiled.load(std::memory_order_acquire);
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
		return m_compiled.load(std::memory_order_acquire);
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
		if (m_compiled.load(std::memory_order_acquire))
		{
			cemuLog_logOnce(LogType::Force,
				"D3D11 native geometry shader {:016x}_{:016x}: using exact point-sprite HLSL translation",
				m_baseHash, m_auxHash);
		}
		return m_compiled.load(std::memory_order_acquire);
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
			m_compiled = false;
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
#if defined(CEMU_UWP)
			std::string pixelStreamoutCaptureHlsl;
#endif
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
			// XFB locations are allocated after all regular stage outputs by the
			// decompiler, so filtering the trailing interface range preserves position
			// and every Wii U varying used by the next stage. The unfiltered variant is
			// compiled separately for the pixel-UAV fallback after resource bindings
			// have been mapped below.
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
			if (!resources.storage_buffers.empty())
				throw std::runtime_error(
					"shader-storage buffers are unavailable at Direct3D Feature Level 11.0");
			// SPIRV-Cross' HLSL backend recognizes geometry-stage reflection but
			// cannot emit its entry point or stream operations. Calling compile()
			// therefore throws CompilerError("Unsupported shader stage"). Keep the
			// D3D11 pipeline valid with a reflected topology/interface preserving
			// shader instead of losing the stage (or stopping in the debugger).
			if (executionModel == spv::ExecutionModelGeometry)
			{
				// The generic HLSL backend cannot emit a geometry entry point. The syntax
				// translator below lowers the geometry entry point explicitly.
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
			if (m_compiled.load(std::memory_order_acquire) && !CreateStreamoutShader(device, source))
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
#if defined(CEMU_UWP)
			if (GetType() == ShaderType::kVertex && firstXfbLocation != UINT_MAX)
			{
				// The normal Xbox vertex function must omit XFB outputs, but the
				// pixel-UAV fallback needs their original bit-exact values. Generate a
				// private vertex function with only the XFB interface; it is never
				// bound in the title's raster pipeline. Omitting ordinary varyings
				// keeps that private function below Xbox's strict output-signature
				// limits.
				auto allActiveInterfaces = compiler.get_active_interface_variables();
				auto captureInterfaces = allActiveInterfaces;
				for (const auto& output : resources.stage_outputs)
				{
					if (compiler.has_decoration(output.id, spv::DecorationLocation) &&
						compiler.get_decoration(output.id, spv::DecorationLocation) < firstXfbLocation)
						captureInterfaces.erase(output.id);
				}
				compiler.set_enabled_interface_variables(std::move(captureInterfaces));
				try
				{
					pixelStreamoutCaptureHlsl = compiler.compile();
				}
				catch (const std::exception& ex)
				{
					// The fallback is optional. Failing to translate its private vertex
					// function must not reject the ordinary raster shader.
					cemuLog_logOnce(LogType::Force,
						"D3D11 Xbox pixel stream-output translation skipped for {:016x}_{:016x}: {}",
						m_baseHash, m_auxHash, ex.what());
				}
				auto rasterInterfaces = std::move(allActiveInterfaces);
				for (const auto& output : resources.stage_outputs)
				{
					if (compiler.has_decoration(output.id, spv::DecorationLocation) &&
						compiler.get_decoration(output.id, spv::DecorationLocation) >= firstXfbLocation)
						rasterInterfaces.erase(output.id);
				}
				compiler.set_enabled_interface_variables(std::move(rasterInterfaces));
			}
#endif
			hlsl = compiler.compile();
			hlsl = AddRuntimeSamplerSwizzles(std::move(hlsl), uniformSlot, m_usesRuntimeSwizzle);
			if (m_usesRuntimeSwizzle)
				m_samplerSwizzleSlot = uniformSlot;
#if defined(CEMU_UWP)
			if (!pixelStreamoutCaptureHlsl.empty())
			{
				bool usesCaptureRuntimeSwizzle{};
				pixelStreamoutCaptureHlsl = AddRuntimeSamplerSwizzles(
					std::move(pixelStreamoutCaptureHlsl), uniformSlot, usesCaptureRuntimeSwizzle);
			}
#endif
			}
			std::vector<uint32>().swap(spirv);
			ComPtr<ID3DBlob> errors;
			HRESULT hr = CompileHLSLCached(hlsl.data(), hlsl.size(), profile,
				RuntimeShaderCompileFlags(),
				&m_bytecode, &errors);
			if (FAILED(hr) && GetType() == ShaderType::kFragment && errors)
			{
				const std::string_view message(
					static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
				if (message.find("X3570") != std::string_view::npos &&
					message.find("X3511") != std::string_view::npos)
				{
					std::string dynamicLoopHlsl = ForceDynamicHlslLoops(hlsl);
					errors.Reset();
					hr = CompileHLSLCached(dynamicLoopHlsl.data(), dynamicLoopHlsl.size(), profile,
						RuntimeShaderCompileFlags(), &m_bytecode, &errors);
					if (SUCCEEDED(hr))
					{
						hlsl = std::move(dynamicLoopHlsl);
						cemuLog_logOnce(LogType::Force,
							"D3D11 fragment shader {:016x}_{:016x} recovered with dynamic gradient loops",
							m_baseHash, m_auxHash);
					}
				}
			}
			if (FAILED(hr))
			{
				if (hr == E_OUTOFMEMORY)
					throw std::bad_alloc{};
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
#if defined(CEMU_UWP)
				if (!pixelStreamoutCaptureHlsl.empty())
					CreatePixelStreamoutCaptureVertex(device, pixelStreamoutCaptureHlsl);
#endif
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
			OutputDebugStringA("[Cemu/D3D11] Shader compilation deferred: out of memory; source retained for retry\n");
			m_retryableCompilationFailure = true;
			m_compiled = false;
		}
		catch (const std::exception& ex)
		{
			m_compiled = false;
			cemuLog_log(LogType::Force, "D3D11 shader {:016x}_{:016x} failed: {}", m_baseHash, m_auxHash, ex.what());
		}
	}

	bool CreatePixelStreamoutCaptureVertex(ID3D11Device* device, const std::string& hlsl)
	{
		m_pixelStreamoutCaptureVs.Reset();
		m_pixelStreamoutCaptureBytecode.Reset();
		ComPtr<ID3DBlob> bytecode;
		ComPtr<ID3DBlob> errors;
		const HRESULT compileResult = CompileHLSLCached(hlsl.data(), hlsl.size(), "vs_5_0",
			RuntimeShaderCompileFlags(), &bytecode, &errors);
		if (FAILED(compileResult))
		{
			cemuLog_logOnce(LogType::Force,
				"D3D11 Xbox pixel stream-output vertex capture disabled for {:016x}_{:016x}: {}",
				m_baseHash, m_auxHash,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error");
			return false;
		}
		const HRESULT createResult = device->CreateVertexShader(bytecode->GetBufferPointer(),
			bytecode->GetBufferSize(), nullptr, &m_pixelStreamoutCaptureVs);
		if (FAILED(createResult))
		{
			cemuLog_logOnce(LogType::Force,
				"D3D11 Xbox pixel stream-output vertex capture creation failed for {:016x}_{:016x} (0x{:08X})",
				m_baseHash, m_auxHash, static_cast<uint32>(createResult));
			m_pixelStreamoutCaptureVs.Reset();
			return false;
		}
		m_pixelStreamoutCaptureBytecode = std::move(bytecode);
		return true;
	}

	bool CreatePixelStreamoutCaptureShader(ID3D11Device* device,
		const std::vector<StreamoutBlock>& blocks)
	{
		m_pixelStreamoutCapturePasses.clear();
		m_pixelStreamoutCaptureStrides.fill(0);
		auto disableCapture = [this]()
		{
			m_pixelStreamoutCapturePasses.clear();
			m_pixelStreamoutCaptureStrides.fill(0);
			m_pixelStreamoutCaptureVs.Reset();
			return true;
		};
		if (GetType() != ShaderType::kVertex)
		{
			// A feature-level 11.0 pixel-UAV replay can only be inserted after the
				// vertex stage. Feature Level 11.0 cannot expose UAV stores from the
				// geometry stage, so this case remains on the native fallback.
			cemuLog_logOnce(LogType::Force,
				"D3D11 Xbox pixel stream-output fallback cannot capture geometry shader {:016x}_{:016x}",
				m_baseHash, m_auxHash);
			return true;
		}
		ComPtr<ID3DBlob> captureBytecode = std::move(m_pixelStreamoutCaptureBytecode);
		if (!m_pixelStreamoutCaptureVs || !captureBytecode)
			return disableCapture();

		ComPtr<ID3D11ShaderReflection> reflection;
		if (FAILED(D3DReflect(captureBytecode->GetBufferPointer(), captureBytecode->GetBufferSize(),
			IID_PPV_ARGS(&reflection))))
			return disableCapture();
		D3D11_SHADER_DESC shaderDesc{};
		if (FAILED(reflection->GetDesc(&shaderDesc)))
			return disableCapture();
		std::vector<D3D11_SIGNATURE_PARAMETER_DESC> outputs(shaderDesc.OutputParameters);
		for (UINT i = 0; i < shaderDesc.OutputParameters; ++i)
		{
			if (FAILED(reflection->GetOutputParameterDesc(i, &outputs[i])))
				return disableCapture();
		}

		struct CaptureValue
		{
			UINT buffer{};
			UINT wordsPerVertex{};
			UINT wordOffset{};
			UINT semanticIndex{};
			std::string hlslType;
		};
		std::vector<CaptureValue> values;
		for (const auto& block : blocks)
		{
			if (block.slot >= LATTE_NUM_STREAMOUT_BUFFER || !block.stride ||
				(block.stride % sizeof(uint32)) != 0)
				return disableCapture();
			const UINT wordsPerVertex = block.stride / sizeof(uint32);
			if (wordsPerVertex > D3D11_SO_OUTPUT_COMPONENT_COUNT ||
				(m_pixelStreamoutCaptureStrides[block.slot] &&
					m_pixelStreamoutCaptureStrides[block.slot] != block.stride))
				return disableCapture();
			m_pixelStreamoutCaptureStrides[block.slot] = block.stride;
			for (UINT word = 0; word < wordsPerVertex; ++word)
			{
				const UINT semanticIndex = block.location + word;
				auto output = std::find_if(outputs.begin(), outputs.end(), [semanticIndex](const auto& value) {
					return value.SemanticName && _stricmp(value.SemanticName, "TEXCOORD") == 0 &&
						value.SemanticIndex == semanticIndex;
				});
				if (output == outputs.end() || output->Mask == 0)
					return disableCapture();
				const char* baseType = output->ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? "float" :
					output->ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" :
					output->ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : nullptr;
				if (!baseType)
					return disableCapture();
				values.push_back({ block.slot, wordsPerVertex, word, semanticIndex, baseType });
			}
		}
		if (values.empty())
			return disableCapture();

		for (size_t first = 0; first < values.size(); first += StreamoutPixelCaptureWordsPerPass)
		{
			const size_t count = (std::min)(values.size() - first,
				static_cast<size_t>(StreamoutPixelCaptureWordsPerPass));
			const UINT packedCount = static_cast<UINT>((count + 3) / 4);
			std::string geometryHlsl = R"HLSL(
cbuffer CemuStreamoutCapture : register(b0)
{
    uint4 cemuRecordState;
    uint4 cemuRingBase;
    uint4 cemuBufferLimit;
};
StructuredBuffer<uint> cemuRecordMap : register(t0);
struct CaptureInput
{
)HLSL";
			for (size_t index = 0; index < count; ++index)
			{
				const auto& value = values[first + index];
				geometryHlsl += fmt::format("    {} cemuValue{} : TEXCOORD{};\n",
					value.hlslType, index, value.semanticIndex);
			}
			geometryHlsl += "};\nstruct CaptureOutput\n{\n"
				"    float4 position : SV_Position;\n"
				"    nointerpolation uint cemuRecord : TEXCOORD0;\n";
			for (UINT packed = 0; packed < packedCount; ++packed)
				geometryHlsl += fmt::format("    nointerpolation uint4 cemuPacked{} : TEXCOORD{};\n",
					packed, packed + 1);
			geometryHlsl += R"HLSL(};
[maxvertexcount(1)]
void main(point CaptureInput inputVertices[1], uint primitiveId : SV_PrimitiveID,
          inout PointStream<CaptureOutput> outputStream)
{
    const uint vertexIndex = cemuRecordState.z != 0u ? cemuRecordMap[primitiveId] : primitiveId;
    const uint record = cemuRecordState.x + vertexIndex;
    if (record >= cemuRecordState.y)
        return;
    CaptureOutput output = (CaptureOutput)0;
    output.cemuRecord = record;
    const uint x = record & 2047u;
    const uint y = record >> 11u;
    output.position = float4((float(x) + 0.5f) / 1024.0f - 1.0f,
                             1.0f - (float(y) + 0.5f) / 512.0f, 0.0f, 1.0f);
)HLSL";
			for (size_t index = 0; index < count; ++index)
			{
				const auto& value = values[first + index];
				const std::string input = fmt::format("inputVertices[0].cemuValue{}", index);
				const std::string bits = value.hlslType == "float" ?
					fmt::format("asuint({})", input) : value.hlslType == "int" ?
					fmt::format("uint({})", input) : input;
				geometryHlsl += fmt::format("    output.cemuPacked{}.{} = {};\n",
					index / 4, "xyzw"[index % 4], bits);
			}
			geometryHlsl += "    outputStream.Append(output);\n}\n";

			std::string pixelHlsl = R"HLSL(
RWByteAddressBuffer cemuStreamout : register(u1);
cbuffer CemuStreamoutCapture : register(b0)
{
    uint4 cemuRecordState;
    uint4 cemuRingBase;
    uint4 cemuBufferLimit;
};
struct CaptureInput
{
    nointerpolation uint cemuRecord : TEXCOORD0;
)HLSL";
			for (UINT packed = 0; packed < packedCount; ++packed)
				pixelHlsl += fmt::format("    nointerpolation uint4 cemuPacked{} : TEXCOORD{};\n",
					packed, packed + 1);
			pixelHlsl += "};\nfloat4 main(CaptureInput input) : SV_Target\n{\n";
			for (size_t index = 0; index < count; ++index)
			{
				const auto& value = values[first + index];
				pixelHlsl += fmt::format(
					"    if (input.cemuRecord < cemuBufferLimit[{}]) cemuStreamout.Store((cemuRingBase[{}] + input.cemuRecord * {}u + {}u) * 4u, input.cemuPacked{}.{});\n",
					value.buffer, value.buffer, value.wordsPerVertex, value.wordOffset,
					index / 4, "xyzw"[index % 4]);
			}
			pixelHlsl += "    return float4(0.0f, 0.0f, 0.0f, 0.0f);\n}\n";

			ComPtr<ID3DBlob> geometryBytecode;
			ComPtr<ID3DBlob> errors;
			HRESULT result = CompileHLSLCached(geometryHlsl.data(), geometryHlsl.size(), "gs_5_0",
				RuntimeShaderCompileFlags(), geometryBytecode.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(result))
			{
				cemuLog_log(LogType::Force,
					"D3D11 Xbox pixel stream-output geometry capture failed for {:016x}_{:016x}: {}",
					m_baseHash, m_auxHash, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error");
				return disableCapture();
			}
			ComPtr<ID3D11GeometryShader> geometry;
			if (FAILED(device->CreateGeometryShader(geometryBytecode->GetBufferPointer(),
				geometryBytecode->GetBufferSize(), nullptr, &geometry)))
			{
				return disableCapture();
			}
			errors.Reset();
			ComPtr<ID3DBlob> pixelBytecode;
			result = CompileHLSLCached(pixelHlsl.data(), pixelHlsl.size(), "ps_5_0",
				RuntimeShaderCompileFlags(), pixelBytecode.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(result))
			{
				cemuLog_log(LogType::Force,
					"D3D11 Xbox pixel stream-output shader capture failed for {:016x}_{:016x}: {}",
					m_baseHash, m_auxHash, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown HLSL error");
				return disableCapture();
			}
			ComPtr<ID3D11PixelShader> pixel;
			if (FAILED(device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
				pixelBytecode->GetBufferSize(), nullptr, &pixel)))
			{
				return disableCapture();
			}
			m_pixelStreamoutCapturePasses.push_back({ std::move(geometry), std::move(pixel) });
		}
		cemuLog_logOnce(LogType::Force,
			"D3D11 Xbox pixel-UAV stream-output fallback active for vertex shader {:016x}_{:016x}",
			m_baseHash, m_auxHash);
		return true;
	}

	bool CreateStreamoutShader(ID3D11Device* device, const std::string& source)
	{
		std::vector<StreamoutBlock> blocks;
		size_t cursor{};
		while ((cursor = source.find("XFB_BLOCK_LAYOUT(", cursor)) != std::string::npos)
		{
			StreamoutBlock block{};
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
		// Vertex-only transform feedback uses the pixel-UAV replay. A title geometry
		// shader cannot be replaced by that helper because its emitted vertices would
		// be lost. Feature Level 11.0 natively supports geometry stream output, so a
		// geometry stage continues into the corrected scalar/stride declaration below.
		if (GetType() == ShaderType::kVertex)
			return CreatePixelStreamoutCaptureShader(device, blocks);
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
		// All declarations above belong to stream zero. Multiplying the limit by
		// the number of streams accepted declarations that the active stream alone
		// cannot represent and deferred the failure to D3D11On12's first draw.
		if (declarations.empty() || declarations.size() > D3D11_SO_OUTPUT_COMPONENT_COUNT)
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
		// When the GX2 draw also rasterizes, use stream zero as the rasterized
		// stream. This captures and renders in one invocation instead of running
		// the complete shader a second time. Keep the no-raster object above for
		// rasterizer-kill/cull and as a compatibility fallback.
		const HRESULT rasterizedHr = device->CreateGeometryShaderWithStreamOutput(
			m_bytecode->GetBufferPointer(), m_bytecode->GetBufferSize(),
			declarations.data(), static_cast<UINT>(declarations.size()),
			strides.data(), strideCount, 0, nullptr, &m_streamoutRasterizedGs);
		if (FAILED(rasterizedHr))
		{
			cemuLog_log(LogType::Force,
				"D3D11 rasterized stream-output shader unavailable (0x{:08X}); using replay",
				static_cast<uint32>(rasterizedHr));
		}
		return true;
	}

	ComPtr<ID3D11Device> m_device;
	std::string m_source;
	bool m_sourceIsHlsl{};
	bool m_isCacheRestore{};
	StateSemaphore<CompilationState> m_compilationState{ CompilationState::None };
	std::atomic_bool m_compiled{};
	ComPtr<ID3DBlob> m_bytecode;
	ComPtr<ID3D11VertexShader> m_vs;
	ComPtr<ID3D11PixelShader> m_ps;
	ComPtr<ID3D11GeometryShader> m_gs;
	ComPtr<ID3D11GeometryShader> m_streamoutGs;
	ComPtr<ID3D11GeometryShader> m_streamoutRasterizedGs;
	ComPtr<ID3D11VertexShader> m_pixelStreamoutCaptureVs;
	ComPtr<ID3DBlob> m_pixelStreamoutCaptureBytecode;
	std::array<UINT, 256> m_textureSlots{};
	std::array<UINT, 256> m_uniformSlots{};
	UINT m_samplerSwizzleSlot{ InvalidSlot };
	bool m_usesRuntimeSwizzle{};
	bool m_retryableCompilationFailure{};
	std::array<UINT, LATTE_NUM_STREAMOUT_BUFFER> m_pixelStreamoutCaptureStrides{};
	std::vector<PixelStreamoutCapturePass> m_pixelStreamoutCapturePasses;
};

// D3D11 has no portable equivalent of Vulkan's pipeline cache.  Creating a
// shader object can still trigger D3D11On12 work, so restore known shaders in
// one low-priority worker and promote the small subset needed by the current
// draw synchronously.  The queue owns no shaders: every owner either promotes
// or cancels its pending job before destruction.
class D3D11ShaderCompilerQueue
{
public:
	void Start()
	{
		std::lock_guard lock(m_mutex);
		if (m_running)
			return;
		m_running = true;
		m_thread = std::thread(&D3D11ShaderCompilerQueue::ThreadMain, this);
	}

	void Stop()
	{
		{
			std::lock_guard lock(m_mutex);
			if (!m_running)
				return;
			m_running = false;
			for (auto* shader : m_queue)
			{
				shader->m_compiled.store(false, std::memory_order_release);
				shader->m_compilationState.setValue(D3D11Shader::CompilationState::Done);
			}
			m_queue.clear();
		}
		m_condition.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void Submit(D3D11Shader* shader)
	{
		Start();
		{
			std::lock_guard lock(m_mutex);
			if (!m_running)
			{
				// Shutdown raced title-cache discovery. Do not leave a shader in
				// QUEUED without a worker that can ever complete it.
				shader->m_compiled.store(false, std::memory_order_release);
				shader->m_compilationState.setValue(D3D11Shader::CompilationState::Done);
				return;
			}
			m_queue.push_back(shader);
		}
		m_condition.notify_one();
	}

	bool Promote(D3D11Shader* shader)
	{
		std::lock_guard lock(m_mutex);
		if (!shader->m_compilationState.hasState(D3D11Shader::CompilationState::Queued))
			return false;
		auto it = std::find(m_queue.begin(), m_queue.end(), shader);
		if (it == m_queue.end())
			return false;
		m_queue.erase(it);
		shader->m_compilationState.setValue(D3D11Shader::CompilationState::Compiling);
		return true;
	}

	void Cancel(D3D11Shader* shader)
	{
		{
			std::lock_guard lock(m_mutex);
			if (shader->m_compilationState.hasState(D3D11Shader::CompilationState::Queued))
			{
				auto it = std::find(m_queue.begin(), m_queue.end(), shader);
				if (it != m_queue.end())
				{
					m_queue.erase(it);
					shader->m_compiled.store(false, std::memory_order_release);
					shader->m_compilationState.setValue(D3D11Shader::CompilationState::Done);
					return;
				}
			}
		}
		if (!shader->m_compilationState.hasState(D3D11Shader::CompilationState::Done))
			shader->m_compilationState.waitUntilValue(D3D11Shader::CompilationState::Done);
	}

	~D3D11ShaderCompilerQueue() { Stop(); }

private:
	void ThreadMain()
	{
		SetThreadName("d3d11ShaderComp");
#if defined(CEMU_UWP)
		// Cache restoration should fill otherwise idle time, not steal a core from
		// Latte's emulation and render threads.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
		while (true)
		{
			D3D11Shader* shader{};
			{
				std::unique_lock lock(m_mutex);
				m_condition.wait(lock, [this]() {
					// Speculative native restoration is allowed only while Cemu is on the
					// shader-loading screen. Once gameplay begins, queued shaders remain
					// dormant until PreponeCompilation promotes one for the current draw.
					return !m_running ||
						(!m_queue.empty() && LatteShaderCache_IsLoading());
				});
				if (!m_running)
					return;
				shader = m_queue.front();
				m_queue.pop_front();
				shader->m_compilationState.setValue(D3D11Shader::CompilationState::Compiling);
			}
			shader->CompileNow();
			if (shader->m_compiled.load(std::memory_order_acquire) && shader->m_isGameShader)
				++g_compiled_shaders_async;
			shader->m_compilationState.setValue(D3D11Shader::CompilationState::Done);
#if defined(CEMU_UWP)
			// Keep the loading UI responsive between startup cache entries. Once the
			// loading scope ends, the wait predicate above suspends speculative work.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::deque<D3D11Shader*> m_queue;
	std::thread m_thread;
	bool m_running{};
};

D3D11ShaderCompilerQueue s_d3d11ShaderCompilerQueue;

void D3D11ShaderQueueStart()
{
	s_d3d11ShaderCompilerQueue.Start();
}

void D3D11ShaderQueueStop()
{
	s_d3d11ShaderCompilerQueue.Stop();
}

void D3D11ShaderQueueSubmit(D3D11Shader* shader)
{
	s_d3d11ShaderCompilerQueue.Submit(shader);
}

bool D3D11ShaderQueuePromote(D3D11Shader* shader)
{
	return s_d3d11ShaderCompilerQueue.Promote(shader);
}

void D3D11ShaderQueueCancel(D3D11Shader* shader)
{
	s_d3d11ShaderCompilerQueue.Cancel(shader);
}
