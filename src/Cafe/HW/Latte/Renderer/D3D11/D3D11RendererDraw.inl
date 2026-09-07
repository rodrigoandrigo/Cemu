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
		RendererShader::ShaderType::kGeometry, key, 0, false, false, source, true, false);
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

bool D3D11Renderer::EnsureActiveShadersCompiled()
{
	const std::array<LatteDecompilerShader*, 3> contexts = {
		LatteSHRC_GetActiveVertexShader(),
		LatteSHRC_GetActivePixelShader(),
		LatteSHRC_GetActiveGeometryShader(),
	};
	for (auto* context : contexts)
	{
		auto* shader = context ? static_cast<D3D11Shader*>(context->shader) : nullptr;
		if (!shader)
			continue;
		// A cache worker may already be compiling this entry. Promotion either
		// claims a queued item or waits for that single in-flight job; it never
		// races the device or exposes a half-created native shader.
		shader->PreponeCompilation(true);
		if (!shader->IsCompiled())
			return false;
	}
	return true;
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

void D3D11Renderer::UpdateUniformVars(LatteDecompilerShader* shader, uint32 verticesPerInstance,
	bool fullUpdate, bool aluConstantsDirty, uint32 uniformBufferDirtyMask)
{
	if (!shader || shader->resourceMapping.uniformVarsBufferBindingPoint < 0 ||
		shader->uniform.uniformRangeSize == 0)
		return;
	const uint32 stage = static_cast<uint32>(shader->shaderType);
	if (stage >= m_uniformScratch.size())
		return;
	auto* nativeShader = static_cast<D3D11Shader*>(shader->shader);
	if (!nativeShader)
		return;
	uint64 shaderKey = HashBytes(&shader->shaderType, sizeof(shader->shaderType));
	const uint64 baseHash = nativeShader->BaseHash();
	const uint64 auxHash = nativeShader->AuxHash();
	shaderKey = HashBytes(&baseHash, sizeof(baseHash), shaderKey);
	shaderKey = HashBytes(&auxHash, sizeof(auxHash), shaderKey);
	auto& bytes = m_uniformScratch[stage];
	const uint32 alignedSize = Align16(shader->uniform.uniformRangeSize);
	fullUpdate |= bytes.size() != alignedSize || m_uniformShaderKeys[stage] != shaderKey;
	if (fullUpdate)
	{
		bytes.assign(alignedSize, 0);
		m_uniformShaderKeys[stage] = shaderKey;
	}
	auto dataAt = [&bytes](sint32 offset) { return bytes.data() + offset; };
	bool valuesChanged = fullUpdate;
	const auto updateValue = [&](sint32 offset, const void* value, size_t size)
	{
		if (offset < 0 || std::memcmp(dataAt(offset), value, size) == 0)
			return;
		std::memcpy(dataAt(offset), value, size);
		valuesChanged = true;
	};
	// Texture bindings, fixed-function state and viewport changes terminate a
	// fast-draw sequence in the common renderer. Populate those values only when
	// rebuilding the sequence; walking every texture-scale entry on thousands of
	// repeated Wind Waker draws was pure CPU overhead.
	if (fullUpdate)
	{
		for (auto& entry : shader->uniform.list_ufTexRescale)
		{
			float* scale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
			std::memcpy(entry.currentValue, scale, sizeof(float) * 2);
			updateValue(entry.uniformLocation, scale, sizeof(float) * 2);
		}
		const float alphaTestRef = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
		updateValue(shader->uniform.loc_alphaTestRef, &alphaTestRef, sizeof(alphaTestRef));
		const float rawPointSize = static_cast<float>(LatteGPUState.contextNew.PA_SU_POINT_SIZE.get_WIDTH()) / 8.0f;
		const float pointSize = rawPointSize == 0.0f ? 1.0f / 8.0f : rawPointSize;
		updateValue(shader->uniform.loc_pointSize, &pointSize, sizeof(pointSize));
		if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
		{
			sint32 width{}, height{};
			LatteRenderTarget_GetCurrentVirtualViewportSize(&width, &height);
			const float value[] = { 2.0f / (std::max)(width, 1), 2.0f / (std::max)(height, 1) };
			updateValue(shader->uniform.loc_windowSpaceToClipSpaceTransform, value, sizeof(value));
		}
		if (shader->uniform.loc_fragCoordScale >= 0)
		{
			float value[4]{};
			LatteMRT::GetCurrentFragCoordScale(value);
			updateValue(shader->uniform.loc_fragCoordScale, value, sizeof(value));
		}
	}
	updateValue(shader->uniform.loc_verticesPerInstance, &verticesPerInstance,
		sizeof(verticesPerInstance));
	for (UINT buffer = 0; buffer < LATTE_NUM_STREAMOUT_BUFFER; ++buffer)
		updateValue(shader->uniform.loc_streamoutBufferBase[buffer],
			&m_streamoutOffsets[buffer], sizeof(m_streamoutOffsets[buffer]));
	if (shader->uniform.loc_remapped >= 0)
		valuesChanged |= LatteBufferCache_LoadRemappedUniforms(shader,
			reinterpret_cast<float*>(dataAt(shader->uniform.loc_remapped)),
			fullUpdate || aluConstantsDirty,
			fullUpdate ? (1u << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1 : uniformBufferDirtyMask);
	if (shader->uniform.loc_uniformRegister >= 0 && (fullUpdate || aluConstantsDirty))
	{
		const sint32 registerOffset = shader->shaderType == LatteConst::ShaderType::Vertex ? 0x400 : 0;
		const uint32* registers = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + registerOffset;
		std::memcpy(dataAt(shader->uniform.loc_uniformRegister), registers, shader->uniform.count_uniformRegister * 16);
		valuesChanged = true;
	}

	if (valuesChanged && (!m_uniformVarsBuffers[stage] || !m_uniformScratchUploaded[stage] ||
		m_uploadedUniformScratch[stage] != bytes))
	{
		if (!UpdateDynamicConstantBuffer(m_uniformVarsBuffers[stage],
			m_uniformVarsBufferCapacity[stage], bytes.data(), static_cast<UINT>(bytes.size())))
			return;
		m_uploadedUniformScratch[stage] = bytes;
		m_uniformScratchUploaded[stage] = true;
	}
	// Map/Unmap updates the storage already bound to the stage. Fast draws keep
	// the same shader and resource layout, so rebinding the same constant buffer
	// here would add up to three D3D11On12 calls per draw (over twenty thousand
	// calls per Wind Waker frame) without changing GPU state.
	if (!fullUpdate)
		return;
	ID3D11Buffer* buffer = m_uniformVarsBuffers[stage].Get();
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
	if (rasterizer.CullMode == D3D11_CULL_NONE)
		// Winding has no effect without culling. Canonicalizing it avoids a second
		// D3D11On12 pipeline for an otherwise identical rasterizer state.
		rasterizer.FrontCounterClockwise = FALSE;
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
	if (m_appliedRasterizerState.Get() != rasterizerIt->second.Get())
	{
		m_context->RSSetState(rasterizerIt->second.Get());
		m_appliedRasterizerState = rasterizerIt->second;
	}

	D3D11_BLEND_DESC blend{};
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
		if (target.RenderTargetWriteMask == 0)
		{
			// All blend/logic fields are ignored when no component can be written.
			// Normalize them before hashing to keep register noise out of the native
			// D3D12 shader/pipeline cache used underneath D3D11On12.
			target.BlendEnable = FALSE;
		}
		// These fields are ignored when fixed-function blending is disabled. GX2
		// titles still vary their registers, which otherwise creates distinct
		// D3D11 state objects and D3D11On12 PSOs with identical behavior.
		if (!target.BlendEnable)
		{
			target.SrcBlend = D3D11_BLEND_ONE;
			target.DestBlend = D3D11_BLEND_ZERO;
			target.BlendOp = D3D11_BLEND_OP_ADD;
			target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlendAlpha = D3D11_BLEND_ZERO;
			target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		}
	}
	const uint64 blendKey = HashBytes(&blend, sizeof(blend));
	auto blendIt = m_blendCache.find(blendKey);
	if (blendIt == m_blendCache.end())
	{
		ComPtr<ID3D11BlendState> state;
		ThrowIfFailed(m_device->CreateBlendState(&blend, &state), "Create GX2 blend state");
		if (logicOpEnabled)
			cemuLog_logOnce(LogType::Force,
				"DirectX 11 Feature Level 11.0 has no logic operations; GX2 logic op {} will use COPY",
				static_cast<uint32>(gx2LogicOp));
		blendIt = m_blendCache.emplace(blendKey, std::move(state)).first;
	}
	float blendConstant[] = {
		registers.CB_BLEND_RED.get_RED(), registers.CB_BLEND_GREEN.get_GREEN(),
		registers.CB_BLEND_BLUE.get_BLUE(), registers.CB_BLEND_ALPHA.get_ALPHA()
	};
	const bool usesBlendConstant = std::any_of(std::begin(blend.RenderTarget),
		std::end(blend.RenderTarget), [](const auto& target)
		{
			if (!target.BlendEnable)
				return false;
			return target.SrcBlend == D3D11_BLEND_BLEND_FACTOR ||
				target.SrcBlend == D3D11_BLEND_INV_BLEND_FACTOR ||
				target.DestBlend == D3D11_BLEND_BLEND_FACTOR ||
				target.DestBlend == D3D11_BLEND_INV_BLEND_FACTOR ||
				target.SrcBlendAlpha == D3D11_BLEND_BLEND_FACTOR ||
				target.SrcBlendAlpha == D3D11_BLEND_INV_BLEND_FACTOR ||
				target.DestBlendAlpha == D3D11_BLEND_BLEND_FACTOR ||
				target.DestBlendAlpha == D3D11_BLEND_INV_BLEND_FACTOR;
		});
	if (!usesBlendConstant)
		std::fill(std::begin(blendConstant), std::end(blendConstant), 0.0f);
	const bool blendConstantChanged = !m_appliedBlendStateValid ||
		!std::equal(std::begin(blendConstant), std::end(blendConstant),
			m_appliedBlendConstant.begin());
	if (m_appliedBlendState.Get() != blendIt->second.Get() || blendConstantChanged)
	{
		m_context->OMSetBlendState(blendIt->second.Get(), blendConstant, 0xFFFFFFFF);
		m_appliedBlendState = blendIt->second;
		std::copy(std::begin(blendConstant), std::end(blendConstant),
			m_appliedBlendConstant.begin());
		m_appliedBlendStateValid = true;
	}

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
	// Canonicalize fields that D3D11 ignores. This prevents register noise from
	// multiplying native D3D12 pipeline variants without changing GX2 results.
	if (!depth.DepthEnable)
	{
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
	}
	if (!depth.StencilEnable)
	{
		depth.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		depth.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		depth.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
		depth.BackFace = depth.FrontFace;
	}
	const uint64 depthKey = HashBytes(&depth, sizeof(depth));
	auto depthIt = m_depthStencilCache.find(depthKey);
	if (depthIt == m_depthStencilCache.end())
	{
		ComPtr<ID3D11DepthStencilState> state;
		ThrowIfFailed(m_device->CreateDepthStencilState(&depth, &state), "Create GX2 depth/stencil state");
		depthIt = m_depthStencilCache.emplace(depthKey, std::move(state)).first;
	}
	const UINT stencilRef = depth.StencilEnable ?
		registers.DB_STENCILREFMASK.get_STENCILREF_F() : 0;
	if (m_appliedDepthStencilState.Get() != depthIt->second.Get() ||
		!m_appliedDepthStencilStateValid || m_appliedStencilRef != stencilRef)
	{
		m_context->OMSetDepthStencilState(depthIt->second.Get(), stencilRef);
		m_appliedDepthStencilState = depthIt->second;
		m_appliedStencilRef = stencilRef;
		m_appliedDepthStencilStateValid = true;
	}
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
	const bool rebuildGraphicsState = drawcallContext.isFirst || m_graphicsStateInvalid;
	if (rebuildGraphicsState)
	{
		m_graphicsStateInvalid = false;
		LatteSHRC_UpdateActiveShaders();
		if (LatteGPUState.activeShaderHasError)
			return;
		if (!EnsureActiveShadersCompiled())
		{
			LatteGPUState.activeShaderHasError = true;
			cemuLog_log(LogType::Force,
				"D3D11 draw skipped because an active shader could not be prepared");
			return;
		}
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
#if defined(CEMU_UWP)
		// Active shaders only change when the GX2 graphics state is rebuilt. Keep
		// their hashes for device-loss diagnostics without walking all three shader
		// contexts on every draw in the sequence.
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
#if defined(CEMU_UWP)
	// The pixel-UAV stream-output replay needs the decoded host indices after the
	// normal upload. Force a fresh cached allocation here and retain its tiny CPU
	// staging copy just until the replay has converted it into a GPU record map.
	m_keepIndexStagingForPixelStreamout = false;
	if (indices && !UseTFViaSSBO() &&
		LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0 &&
		!LatteSHRC_GetActiveGeometryShader())
	{
		auto* vertexContext = LatteSHRC_GetActiveVertexShader();
		auto* vertexShader = vertexContext ? static_cast<D3D11Shader*>(vertexContext->shader) : nullptr;
		if (vertexShader && vertexShader->HasPixelStreamoutCapture())
		{
			m_keepIndexStagingForPixelStreamout = true;
			LatteIndices_invalidateAll();
		}
	}
#else
	m_keepIndexStagingForPixelStreamout = false;
#endif
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
	if (!rebuildGraphicsState && m_activeFbo && m_activeFeedbackLoop)
	{
		auto* nativeFbo = static_cast<D3D11CachedFBO*>(m_activeFbo);
		m_activeFeedbackLoop = ResolveTextureFeedbackLoops(nativeFbo->targets, nativeFbo->depth);
		if (m_activeFeedbackLoop)
			m_context->OMSetRenderTargets(nativeFbo->targetCount,
				nativeFbo->targets.data(), nativeFbo->depth);
	}
	LatteStreamout_PrepareDrawcall(count, instanceCount);
	UpdateUniformVars(LatteSHRC_GetActiveVertexShader(), count, rebuildGraphicsState,
		drawcallContext.aluConstVSDirty, drawcallContext.vsUniformBufferDirtyMask);
	UpdateUniformVars(LatteSHRC_GetActivePixelShader(), count, rebuildGraphicsState,
		drawcallContext.aluConstPSDirty, drawcallContext.psUniformBufferDirtyMask);
	UpdateUniformVars(LatteSHRC_GetActiveGeometryShader(), count, rebuildGraphicsState,
		false, drawcallContext.gsUniformBufferDirtyMask);
	if (rebuildGraphicsState)
	{
		UpdateSamplerSwizzleBuffer(LatteSHRC_GetActiveVertexShader());
		UpdateSamplerSwizzleBuffer(LatteSHRC_GetActivePixelShader());
		UpdateSamplerSwizzleBuffer(LatteSHRC_GetActiveGeometryShader());
		ApplyPipelineState();
	}
	const auto topology = PrimitiveTopology(primitive);
	if (m_appliedPrimitiveTopology != topology)
	{
		m_context->IASetPrimitiveTopology(topology);
		m_appliedPrimitiveTopology = topology;
	}
	// D3D11On12 materializes native D3D12 pipelines from the complete logical
	// combination. Keep the same combined identity used by explicit backends so
	// diagnostics and failure admission operate on canonical states rather than
	// on a stream of redundant component binds. The component objects themselves
	// remain cached by their normalized descriptors in ApplyPipelineState().
	if (rebuildGraphicsState && m_seenLogicalPipelines.size() < 8192)
	{
		const bool inserted = m_seenLogicalPipelines.insert(CurrentLogicalPipelineKey()).second;
		if (inserted)
			++g_compiling_pipelines;
	}
	bool drawIssued = false;

	const bool rasterizerKilled =
		LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() &&
		LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	const bool bothFacesCulled =
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_FRONT() &&
		LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL.get_CULL_BACK();
	const bool skipRasterDraw =
		(rasterizerKilled || bothFacesCulled) && !m_streamoutActive;
	const bool suppressStorageRaster =
		(rasterizerKilled || bothFacesCulled) && m_streamoutActive && m_streamoutUsesStorage;
	const bool skipPixelCaptureRaster =
		(rasterizerKilled || bothFacesCulled) && m_streamoutActive && m_streamoutUsesPixelCapture;
	if (suppressStorageRaster)
	{
		// The UAV shader must still execute to produce transform feedback, but GX2
		// requested no raster output. Remove RTV/DSV bindings for this draw while
		// retaining the stream-out UAV; the active cached FBO is restored below.
		ID3D11UnorderedAccessView* uav = m_streamoutStorageUav.Get();
		m_context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
			StreamoutUavSlot, 1, &uav, nullptr);
	}
	else if (skipPixelCaptureRaster)
	{
		// Pixel-UAV capture is replayed below with a private target. Unlike the
		// shader-storage path, the title draw itself has no feedback work left to
		// execute, so honoring GX2 raster discard avoids a redundant full draw.
		m_context->OMSetRenderTargets(0, nullptr, nullptr);
	}

	if (skipRasterDraw || skipPixelCaptureRaster)
	{
		// Vulkan represents both states directly. D3D11 has no rasterizer-discard
		// switch and no FRONT_AND_BACK cull mode, so a draw without stream output
		// is a true no-op. Pixel stream-output is handled by its dedicated replay.
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

	// Native D3D11 stream output uses a geometry shader created with
	// D3D11_SO_NO_RASTERIZED_STREAM and therefore needs a raster replay. The
	// The Feature Level 11.0 UWP pixel-UAV path writes feedback in a dedicated
	// capture pass; native D3D11 stream output still needs this raster replay.
	const bool needsRasterReplay = drawIssued && m_streamoutActive &&
		!m_streamoutUsesStorage && !m_streamoutUsesPixelCapture;
	// Present reports removal every frame. On Xbox, sample at the first draw of
	// each frame as well, retaining early diagnostics without calling through
	// D3D11On12 after every individual draw. Keep desktop behavior unchanged.
#if defined(CEMU_UWP)
	const bool sampleDeviceHealth = drawIssued && m_deviceHealthFrame != m_memoryCheckFrame;
	if (sampleDeviceHealth)
		m_deviceHealthFrame = m_memoryCheckFrame;
#else
	const bool sampleDeviceHealth = drawIssued;
#endif
	if (sampleDeviceHealth && !CheckDeviceHealth("GX2 draw"))
	{
		// Do not unbind SO or restore shaders after removal; either operation would
		// enter D3D11On12 again and amplify the driver's exception cascade.
		m_streamoutActive = false;
		m_streamoutEnabled.fill(false);
		m_graphicsStateInvalid = true;
		LatteGPUState.drawCallCounter++;
		return;
	}
	bool pixelCaptureSucceeded{};
	if (m_streamoutActive && m_streamoutUsesPixelCapture)
	{
		auto* indexAllocation = static_cast<IndexBufferAllocation*>(allocation.rendererInternal);
		ID3D11Buffer* indexBuffer = indexAllocation ? indexAllocation->buffer.Get() : nullptr;
		const UINT indexOffset = indexAllocation ? indexAllocation->offset : 0;
		const uint8* decodedIndices = indexAllocation && !indexAllocation->data.empty() ?
			indexAllocation->data.data() : nullptr;
		const UINT decodedIndexBytes = indexAllocation ?
			static_cast<UINT>(indexAllocation->data.size()) : 0;
		pixelCaptureSucceeded = ExecutePixelStreamoutCapture(baseVertex, baseInstance, instanceCount,
			count, hostIndexCount, hostIndexType, indexBuffer, indexOffset,
			decodedIndices, decodedIndexBytes);
		if (m_keepIndexStagingForPixelStreamout && indexAllocation)
		{
			// The record map has already been uploaded. Do not retain a duplicate CPU
			// copy in the index LRU; the next pixel-capture draw invalidates it before
			// decode, guaranteeing that its indices are current as well.
			std::vector<uint8>().swap(indexAllocation->data);
		}
		m_keepIndexStagingForPixelStreamout = false;
		if (!pixelCaptureSucceeded)
		{
			m_streamoutDataAvailable = false;
			m_streamoutEnabled.fill(false);
			m_streamoutRangeSizes.fill(0);
			cemuLog_logOnce(LogType::Force,
				"D3D11 Xbox pixel stream-output replay failed; destination ranges were not reused");
		}
	}
	if (!drawIssued && !pixelCaptureSucceeded && m_streamoutActive)
	{
		m_streamoutEnabled.fill(false);
		m_streamoutRangeSizes.fill(0);
		m_streamoutDataAvailable = false;
	}
	// This also unbinds SO and restores the title/rectangle geometry shader.
	LatteStreamout_FinishDrawcall(false);
	if ((suppressStorageRaster || skipPixelCaptureRaster) && m_activeFbo)
	{
		auto* nativeFbo = static_cast<D3D11CachedFBO*>(m_activeFbo);
		m_context->OMSetRenderTargets(nativeFbo->targetCount,
			nativeFbo->targets.data(), nativeFbo->depth);
	}
	// Stream-output shaders use D3D11_SO_NO_RASTERIZED_STREAM, so a GX2 operation
	// requesting feedback and rasterization needs an ordinary second draw.
	bool rasterDrawIssued = drawIssued && !needsRasterReplay;
	if (needsRasterReplay && !rasterizerKilled && !bothFacesCulled)
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

	if (rasterDrawIssued && LatteSHRC_GetActivePixelShader())
		LatteRenderTarget_trackUpdates();
	LatteGPUState.drawCallCounter++;
	LatteTextureReadback_Update();
}

void D3D11Renderer::draw_endSequence() {}
