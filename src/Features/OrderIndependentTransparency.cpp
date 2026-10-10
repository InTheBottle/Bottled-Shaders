#include "OrderIndependentTransparency.h"

#include "Features/Effects11.h"
#include "Features/ReverseZ.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format, uint bindFlags);

#define I18N_KEY_PREFIX "feature.oit."

NLOHMANN_JSON_SERIALIZE_ENUM(OrderIndependentTransparency::OITMethod,
	{
		{ OrderIndependentTransparency::OITMethod::Disabled, "Disabled" },
		{ OrderIndependentTransparency::OITMethod::Visualize, "Visualize" },
		{ OrderIndependentTransparency::OITMethod::Balanced, "AT" },
		{ OrderIndependentTransparency::OITMethod::Fast, "Blended" },
		{ OrderIndependentTransparency::OITMethod::Stable, "RVO" },
	})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OrderIndependentTransparency::Settings,
	Method,
	BufferSize,
	MaxLayers,
	AlphaThreshold,
	DistanceThreshold,
	CaptureMultiplicativeLayer,
	OverrideRenderTargets,
	WriteDepth,
	WriteDepthThreshold,
	WBOITAdditiveAlphaScale,
	WBOITMinProjectedDistance,
	WBOITWeightMin,
	WBOITWeightMax,
	SSRAlphaScale)

namespace
{
	using OITMethod = OrderIndependentTransparency::OITMethod;

	constexpr uint32_t kAdditiveDescriptor = static_cast<uint32_t>(State::ExtraFeatureDescriptors::OITAdditive);
	constexpr uint32_t kMultiplicativeDescriptor = static_cast<uint32_t>(State::ExtraFeatureDescriptors::OITMultiplicative);
	constexpr uint32_t kDepthWriteDescriptor = static_cast<uint32_t>(State::ExtraFeatureDescriptors::OITDepthWrite);
	constexpr uint32_t kDisabledDescriptor = static_cast<uint32_t>(State::ExtraFeatureDescriptors::OITDisabled);
	constexpr uint32_t kDescriptorMask = kAdditiveDescriptor | kMultiplicativeDescriptor | kDepthWriteDescriptor | kDisabledDescriptor;

	enum AlphaBlendMode : uint32_t
	{
		kBlendOpaque = 0,
		kBlendAlpha = 1,
		kBlendAdditive = 2,
		kBlendMultiplicativeAlpha = 3,
		kBlendMultiplicative = 4,
	};

	constexpr UINT kKeepCounter = static_cast<UINT>(-1);

	uint32_t GetCaptureMask(RE::BSShader::Type a_type)
	{
		switch (a_type) {
		case RE::BSShader::Type::Lighting:
			return static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::OIT);
		case RE::BSShader::Type::Effect:
			return static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::OIT);
		case RE::BSShader::Type::Particle:
			return static_cast<uint32_t>(SIE::ShaderCache::ParticleShaderFlags::OIT);
		default:
			return 0;
		}
	}

	float ElapsedMs(std::chrono::steady_clock::time_point a_begin, std::chrono::steady_clock::time_point a_end)
	{
		return std::chrono::duration<float, std::milli>(a_end - a_begin).count();
	}

	struct Main_RenderWorld_RenderTransparency
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			auto& oit = globals::features::orderIndependentTransparency;
			const auto passBegin = std::chrono::steady_clock::now();
			const bool capture = oit.BeginAlphaGroup();
			func(a_accumulator, a_renderFlags);
			const auto passEnd = std::chrono::steady_clock::now();
			if (capture) {
				oit.EndAlphaGroup();
				oit.passTime = ElapsedMs(passBegin, passEnd);
				oit.compositeTime = ElapsedMs(passEnd, std::chrono::steady_clock::now());
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Renderer_Flush
	{
		static void thunk(RE::BSGraphics::Renderer* a_renderer, uint8_t a_flags)
		{
			globals::features::orderIndependentTransparency.PreSetStateDirty();
			func(a_renderer, a_flags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <RE::BSShader::Type ShaderType>
	struct BSShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::orderIndependentTransparency.SetupGeometry(a_pass);
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <RE::BSShader::Type ShaderType>
	struct BSShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::orderIndependentTransparency.RestoreGeometry();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <RE::BSShader::Type ShaderType>
	void HookGeometry(REL::VariantID a_vtable)
	{
		stl::write_vfunc<0x6, BSShader_SetupGeometry<ShaderType>>(a_vtable);
		stl::write_vfunc<0x7, BSShader_RestoreGeometry<ShaderType>>(a_vtable);
	}

	D3D11_RENDER_TARGET_BLEND_DESC DisabledTarget()
	{
		D3D11_RENDER_TARGET_BLEND_DESC target{};
		target.BlendEnable = FALSE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ZERO;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_ZERO;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = 0;
		return target;
	}

	D3D11_RENDER_TARGET_BLEND_DESC NearestDepthTarget(bool a_reverseZ)
	{
		D3D11_RENDER_TARGET_BLEND_DESC target = DisabledTarget();
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ONE;
		target.BlendOp = a_reverseZ ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_MIN;
		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = a_reverseZ ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_MIN;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		return target;
	}

	D3D11_RENDER_TARGET_BLEND_DESC AccumulateTarget()
	{
		D3D11_RENDER_TARGET_BLEND_DESC target = DisabledTarget();
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ONE;
		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		return target;
	}

	D3D11_RENDER_TARGET_BLEND_DESC RevealageTarget()
	{
		D3D11_RENDER_TARGET_BLEND_DESC target = DisabledTarget();
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ZERO;
		target.DestBlend = D3D11_BLEND_SRC_COLOR;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
		return target;
	}

	bool CreateStructuredBuffer(std::optional<Buffer>& a_buffer, const char* a_name, uint64_t a_elements, uint32_t a_stride, bool a_counter)
	{
		const uint64_t bytes = a_elements * a_stride;
		if (a_elements == 0 || bytes > static_cast<uint64_t>(D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM) * 1024 * 1024) {
			logger::error("[OIT] {} would need {} bytes, which exceeds the D3D11 resource limit", a_name, bytes);
			return false;
		}

		try {
			CD3D11_BUFFER_DESC desc(static_cast<UINT>(bytes), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, a_stride);
			a_buffer.emplace(desc, nullptr, a_name);
			a_buffer->CreateSRV(CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN, 0, static_cast<UINT>(a_elements)));

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = static_cast<UINT>(a_elements);
			uavDesc.Buffer.Flags = a_counter ? D3D11_BUFFER_UAV_FLAG_COUNTER : 0;
			a_buffer->CreateUAV(uavDesc);
		} catch (const DX::com_exception& e) {
			logger::error("[OIT] Failed to create {}: {}", a_name, e.what());
			a_buffer.reset();
			return false;
		}
		return true;
	}

	bool CreateScreenTexture(std::optional<Texture2D>& a_texture, const char* a_name, const D3D11_TEXTURE2D_DESC& a_baseDesc, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		auto desc = a_baseDesc;
		desc.Format = a_format;
		desc.BindFlags = a_bindFlags;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.MipLevels = 1;
		desc.ArraySize = 1;

		try {
			a_texture.emplace(desc, a_name);
			if (a_bindFlags & D3D11_BIND_SHADER_RESOURCE)
				a_texture->CreateSRV(CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, a_format));
			if (a_bindFlags & D3D11_BIND_RENDER_TARGET)
				a_texture->CreateRTV(CD3D11_RENDER_TARGET_VIEW_DESC(D3D11_RTV_DIMENSION_TEXTURE2D, a_format));
			if (a_bindFlags & D3D11_BIND_UNORDERED_ACCESS)
				a_texture->CreateUAV(CD3D11_UNORDERED_ACCESS_VIEW_DESC(D3D11_UAV_DIMENSION_TEXTURE2D, a_format));
		} catch (const DX::com_exception& e) {
			logger::error("[OIT] Failed to create {}: {}", a_name, e.what());
			a_texture.reset();
			return false;
		}
		return true;
	}
}

bool OrderIndependentTransparency::HasShaderDefine(RE::BSShader::Type a_type)
{
	return a_type == RE::BSShader::Type::Water || a_type == RE::BSShader::Type::ImageSpace;
}

std::pair<std::string, std::vector<std::string>> OrderIndependentTransparency::GetFeatureSummary()
{
	return {
		T(TKEY("description"), "Order Independent Transparency (OIT) correctly blends overlapping transparent surfaces such as hair, glass, foliage, smoke and particles, independent of the order the game draws them in."),
		{ T(TKEY("key_feature_1"), "Stable layering of overlapping transparent surfaces"),
			T(TKEY("key_feature_2"), "Transparent surfaces stay visible through water refraction and reflections"),
			T(TKEY("key_feature_3"), "Uses Reverse Z-Buffer float depth for distant layer ordering") }
	};
}

OrderIndependentTransparency::FeatureCB OrderIndependentTransparency::GetCommonBufferData() const
{
	FeatureCB data;
	if (!loaded)
		return data;

	data.Enabled = settings.Method != OITMethod::Disabled && compositeValid;
	data.AlphaThreshold = settings.AlphaThreshold;
	data.WriteDepthThreshold = settings.WriteDepthThreshold;
	data.SSRAlphaScale = settings.SSRAlphaScale;
	data.WBOITAdditiveAlphaScale = settings.WBOITAdditiveAlphaScale;
	data.WBOITMinProjectedDistance = settings.WBOITMinProjectedDistance;
	data.WBOITWeightMin = settings.WBOITWeightMin;
	data.WBOITWeightMax = settings.WBOITWeightMax;
	return data;
}

void OrderIndependentTransparency::PostPostLoad()
{
	HookGeometry<RE::BSShader::Type::Lighting>(RE::VTABLE_BSLightingShader[0]);
	HookGeometry<RE::BSShader::Type::Effect>(RE::VTABLE_BSEffectShader[0]);
	HookGeometry<RE::BSShader::Type::Particle>(RE::VTABLE_BSParticleShader[0]);

	stl::detour_thunk<Main_RenderWorld_RenderTransparency>(REL::RelocationID(99940, 106585));

	if (REL::Module::IsAE())
		stl::detour_thunk<Renderer_Flush>(REL::RelocationID(77247, 77247));

	logger::info("[OIT] Installed hooks");
}

void OrderIndependentTransparency::NormalizeSettings()
{
	if (static_cast<uint32_t>(settings.Method) > static_cast<uint32_t>(OITMethod::Stable))
		settings.Method = OITMethod::Disabled;
	if (settings.Method == OITMethod::Stable && rovChecked && !rovSupported)
		settings.Method = OITMethod::Balanced;
	settings.BufferSize = std::clamp<uint>(settings.BufferSize, 2, 16);
	settings.MaxLayers = std::clamp<uint>(((settings.MaxLayers + 2) / 4) * 4, 4, 32);
	settings.AlphaThreshold = std::clamp(settings.AlphaThreshold, 0.0f, 0.1f);
	settings.DistanceThreshold = std::clamp(settings.DistanceThreshold, 0.0f, kInfiniteDistance);
	settings.WriteDepthThreshold = std::clamp(settings.WriteDepthThreshold, 0.0f, 1.0f);
	settings.WBOITAdditiveAlphaScale = std::clamp(settings.WBOITAdditiveAlphaScale, 0.0f, 1.0f);
	settings.WBOITMinProjectedDistance = std::clamp(settings.WBOITMinProjectedDistance, 0.0f, 1.0f);
	settings.WBOITWeightMax = std::clamp(settings.WBOITWeightMax, 0.0f, 10.0f);
	settings.WBOITWeightMin = std::clamp(settings.WBOITWeightMin, 0.0f, settings.WBOITWeightMax);
	settings.SSRAlphaScale = std::clamp(settings.SSRAlphaScale, 0.0f, 10.0f);
}

void OrderIndependentTransparency::LoadSettings(json& o_json)
{
	const auto previousMethod = settings.Method;
	settings = o_json;
	NormalizeSettings();
	if (settings.Method != previousMethod)
		OnMethodChanged();
	SyncMaterialNodeCount();
}

void OrderIndependentTransparency::SaveSettings(json& o_json)
{
	o_json = settings;
}

void OrderIndependentTransparency::RestoreDefaultSettings()
{
	const auto previousMethod = settings.Method;
	settings = {};
	NormalizeSettings();
	if (settings.Method != previousMethod)
		OnMethodChanged();
	SyncMaterialNodeCount();
}

uint32_t OrderIndependentTransparency::GetMaterialVariant() const
{
	return capturing ? GetMethodVariant() : 0;
}

uint32_t OrderIndependentTransparency::GetMethodVariant() const
{
	switch (settings.Method) {
	case OITMethod::Visualize:
	case OITMethod::Balanced:
		return 1;
	case OITMethod::Stable:
		return 2;
	case OITMethod::Fast:
		return 3;
	default:
		return 0;
	}
}

const char* OrderIndependentTransparency::GetNodeCountDefine() const
{
	static constexpr const char* kNodeCounts[] = { "4", "8", "12", "16", "20", "24", "28", "32" };
	return kNodeCounts[GetNodeCount() / 4 - 1];
}

uint OrderIndependentTransparency::GetNodeCount() const
{
	return std::clamp<uint>(((settings.MaxLayers + 2) / 4) * 4, 4, 32);
}

bool OrderIndependentTransparency::UsesFragmentList() const
{
	return settings.Method == OITMethod::Balanced || settings.Method == OITMethod::Visualize;
}

float OrderIndependentTransparency::GetFarDepth() const
{
	return globals::features::reverseZ.IsActive() ? 0.0f : 1.0f;
}

ID3D11ShaderResourceView* OrderIndependentTransparency::GetMainDepthSRV() const
{
	return globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
}

void OrderIndependentTransparency::OnMethodChanged()
{
	blendVariants.clear();
	boundVariant = nullptr;
	boundEngineState = nullptr;
	resourcesFailed = false;
	compositeValid = false;
}

void OrderIndependentTransparency::SyncMaterialNodeCount()
{
	if (settings.Method != OITMethod::Stable)
		return;

	const auto nodeCount = GetNodeCount();
	if (materialNodeCount != 0 && materialNodeCount != nodeCount) {
		auto shaderCache = globals::shaderCache;
		shaderCache->Clear(RE::BSShader::Type::Lighting);
		shaderCache->Clear(RE::BSShader::Type::Effect);
		shaderCache->Clear(RE::BSShader::Type::Particle);
	}
	materialNodeCount = nodeCount;
}

void OrderIndependentTransparency::SetupResources()
{
	auto* renderer = globals::game::renderer;
	auto* device = globals::d3d::device;

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !main.SRV || !main.RTV) {
		DisableForFailure("main render target is unavailable");
		return;
	}

	D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
	rovSupported = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2))) && options2.ROVsSupported;
	rovChecked = true;
	if (!rovSupported) {
		logger::info("[OIT] Rasterizer ordered views are not supported; the Stable method is unavailable");
		const auto previousMethod = settings.Method;
		NormalizeSettings();
		if (settings.Method != previousMethod)
			OnMethodChanged();
	}

	try {
		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.RTV->GetDesc(&rtvDesc);
		SetupRenderTarget(RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA, texDesc, srvDesc, rtvDesc, uavDesc, texDesc.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		auto& alphaOnly = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
		Util::SetResourceName(alphaOnly.texture, "OrderIndependentTransparency::AlphaOnly");
		alphaOnlyReady = alphaOnly.SRV && alphaOnly.RTV;
	} catch (const DX::com_exception& e) {
		logger::error("[OIT] Failed to recreate the alpha-only target: {}", e.what());
		alphaOnlyReady = false;
	}

	if (!alphaOnlyReady) {
		DisableForFailure("the alpha-only target could not be created");
		return;
	}

	try {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = FALSE;
		blendDesc.RenderTarget[0] = DisabledTarget();
		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, compositeBlendState.put()));
		Util::SetResourceName(compositeBlendState.get(), "OrderIndependentTransparency::CompositeBlend");

		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, compositeDepthState.put()));
		Util::SetResourceName(compositeDepthState.get(), "OrderIndependentTransparency::CompositeDepth");

		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthPassState.put()));
		Util::SetResourceName(depthPassState.get(), "OrderIndependentTransparency::DepthWrite");

		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = FALSE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, compositeRasterState.put()));
		Util::SetResourceName(compositeRasterState.get(), "OrderIndependentTransparency::CompositeRaster");
	} catch (const DX::com_exception& e) {
		DisableForFailure(e.what());
		return;
	}

	if (auto* rawDepth = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITDepth.ps.hlsl", {}, "ps_5_0")))
		depthPS.attach(rawDepth);
	else
		logger::error("[OIT] Failed to compile the depth write shader; transparent depth will not be written");

	EnsureResources();
}

void OrderIndependentTransparency::ClearShaderCache()
{
	resolvePS = nullptr;
	depthPS = nullptr;
	compiledMethod = OITMethod::Disabled;
	compiledNodeCount = 0;
	if (auto* rawDepth = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITDepth.ps.hlsl", {}, "ps_5_0")))
		depthPS.attach(rawDepth);
}

void OrderIndependentTransparency::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	const auto type = a_shader->shaderType.get();
	if (type != RE::BSShader::Type::Effect && type != RE::BSShader::Type::Particle)
		return;

	const uint32_t variant = GetMethodVariant();
	if (variant == 0)
		return;

	const uint32_t mask = GetCaptureMask(type);
	const uint32_t variantBits = variant << std::countr_zero(mask);
	constexpr auto motionVectorsNormals = static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::MotionVectorsNormals);

	auto state = globals::state;
	auto shaderCache = globals::shaderCache;
	for (const auto& entry : a_shader->pixelShaders) {
		auto vertexDescriptor = entry->id;
		auto pixelDescriptor = entry->id;
		if (type == RE::BSShader::Type::Effect && (pixelDescriptor & motionVectorsNormals))
			continue;
		state->ModifyShaderLookup(*a_shader, vertexDescriptor, pixelDescriptor);
		std::ignore = shaderCache->GetPixelShader(*a_shader, (pixelDescriptor & ~mask) | variantBits);
	}
}

void OrderIndependentTransparency::CompileShaders()
{
	const auto nodeCount = GetNodeCount();
	if (compiledMethod == settings.Method && compiledNodeCount == nodeCount)
		return;

	resolvePS = nullptr;
	compiledMethod = settings.Method;
	compiledNodeCount = nodeCount;

	const std::string nodes = std::to_string(nodeCount);
	std::vector<std::pair<const char*, const char*>> defines;
	switch (settings.Method) {
	case OITMethod::Visualize:
		defines = { { "OIT_DEBUG", "1" } };
		break;
	case OITMethod::Balanced:
		defines = { { "OIT_AT", "1" }, { "OIT_NODE_COUNT", nodes.c_str() } };
		break;
	case OITMethod::Fast:
		defines = { { "OIT_BLENDED", "1" } };
		break;
	case OITMethod::Stable:
		defines = { { "OIT_ROV", "1" }, { "OIT_NODE_COUNT", nodes.c_str() } };
		break;
	default:
		return;
	}

	if (auto* raw = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\OIT\\OITResolve.ps.hlsl", defines, "ps_5_0")))
		resolvePS.attach(raw);
	else
		logger::error("[OIT] Failed to compile the resolve shader for method {}", static_cast<uint32_t>(settings.Method));
}

void OrderIndependentTransparency::ReleaseMethodResources()
{
	headerTexture.reset();
	nodesBuffer.reset();
	rovColorBuffer.reset();
	rovDepthBuffer.reset();
	accumFrontTexture.reset();
	accumAllTexture.reset();
	revealageTexture.reset();
	resourceMethod = OITMethod::Disabled;
	resourceBufferSize = 0;
	resourceNodeCount = 0;
}

bool OrderIndependentTransparency::EnsureResources()
{
	if (settings.Method == OITMethod::Disabled || resourcesFailed || !alphaOnlyReady)
		return false;
	if (settings.Method == OITMethod::Stable && !rovSupported)
		return false;

	const auto nodeCount = GetNodeCount();
	const bool upToDate = resourceMethod == settings.Method &&
	                      (!UsesFragmentList() || resourceBufferSize == settings.BufferSize) &&
	                      (settings.Method != OITMethod::Stable || resourceNodeCount == nodeCount);

	if (!upToDate) {
		ReleaseMethodResources();

		D3D11_TEXTURE2D_DESC mainDesc{};
		globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&mainDesc);
		const uint64_t pixels = static_cast<uint64_t>(mainDesc.Width) * mainDesc.Height;
		const uint64_t tiledPixels = static_cast<uint64_t>((mainDesc.Width + 1) / 2) * ((mainDesc.Height + 1) / 2) * 4;

		bool ok = true;
		if (!writeDepthTexture)
			ok = CreateScreenTexture(writeDepthTexture, "OrderIndependentTransparency::WriteDepth", mainDesc, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

		switch (settings.Method) {
		case OITMethod::Fast:
			ok = ok && CreateScreenTexture(accumFrontTexture, "OrderIndependentTransparency::AccumFront", mainDesc, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
			ok = ok && CreateScreenTexture(accumAllTexture, "OrderIndependentTransparency::AccumAll", mainDesc, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
			ok = ok && CreateScreenTexture(revealageTexture, "OrderIndependentTransparency::Revealage", mainDesc, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
			break;
		case OITMethod::Visualize:
		case OITMethod::Balanced:
			ok = ok && CreateScreenTexture(headerTexture, "OrderIndependentTransparency::ListHead", mainDesc, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
			ok = ok && CreateStructuredBuffer(nodesBuffer, "OrderIndependentTransparency::ListNodes", pixels * settings.BufferSize, sizeof(FragmentListNode), true);
			break;
		case OITMethod::Stable:
			ok = ok && CreateScreenTexture(headerTexture, "OrderIndependentTransparency::ClearMask", mainDesc, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
			ok = ok && CreateStructuredBuffer(rovColorBuffer, "OrderIndependentTransparency::NodeColor", tiledPixels, sizeof(uint32_t) * 2 * nodeCount, false);
			ok = ok && CreateStructuredBuffer(rovDepthBuffer, "OrderIndependentTransparency::NodeDepth", tiledPixels, sizeof(float) * nodeCount, false);
			break;
		default:
			break;
		}

		if (!ok) {
			ReleaseMethodResources();
			resourcesFailed = true;
			logger::error("[OIT] Resource allocation failed; transparency renders without OIT until the method or buffer size changes");
			return false;
		}

		resourceMethod = settings.Method;
		resourceBufferSize = settings.BufferSize;
		resourceNodeCount = nodeCount;
		blendVariants.clear();
	}

	CompileShaders();
	return resolvePS != nullptr;
}

void OrderIndependentTransparency::DisableForFailure(std::string_view a_reason)
{
	ReleaseMethodResources();
	writeDepthTexture.reset();
	resolvePS = nullptr;
	depthPS = nullptr;
	blendVariants.clear();
	capturing = false;
	resourcesFailed = true;
	logger::error("[OIT] Disabled: {}", a_reason);
}

void OrderIndependentTransparency::SetOwnBlendState(ID3D11BlendState* a_state)
{
	settingOwnBlendState = true;
	globals::d3d::context->OMSetBlendState(a_state, nullptr, 0xFFFFFFFF);
	settingOwnBlendState = false;
}

ID3D11BlendState* OrderIndependentTransparency::GetBlendVariant(ID3D11BlendState* a_engineState, bool a_capture, bool a_depth)
{
	const BlendVariantKey key{ a_engineState, a_capture, a_depth };
	if (auto it = blendVariants.find(key); it != blendVariants.end())
		return it->second.variant.get();

	D3D11_BLEND_DESC engineDesc = CD3D11_BLEND_DESC(D3D11_DEFAULT);
	if (a_engineState)
		a_engineState->GetDesc(&engineDesc);

	D3D11_BLEND_DESC desc{};
	desc.AlphaToCoverageEnable = engineDesc.AlphaToCoverageEnable;
	desc.IndependentBlendEnable = TRUE;
	for (uint32_t i = 0; i < 8; ++i)
		desc.RenderTarget[i] = DisabledTarget();
	for (uint32_t i = 0; i < 3; ++i)
		desc.RenderTarget[i] = engineDesc.IndependentBlendEnable ? engineDesc.RenderTarget[i] : engineDesc.RenderTarget[0];

	if (a_depth)
		desc.RenderTarget[3] = NearestDepthTarget(globals::features::reverseZ.IsActive());

	if (a_capture && settings.Method == OITMethod::Fast) {
		desc.RenderTarget[0].RenderTargetWriteMask = 0;
		desc.RenderTarget[2].RenderTargetWriteMask = 0;
		desc.RenderTarget[4] = AccumulateTarget();
		desc.RenderTarget[5] = AccumulateTarget();
		desc.RenderTarget[6] = RevealageTarget();
	}

	BlendVariant entry;
	if (FAILED(globals::d3d::device->CreateBlendState(&desc, entry.variant.put()))) {
		logger::error("[OIT] Failed to create a capture blend state");
		return a_engineState;
	}
	Util::SetResourceName(entry.variant.get(), "OrderIndependentTransparency::CaptureBlend");
	entry.engineState.copy_from(a_engineState);

	auto* variant = entry.variant.get();
	blendVariants.emplace(key, std::move(entry));
	return variant;
}

void OrderIndependentTransparency::ApplyBlendVariant(bool a_capture, bool a_depth)
{
	auto context = globals::d3d::context;

	winrt::com_ptr<ID3D11BlendState> current;
	FLOAT blendFactor[4]{};
	UINT sampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(current.put(), blendFactor, &sampleMask);

	if (current.get() == boundVariant && a_capture == boundCapture && a_depth == boundDepth)
		return;

	auto* engineState = current.get() == boundVariant ? boundEngineState : current.get();
	auto* variant = GetBlendVariant(engineState, a_capture, a_depth);

	settingOwnBlendState = true;
	context->OMSetBlendState(variant, blendFactor, sampleMask);
	settingOwnBlendState = false;

	boundVariant = variant;
	boundEngineState = engineState;
	boundCapture = a_capture;
	boundDepth = a_depth;
}

void OrderIndependentTransparency::BindCaptureTargets()
{
	auto context = globals::d3d::context;
	if (captureUAVCount == 0) {
		context->OMSetRenderTargets(captureRTVCount, captureRTVs.data(), captureDSV);
		auto* waterDepth = GetMainDepthSRV();
		context->PSSetShaderResources(67, 1, &waterDepth);
	} else {
		const std::array<UINT, 3> counters{ kKeepCounter, kKeepCounter, kKeepCounter };
		context->OMSetRenderTargetsAndUnorderedAccessViews(captureRTVCount, captureRTVs.data(), captureDSV, captureRTVCount, captureUAVCount, captureUAVs.data(), counters.data());
	}
}

bool OrderIndependentTransparency::BeginAlphaGroup()
{
	if (!loaded || settings.Method == OITMethod::Disabled)
		return false;

	auto state = globals::state;
	if (!globals::shaderCache->IsEnabled() || !state->inWorld)
		return false;
	if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
		return false;

	compositeValid = false;
	if (!globals::features::upscaling.GetUpscaleVS() || !compositeRasterState || !EnsureResources() || !writeDepthTexture)
		return false;

	auto* renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& taaMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
	auto& alphaOnly = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
	auto& prepassDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (!main.RTV || !alphaOnly.RTV || !prepassDepth.readOnlyViews[0])
		return false;

	{
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(66, 1, &nullSRV);
	}

	const float farDepth[4] = { GetFarDepth(), 0.0f, 0.0f, 0.0f };
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (settings.WriteDepth)
		context->ClearRenderTargetView(writeDepthTexture->rtv.get(), farDepth);
	context->ClearRenderTargetView(alphaOnly.RTV, zero);

	captureDSV = prepassDepth.readOnlyViews[0];
	captureRTVs = { main.RTV, taaMask.RTV, alphaOnly.RTV, writeDepthTexture->rtv.get(), nullptr, nullptr, nullptr };
	captureUAVs = { nullptr, nullptr, nullptr };
	captureRTVCount = 4;
	captureUAVCount = 0;

	switch (settings.Method) {
	case OITMethod::Fast:
		{
			const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			context->ClearRenderTargetView(accumFrontTexture->rtv.get(), zero);
			context->ClearRenderTargetView(accumAllTexture->rtv.get(), zero);
			context->ClearRenderTargetView(revealageTexture->rtv.get(), one);
			captureRTVs[4] = accumFrontTexture->rtv.get();
			captureRTVs[5] = accumAllTexture->rtv.get();
			captureRTVs[6] = revealageTexture->rtv.get();
			captureRTVCount = 7;
			auto* waterDepth = GetMainDepthSRV();
			context->PSSetShaderResources(67, 1, &waterDepth);
			break;
		}
	case OITMethod::Visualize:
	case OITMethod::Balanced:
		{
			const UINT clear[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(headerTexture->uav.get(), clear);
			captureUAVs = { headerTexture->uav.get(), nodesBuffer->uav.get(), nullptr };
			captureUAVCount = 2;
			break;
		}
	case OITMethod::Stable:
		{
			const UINT clear[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(headerTexture->uav.get(), clear);
			captureUAVs = { headerTexture->uav.get(), rovColorBuffer->uav.get(), rovDepthBuffer->uav.get() };
			captureUAVCount = 3;
			break;
		}
	default:
		return false;
	}

	if (captureUAVCount == 0) {
		context->OMSetRenderTargets(captureRTVCount, captureRTVs.data(), captureDSV);
	} else {
		const std::array<UINT, 3> counters{ kKeepCounter, 1, kKeepCounter };
		context->OMSetRenderTargetsAndUnorderedAccessViews(captureRTVCount, captureRTVs.data(), captureDSV, captureRTVCount, captureUAVCount, captureUAVs.data(), counters.data());
	}

	cameraWorldInverse = RE::NiTransform{};
	if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->cameraRoot)
		cameraWorldInverse = playerCamera->cameraRoot->world.Invert();

	boundVariant = nullptr;
	boundEngineState = nullptr;
	techniqueCaptureReady = false;
	techniquePending = false;
	drawWriteDepth = false;
	drawTooFar = false;
	frameCapturedDraws = 0;
	framePendingDraws = 0;
	frameDepthDraws = 0;
	capturing = true;

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND, RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	return true;
}

void OrderIndependentTransparency::PreSetStateDirty()
{
	if (!capturing)
		return;

	globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	using enum RE::BSGraphics::DepthStencilDepthMode;
	auto& depthMode = globals::game::shadowState->GetRuntimeData().depthStencilDepthMode;
	if (depthMode == kTestWrite)
		depthMode = kTest;
	else if (depthMode == kWrite)
		depthMode = kDisabled;
}

void OrderIndependentTransparency::PreDrawHack()
{
	auto& descriptor = globals::state->permutationData.ExtraFeatureDescriptor;
	descriptor &= ~kDescriptorMask;

	if (!capturing)
		return;

	uint32_t blendMode = globals::game::shadowState->GetRuntimeData().alphaBlendMode;
	if (globals::features::effects11.loaded && globals::features::effects11.OverridesParticleBlend())
		blendMode = kBlendAlpha;
	const bool multiplicative = blendMode == kBlendMultiplicative || blendMode == kBlendMultiplicativeAlpha;
	const bool supportedBlend = blendMode >= kBlendAlpha && blendMode <= kBlendMultiplicative && (!multiplicative || settings.CaptureMultiplicativeLayer);
	const bool capture = techniqueCaptureReady && supportedBlend && !drawTooFar;
	const bool depth = techniqueCaptureReady && drawWriteDepth && settings.WriteDepth;

	if (blendMode == kBlendAdditive)
		descriptor |= kAdditiveDescriptor;
	else if (blendMode == kBlendMultiplicative)
		descriptor |= kMultiplicativeDescriptor;
	else if (blendMode == kBlendMultiplicativeAlpha)
		descriptor |= kMultiplicativeDescriptor | kAdditiveDescriptor;
	if (depth)
		descriptor |= kDepthWriteDescriptor;
	if (!capture)
		descriptor |= kDisabledDescriptor;

	if (capture)
		++frameCapturedDraws;
	else if (techniquePending)
		++framePendingDraws;
	if (depth)
		++frameDepthDraws;

	ApplyBlendVariant(capture, depth);

	if (settings.OverrideRenderTargets || !REL::Module::IsAE())
		BindCaptureTargets();
}

void OrderIndependentTransparency::ResolveTechnique(const RE::BSShader& a_shader, uint32_t& a_pixelDescriptor, bool a_skipPixelShader)
{
	techniqueCaptureReady = false;
	techniquePending = false;
	if (!capturing)
		return;

	const auto type = a_shader.shaderType.get();
	const auto mask = GetCaptureMask(type);
	if (mask == 0 || (a_pixelDescriptor & mask) == 0)
		return;

	const bool shaderEnabled = !a_skipPixelShader && globals::state->ShaderEnabled(type);
	if (shaderEnabled && globals::shaderCache->GetPixelShader(a_shader, a_pixelDescriptor) != nullptr) {
		techniqueCaptureReady = true;
	} else {
		techniquePending = shaderEnabled;
		a_pixelDescriptor &= ~mask;
	}
}

void OrderIndependentTransparency::SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!capturing || !a_pass)
		return;

	drawWriteDepth = a_pass->shaderProperty && a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite);

	drawTooFar = false;
	if (settings.DistanceThreshold < kInfiniteDistance && a_pass->geometry) {
		const auto viewPosition = cameraWorldInverse * a_pass->geometry->worldBound.center;
		drawTooFar = viewPosition.y >= settings.DistanceThreshold;
	}
}

void OrderIndependentTransparency::RestoreGeometry()
{
	drawWriteDepth = false;
	drawTooFar = false;
}

void OrderIndependentTransparency::EndAlphaGroup()
{
	capturing = false;
	techniqueCaptureReady = false;
	techniquePending = false;
	capturedDraws = frameCapturedDraws;
	pendingDraws = framePendingDraws;

	auto* renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto stateUpdateFlags = globals::game::stateUpdateFlags;

	context->OMSetRenderTargets(0, nullptr, nullptr);
	{
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(67, 1, &nullSRV);
	}

	globals::profiler->BeginPass("OrderIndependentTransparency::Composite");

	winrt::com_ptr<ID3D11VertexShader> previousVS;
	winrt::com_ptr<ID3D11PixelShader> previousPS;
	context->VSGetShader(previousVS.put(), nullptr, nullptr);
	context->PSGetShader(previousPS.put(), nullptr, nullptr);

	std::array<ID3D11ShaderResourceView*, 4> previousSRVs{};
	context->PSGetShaderResources(0, static_cast<UINT>(previousSRVs.size()), previousSRVs.data());

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& alphaOnly = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(globals::game::graphicsState->screenWidth);
	viewport.Height = static_cast<float>(globals::game::graphicsState->screenHeight);
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(globals::features::upscaling.GetUpscaleVS(), nullptr, 0);
	context->RSSetState(compositeRasterState.get());

	std::array<ID3D11ShaderResourceView*, 4> srvs{};
	switch (settings.Method) {
	case OITMethod::Fast:
		srvs = { accumFrontTexture->srv.get(), accumAllTexture->srv.get(), revealageTexture->srv.get(), nullptr };
		break;
	case OITMethod::Visualize:
	case OITMethod::Balanced:
		srvs = { GetMainDepthSRV(), headerTexture->srv.get(), nodesBuffer->srv.get(), nullptr };
		break;
	case OITMethod::Stable:
		srvs = { GetMainDepthSRV(), headerTexture->srv.get(), rovColorBuffer->srv.get(), rovDepthBuffer->srv.get() };
		break;
	default:
		break;
	}

	{
		ID3D11RenderTargetView* rtvs[2] = { main.RTV, alphaOnly.RTV };
		context->OMSetRenderTargets(2, rtvs, nullptr);
		context->OMSetDepthStencilState(compositeDepthState.get(), 0);
		SetOwnBlendState(compositeBlendState.get());
		context->PSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
		context->PSSetShader(resolvePS.get(), nullptr, 0);
		context->Draw(3, 0);
	}

	{
		std::array<ID3D11ShaderResourceView*, 4> nullSRVs{};
		context->PSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
	}

	if (settings.WriteDepth && frameDepthDraws > 0 && depthPS && mainDepth.views[0]) {
		winrt::com_ptr<ID3D11ShaderResourceView> sharedDepth;
		context->PSGetShaderResources(17, 1, sharedDepth.put());
		context->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);
		context->OMSetDepthStencilState(depthPassState.get(), 0);
		SetOwnBlendState(nullptr);
		auto* writeDepth = writeDepthTexture->srv.get();
		context->PSSetShaderResources(0, 1, &writeDepth);
		context->PSSetShader(depthPS.get(), nullptr, 0);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		auto* restoredDepth = sharedDepth.get();
		context->PSSetShaderResources(17, 1, &restoredDepth);
	}

	globals::profiler->EndPass();

	context->PSSetShaderResources(0, static_cast<UINT>(previousSRVs.size()), previousSRVs.data());
	for (auto* srv : previousSRVs) {
		if (srv)
			srv->Release();
	}
	context->VSSetShader(previousVS.get(), nullptr, 0);
	context->PSSetShader(previousPS.get(), nullptr, 0);

	boundVariant = nullptr;
	boundEngineState = nullptr;
	compositeValid = true;

	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET,
		RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT,
		RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE,
		RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_STENCILREF_MODE,
		RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE,
		RE::BSGraphics::ShaderFlags::DIRTY_RASTER_DEPTH_BIAS,
		RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND,
		RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC,
		RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
}

void OrderIndependentTransparency::BeginWater()
{
	if (!loaded || settings.Method == OITMethod::Disabled || !alphaOnlyReady || resourcesFailed)
		return;

	auto* srv = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_ONLY_ALPHA].SRV;
	globals::d3d::context->PSSetShaderResources(66, 1, &srv);
}

void OrderIndependentTransparency::DrawSettings()
{
	const auto previousMethod = settings.Method;
	const auto previousNodeCount = GetNodeCount();

	if (ImGui::TreeNodeEx(T(TKEY("method"), "Method"), ImGuiTreeNodeFlags_DefaultOpen)) {
		auto radio = [this](const char* a_label, OITMethod a_method, const char* a_tooltip) {
			if (ImGui::RadioButton(a_label, settings.Method == a_method))
				settings.Method = a_method;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextWrapped("%s", a_tooltip);
		};

		radio(T(TKEY("method_disabled"), "Disabled"), OITMethod::Disabled,
			T(TKEY("method_disabled_tooltip"), "Transparent surfaces render in the game's own order."));
		radio(T(TKEY("method_blended"), "Fast Approximation"), OITMethod::Fast,
			T(TKEY("method_blended_tooltip"), "Weighted blended OIT. Cheapest; best for fog, smoke, hair and grass.\nDense or highly opaque stacks are approximated."));
		radio(T(TKEY("method_adaptive"), "Balanced"), OITMethod::Balanced,
			T(TKEY("method_adaptive_tooltip"), "Per-pixel fragment lists. The nearest layers are sorted exactly, layers past Maximum Layers are blended.\nStable ordering, memory-bound cost."));
		ImGui::BeginDisabled(!rovSupported);
		radio(T(TKEY("method_stable"), "Stable"), OITMethod::Stable,
			T(TKEY("method_stable_tooltip"), "Adaptive OIT with rasterizer ordered views. Requires DirectX 11.3.\nMost expensive, especially above 4 layers."));
		ImGui::EndDisabled();
		radio(T(TKEY("method_visualize"), "Visualize Layers"), OITMethod::Visualize,
			T(TKEY("method_visualize_tooltip"), "Shows the transparent layer count per pixel, blue (1) to purple (8+)."));
		ImGui::TreePop();
	}

	if (settings.Method == OITMethod::Fast && ImGui::TreeNodeEx(T(TKEY("wboit_parameters"), "Tuning"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("wboit_additive_alpha_scale"), "Additive Alpha Scale"), &settings.WBOITAdditiveAlphaScale, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("wboit_additive_alpha_scale_tooltip"), "Raise when additive effects show hard edges."));
		ImGui::SliderFloat(T(TKEY("wboit_min_projected_distance"), "Minimum Projected Distance"), &settings.WBOITMinProjectedDistance, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("wboit_min_projected_distance_tooltip"), "Raise when rain or snow looks overbright."));
		ImGui::SliderFloat(T(TKEY("wboit_min_final_weight"), "Minimum Final Weight"), &settings.WBOITWeightMin, 0.0f, settings.WBOITWeightMax, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("wboit_max_final_weight"), "Maximum Final Weight"), &settings.WBOITWeightMax, settings.WBOITWeightMin, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("wboit_final_weight_tooltip"), "A wider range favours close layers (clothing), a narrower one distant layers (fog)."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("compatibility"), "Compatibility"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("multiplicative_blend_support"), "Multiplicative Blend Support"), &settings.CaptureMultiplicativeLayer);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("multiplicative_blend_support_tooltip"), "Captures greyscale multiplicative layers. When off they render in game order."));
		ImGui::Checkbox(T(TKEY("enforce_render_target"), "Enforce Render Targets"), &settings.OverrideRenderTargets);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("enforce_render_target_tooltip"), "Rebinds OIT targets on every draw. Costs CPU; only for transparent meshes that vanish. Always on for SE."));
		ImGui::Checkbox(T(TKEY("write_depth"), "Write Depth"), &settings.WriteDepth);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("write_depth_tooltip"), "Writes the nearest depth of transparent meshes flagged to write depth, as the game does."));
		ImGui::BeginDisabled(!settings.WriteDepth);
		ImGui::SliderFloat(T(TKEY("write_depth_threshold"), "Write Depth Alpha"), &settings.WriteDepthThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("write_depth_threshold_tooltip"), "Pixels below this alpha do not write depth."));
		ImGui::EndDisabled();
		ImGui::SliderFloat(T(TKEY("ssr_alpha_scale"), "SSR Alpha Scale"), &settings.SSRAlphaScale, 0.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("ssr_alpha_scale_tooltip"), "Scales how strongly transparent surfaces show in screen-space reflections."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("performance"), "Performance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(T(TKEY("pass_time"), "Pass Time      = %6.2f ms"), passTime);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("pass_time_tooltip"), "CPU time spent in the game's transparent pass."));
		ImGui::Text(T(TKEY("composite_time"), "Composite Time = %6.2f ms"), compositeTime);
		ImGui::Text(T(TKEY("captured_draws"), "Captured Draws = %u"), capturedDraws);
		ImGui::Text(T(TKEY("pending_draws"), "Pending Draws  = %u"), pendingDraws);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("pending_draws_tooltip"), "Draws rendered in game order while their OIT shaders compile."));

		auto applyOnRelease = [](const char* a_label, uint& a_value, uint& a_edit, bool& a_editing, int a_min, int a_max, int a_step) {
			if (!a_editing)
				a_edit = a_value;
			int value = static_cast<int>(a_edit);
			if (ImGui::SliderInt(a_label, &value, a_min, a_max, "%d", ImGuiSliderFlags_AlwaysClamp))
				a_edit = static_cast<uint>(std::clamp(((value + a_step / 2) / a_step) * a_step, a_min, a_max));
			a_editing = ImGui::IsItemActive();
			if (ImGui::IsItemDeactivatedAfterEdit())
				a_value = a_edit;
		};

		ImGui::BeginDisabled(!UsesFragmentList());
		applyOnRelease(T(TKEY("pixel_buffer_size"), "Pixel Buffer Size"), settings.BufferSize, editBufferSize, editingBufferSize, 2, 16, 1);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("pixel_buffer_size_tooltip"), "Fragments reserved per pixel. Overflowing fragments render in game order."));
		ImGui::EndDisabled();

		ImGui::BeginDisabled(settings.Method != OITMethod::Balanced && settings.Method != OITMethod::Stable);
		applyOnRelease(T(TKEY("max_layers"), "Maximum Layers"), settings.MaxLayers, editMaxLayers, editingMaxLayers, 4, 32, 4);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("max_layers_tooltip"), "Layers sorted exactly per pixel. Higher costs more."));
		ImGui::EndDisabled();

		ImGui::SliderFloat(T(TKEY("alpha_cutoff"), "Alpha Cutoff"), &settings.AlphaThreshold, 0.0f, 0.1f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("alpha_cutoff_tooltip"), "Remaps alpha below this value to zero. Above 0.01 can fade rain."));

		float distance = settings.DistanceThreshold;
		if (ImGui::SliderFloat(T(TKEY("distance_threshold"), "Distance Threshold"), &distance, 0.0f, kInfiniteDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
			settings.DistanceThreshold = distance;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("distance_threshold_tooltip"), "Meshes farther than this render in game order. The maximum captures everything."));
		ImGui::TreePop();
	}

	if (resourcesFailed)
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", T(TKEY("resource_failure"), "OIT resources could not be allocated; see the log."));

	NormalizeSettings();
	if (settings.Method != previousMethod)
		OnMethodChanged();
	else if (resourcesFailed && UsesFragmentList() && settings.BufferSize != resourceBufferSize)
		resourcesFailed = false;
	if (settings.Method != previousMethod || GetNodeCount() != previousNodeCount)
		SyncMaterialNodeCount();
}

#undef I18N_KEY_PREFIX
