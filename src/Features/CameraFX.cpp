#include "CameraFX.h"

#include "Features/Effects11.h"
#include "Features/HDRDisplay.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Features/PostProcessing/ImageSpaceModifiers.h"
#include "Features/ProceduralSun.h"
#include "Features/ReverseZ.h"
#include "Features/SkySync.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/Moon.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.camera_fx."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CameraFX::Settings,
	Enabled,
	ReplaceModifiers,
	ColdFrost,
	FrostStrength,
	LowHealth,
	LowHealthStrength,
	HeartbeatBlur,
	Exhaustion,
	ExhaustionStrength,
	Hunger,
	HungerStrength,
	Poison,
	PoisonStrength,
	Transformation,
	TransformationStrength,
	SunLens,
	SunLensStrength,
	LensDirt,
	SunMaxHeight,
	MoonLens,
	MoonLensStrength,
	RainDrops,
	RainStrength,
	WaterSheet,
	WeatherFrost,
	WeatherFrostStrength,
	ColdBreath,
	EntryFog,
	FogCondensation,
	Atmosphere,
	AtmosphereStrength,
	Combat,
	Blood,
	BloodStrength,
	BloodBlur,
	BloodBlurStrength,
	ElementalSplats,
	ImpactFlash,
	FlashStrength,
	SlashStreaks,
	HitShake,
	HitShakeStrength,
	HitBlur,
	HitBlurStrength,
	StaminaBreath,
	PhotosensitiveSafe)

namespace
{
	using Theme = CameraFXModifiers::Theme;

	constexpr uint32_t kSceneMipLevels = 6;
	constexpr float kMaxStrength = 2.0f;
	constexpr float kMinSunMaxHeight = 0.2f;
	constexpr float kMaxSunMaxHeight = 1.0f;
	constexpr float kActiveEpsilon = 1e-3f;
	constexpr float kFrostGrowRate = 0.25f;
	constexpr float kFrostMeltRate = 0.35f;
	constexpr float kLightEaseRate = 1.5f;
	constexpr float kVisibilityRate = 6.0f;
	constexpr float kVisibilityFallRate = 30.0f;
	constexpr float kThemeRiseRate = 1.5f;
	constexpr float kThemeFallRate = 0.75f;
	constexpr uint32_t kMaxFramesSinceDraw = 2;
	constexpr float kMinBlurRadius = 0.01f;
	constexpr float kLensModeScene = 0.0f;
	constexpr float kLensModeUI = 1.0f;
	constexpr float kLensModeUIOverlay = 2.0f;
	constexpr float kLensModeSceneAdditive = 3.0f;
	constexpr float kCelestialDistance = 100000.0f;
	constexpr float kScreenMargin = 0.15f;
	constexpr float kReferenceHeight = 1080.0f;
	constexpr float kBlurPixelsPerUnit = 1.5f;
	constexpr float kDoubleVisionUVPerUnit = 0.004f;
	constexpr float kMasserRadius = 0.035f;
	constexpr float kSecundaRadius = 0.022f;
	constexpr float kSecundaLensScale = 0.6f;
	constexpr float kSceneCopyIdleSeconds = 10.0f;
	constexpr float kContrastPivot = 0.5f;
	constexpr float kFrostIdleSeconds = 30.0f;
	constexpr UINT kLensResourceCount = 5;
	constexpr UINT kLensSamplerCount = 2;
	constexpr UINT kDirtSize = 1024;
	constexpr UINT kFrostWidth = 2048;
	constexpr UINT kFrostHeight = 1152;
	constexpr UINT kBakeGroupSize = 8;
	constexpr float kSwimDropsFromSheet = 0.8f;
	constexpr float kThawStart = 0.05f;
	constexpr float kBeastRiseRate = 8.0f;
	constexpr float kBeastFallRate = 1.0f;
	constexpr float kBeastBeatsPerMinute = 110.0f;
	constexpr float kBeastRestartGap = 0.5f;
	constexpr float kDefaultLowHealthInterval = 2.0f;
	constexpr float kDefaultHeartbeatSlow = 0.2f;
	constexpr float kDefaultHeartbeatFast = 0.1f;
	constexpr float kLowHealthHoldMargin = 0.25f;
	constexpr float kLowHealthRecoveryMargin = 0.01f;
	constexpr float kLowHealthFloor = 0.6f;
	constexpr float kCalmHeartRate = 64.0f;
	constexpr float kLowHealthHeartRate = 92.0f;
	constexpr float kSafeHeartRate = 72.0f;

	float Beat(float a_phase, float a_centre, float a_sharpness)
	{
		float offset = a_phase - a_centre;
		offset -= std::round(offset);
		return std::exp(-offset * offset * a_sharpness);
	}

	float ReadSetting(const RE::Setting* a_setting, float a_default, float a_min, float a_max)
	{
		return a_setting && a_setting->GetType() == RE::Setting::Type::kFloat ? Util::ClampFinite(a_setting->GetFloat(), a_default, a_min, a_max) : a_default;
	}

	struct MenuManager_DrawInterfaceStart
	{
		static void thunk(int64_t a1)
		{
			globals::features::cameraFX.DrawBeforeInterface();
			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <class T>
	void SafeRelease(T*& a_object)
	{
		if (a_object) {
			a_object->Release();
			a_object = nullptr;
		}
	}

	class StateBackup
	{
	public:
		explicit StateBackup(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthStencilView);
			context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depthStencilState, &stencilRef);
			context->RSGetState(&rasterizerState);
			context->RSGetViewports(&viewportCount, viewports);
			context->IAGetInputLayout(&inputLayout);
			context->IAGetPrimitiveTopology(&topology);
			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->HSGetShader(&hullShader, nullptr, nullptr);
			context->DSGetShader(&domainShader, nullptr, nullptr);
			context->GSGetShader(&geometryShader, nullptr, nullptr);
			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->PSGetConstantBuffers(0, 1, &psBuffer);
			context->PSGetShaderResources(0, kLensResourceCount, psResources);
			context->PSGetSamplers(0, kLensSamplerCount, psSamplers);
			context->CSGetShader(&computeShader, nullptr, nullptr);
			context->CSGetConstantBuffers(0, 1, &csBuffer);
			context->CSGetShaderResources(0, 1, &csResource);
			context->CSGetUnorderedAccessViews(0, 1, &csUAV);
		}

		StateBackup(const StateBackup&) = delete;
		StateBackup& operator=(const StateBackup&) = delete;

		~StateBackup()
		{
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthStencilView);
			context->OMSetBlendState(blendState, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depthStencilState, stencilRef);
			context->RSSetState(rasterizerState);
			context->RSSetViewports(viewportCount, viewports);
			context->IASetInputLayout(inputLayout);
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->HSSetShader(hullShader, nullptr, 0);
			context->DSSetShader(domainShader, nullptr, 0);
			context->GSSetShader(geometryShader, nullptr, 0);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &psBuffer);
			context->PSSetShaderResources(0, kLensResourceCount, psResources);
			context->PSSetSamplers(0, kLensSamplerCount, psSamplers);
			context->CSSetShader(computeShader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &csBuffer);
			context->CSSetShaderResources(0, 1, &csResource);
			context->CSSetUnorderedAccessViews(0, 1, &csUAV, nullptr);

			for (auto*& view : renderTargets)
				SafeRelease(view);
			SafeRelease(depthStencilView);
			SafeRelease(blendState);
			SafeRelease(depthStencilState);
			SafeRelease(rasterizerState);
			SafeRelease(inputLayout);
			SafeRelease(vertexShader);
			SafeRelease(hullShader);
			SafeRelease(domainShader);
			SafeRelease(geometryShader);
			SafeRelease(pixelShader);
			SafeRelease(psBuffer);
			for (auto*& view : psResources)
				SafeRelease(view);
			for (auto*& sampler : psSamplers)
				SafeRelease(sampler);
			SafeRelease(computeShader);
			SafeRelease(csBuffer);
			SafeRelease(csResource);
			SafeRelease(csUAV);
		}

	private:
		ID3D11DeviceContext* context;
		ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* depthStencilView = nullptr;
		ID3D11BlendState* blendState = nullptr;
		FLOAT blendFactor[4] = {};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthStencilState = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* rasterizerState = nullptr;
		UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11HullShader* hullShader = nullptr;
		ID3D11DomainShader* domainShader = nullptr;
		ID3D11GeometryShader* geometryShader = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11Buffer* psBuffer = nullptr;
		ID3D11ShaderResourceView* psResources[kLensResourceCount] = {};
		ID3D11SamplerState* psSamplers[kLensSamplerCount] = {};
		ID3D11ComputeShader* computeShader = nullptr;
		ID3D11Buffer* csBuffer = nullptr;
		ID3D11ShaderResourceView* csResource = nullptr;
		ID3D11UnorderedAccessView* csUAV = nullptr;
	};

	class IndentScope
	{
	public:
		IndentScope() { ImGui::Indent(); }
		~IndentScope() { ImGui::Unindent(); }
		IndentScope(const IndentScope&) = delete;
		IndentScope& operator=(const IndentScope&) = delete;
	};

	DXGI_FORMAT GetTypedFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default:
			return a_format;
		}
	}

	bool ViewsResource(ID3D11View* a_view, ID3D11Resource* a_resource)
	{
		if (!a_view || !a_resource)
			return false;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return resource.get() == a_resource;
	}

	float Ease(float a_current, float a_target, float a_rate, float a_deltaTime)
	{
		return a_current + (a_target - a_current) * std::min(1.0f, a_rate * a_deltaTime);
	}

	float GetClearSky(const RE::TESWeather* a_weather)
	{
		using Flag = RE::TESWeather::WeatherDataFlag;
		if (!a_weather)
			return 0.0f;
		const auto& flags = a_weather->data.flags;
		if (flags.any(Flag::kRainy, Flag::kSnow))
			return 0.0f;
		if (flags.any(Flag::kCloudy))
			return 0.35f;
		if (flags.any(Flag::kPleasant))
			return 1.0f;
		return 0.6f;
	}

	float BlendWeathers(const RE::Sky* a_sky, float (*a_value)(const RE::TESWeather*))
	{
		const float pct = std::clamp(a_sky->currentWeatherPct, 0.0f, 1.0f);
		const float current = a_value(a_sky->currentWeather);
		return a_sky->lastWeather ? std::lerp(a_value(a_sky->lastWeather), current, pct) : current;
	}

	float GetSunGlare(const RE::TESWeather* a_weather)
	{
		return a_weather ? a_weather->data.sunGlare / 255.0f : 0.0f;
	}

	float GetMoonDiscVisibility(const RE::Moon* a_moon)
	{
		if (!a_moon || !a_moon->root || !a_moon->moonNode || !a_moon->moonMesh ||
			a_moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) || a_moon->moonNode->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return 0.0f;
		const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(a_moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get());
		return prop ? std::clamp(prop->kBlendColor.alpha, 0.0f, 1.0f) : 0.0f;
	}

	CameraFXModifiers::Blend GetModifierBlend()
	{
		auto& postProcessing = globals::features::postProcessing;
		if (!postProcessing.loaded || !postProcessing.IsPipelineRunning())
			return {};
		const auto* grading = postProcessing.GetPipelineFeature<ColorGrading>(PostProcessing::FeaturePipelineIndex::ColorGrading);
		if (!grading || !grading->IsActive())
			return {};
		const auto& grade = grading->settings;
		CameraFXModifiers::Blend blend;
		blend.baseCinematic = { Util::ClampFinite(grade.gameCinematicBlend.x, 0.0f, 0.0f, 1.0f), Util::ClampFinite(grade.gameCinematicBlend.y, 0.0f, 0.0f, 1.0f),
			Util::ClampFinite(grade.gameCinematicBlend.z, 0.0f, 0.0f, 1.0f) };
		blend.baseTint = Util::ClampFinite(grade.gameTintBlend, 0.0f, 0.0f, 1.0f);
		blend.modifiers = Util::ClampFinite(grade.gameModifierBlend, 1.0f, 0.0f, 1.0f);
		blend.fade = Util::ClampFinite(grade.gameFadeBlend, 1.0f, 0.0f, 1.0f);
		return blend;
	}

	bool ProjectDirection(const RE::NiPoint3& a_direction, float2& o_uv, float& o_onScreen)
	{
		o_uv = { 0.5f, 0.5f };
		o_onScreen = 0.0f;
		auto* camera = RE::Main::WorldRootCamera();
		if (!camera)
			return false;
		const RE::NiPoint3 point = camera->world.translate + a_direction * kCelestialDistance;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		if (!camera->WorldPtToScreenPt3(point, x, y, z, 1e-5f) || z <= 0.0f || !std::isfinite(x) || !std::isfinite(y))
			return false;
		o_uv = { x, 1.0f - y };
		const float inside = std::min({ x, y, 1.0f - x, 1.0f - y });
		o_onScreen = Util::Smoothstep(-kScreenMargin, kScreenMargin, inside);
		return o_onScreen > 0.0f;
	}
}

void CameraFX::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "CameraFX::FrameData");
	visibilityConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VisibilityData>(), "CameraFX::VisibilityData");

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 3;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		visibility = eastl::make_unique<Texture2D>(desc, "CameraFX::LightVisibility");
		visibility->CreateSRV(srvDesc);
		visibility->CreateUAV(uavDesc);
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(visibility->uav.get(), zero);
	}

	{
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "CameraFX::LinearSampler");

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, wrapSampler.put()));
		Util::SetResourceName(wrapSampler.get(), "CameraFX::WrapSampler");
	}

	{
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, opaqueBlend.put()));
		Util::SetResourceName(opaqueBlend.get(), "CameraFX::OpaqueBlend");
	}

	{
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, lensSceneBlend.put()));
		Util::SetResourceName(lensSceneBlend.get(), "CameraFX::LensSceneBlend");

		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, lensUIBlend.put()));
		Util::SetResourceName(lensUIBlend.get(), "CameraFX::LensUIBlend");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "CameraFX::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "CameraFX::DepthStencilState");
	}

	EnsurePipeline();
}

void CameraFX::ClearShaderCache()
{
	vertexShader = nullptr;
	scenePixelShader = nullptr;
	lensPixelShader = nullptr;
	combatLensPixelShader = nullptr;
	visibilityShader = nullptr;
	dirtBakeShader = nullptr;
	frostBakeShader = nullptr;
	vertexShaderFailed = false;
	sceneShaderFailed = false;
	lensShaderFailed = false;
	combatLensShaderFailed = false;
	visibilityShaderFailed = false;
	dirtBakeShaderFailed = false;
	frostBakeShaderFailed = false;
	dirtBakeFailed = false;
	frostBakeFailed = false;
	lensDirt.reset();
	frostMask.reset();
	pipelineReady = false;
}

ID3D11VertexShader* CameraFX::GetVS()
{
	if (!vertexShader && !vertexShaderFailed) {
		vertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\FullscreenVS.hlsl", {}, "vs_5_0")));
		vertexShaderFailed = !vertexShader;
	}
	return vertexShader.get();
}

ID3D11PixelShader* CameraFX::GetScenePS()
{
	if (!scenePixelShader && !sceneShaderFailed) {
		scenePixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\ScenePS.hlsl", {}, "ps_5_0")));
		sceneShaderFailed = !scenePixelShader;
	}
	return scenePixelShader.get();
}

ID3D11PixelShader* CameraFX::GetLensPS()
{
	if (!lensPixelShader && !lensShaderFailed) {
		lensPixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\LensPS.hlsl", {}, "ps_5_0")));
		lensShaderFailed = !lensPixelShader;
	}
	return lensPixelShader.get();
}

ID3D11PixelShader* CameraFX::GetCombatLensPS()
{
	if (!combatLensPixelShader && !combatLensShaderFailed) {
		combatLensPixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\LensPS.hlsl", { { "COMBAT_PASS", "" } }, "ps_5_0")));
		combatLensShaderFailed = !combatLensPixelShader;
	}
	return combatLensPixelShader.get();
}

ID3D11ComputeShader* CameraFX::GetVisibilityCS()
{
	if (!visibilityShader && !visibilityShaderFailed) {
		visibilityShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\LightVisibilityCS.hlsl", {}, "cs_5_0")));
		visibilityShaderFailed = !visibilityShader;
	}
	return visibilityShader.get();
}

ID3D11ComputeShader* CameraFX::GetBakeCS(bool a_frost)
{
	auto& shader = a_frost ? frostBakeShader : dirtBakeShader;
	bool& failed = a_frost ? frostBakeShaderFailed : dirtBakeShaderFailed;
	if (!shader && !failed) {
		shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CameraFX\\LensBakeCS.hlsl", { { a_frost ? "BAKE_FROST" : "BAKE_DIRT", "" } }, "cs_5_0")));
		failed = !shader;
	}
	return shader.get();
}

bool CameraFX::EnsurePipeline()
{
	pipelineReady = GetVS() && GetScenePS() && GetLensPS();
	if (pipelineReady) {
		GetVisibilityCS();
		GetCombatLensPS();
		GetBakeCS(false);
		GetBakeCS(true);
	}
	if (!pipelineReady && !pipelineFailureLogged) {
		pipelineFailureLogged = true;
		logger::error("[CameraFX] Shaders failed to compile, leaving screen effects to the game");
	}
	return pipelineReady;
}

void CameraFX::Load()
{
	stl::detour_thunk<MenuManager_DrawInterfaceStart>(REL::RelocationID(79947, 82084));
	logger::info("[CameraFX] Installed interface hook");
}

void CameraFX::PostPostLoad()
{
	ImageSpaceModifiers::Install();
}

void CameraFX::DataLoaded()
{
	CameraFXModifiers::ResolveForms();
	CameraFXLens::WeatherLens::ResolveForms();
	combat.Register();

	lowHealthIntervalSetting = RE::GetINISetting("fLowHealthIModInterval:Display");
	if (auto* gameSettings = RE::GameSettingCollection::GetSingleton()) {
		heartbeatSlowSetting = gameSettings->GetSetting("fPlayerHealthHeartbeatSlow");
		heartbeatFastSetting = gameSettings->GetSetting("fPlayerHealthHeartbeatFast");
	}
}

void CameraFX::GameLoaded()
{
	weather.Reset();
	combat.Reset();
	frost = 0.0f;
	sunAmount = 0.0f;
	moonAmount = {};
	themeLevels = {};
	lowHealthHoldSeconds = 0.0f;
	lowHealthSeverity = 0.0f;
	beastAge = 99.0f;
	beastLevel = 0.0f;
	beastAbsentSeconds = 99.0f;
	beastVampire = false;
}

bool CameraFX::TakesOverModifiers() const
{
	auto state = globals::state;
	return loaded && settings.Enabled && settings.ReplaceModifiers && pipelineReady && !sceneCopyFailed && sceneTargetAvailable && state &&
	       !state->IsMenuSceneOpen() && state->frameCount - lastDrawnFrame <= kMaxFramesSinceDraw;
}

std::array<bool, CameraFXModifiers::kThemeCount> CameraFX::GetEnabledThemes() const
{
	std::array<bool, CameraFXModifiers::kThemeCount> themes{};
	themes[static_cast<size_t>(Theme::kCold)] = settings.ColdFrost;
	themes[static_cast<size_t>(Theme::kLowHealth)] = settings.LowHealth;
	themes[static_cast<size_t>(Theme::kExhaustion)] = settings.Exhaustion;
	themes[static_cast<size_t>(Theme::kHunger)] = settings.Hunger;
	themes[static_cast<size_t>(Theme::kPoison)] = settings.Poison;
	themes[static_cast<size_t>(Theme::kWerewolf)] = settings.Transformation;
	themes[static_cast<size_t>(Theme::kVampireLord)] = settings.Transformation;
	return themes;
}

CameraFX::SceneTarget CameraFX::GetSceneTarget() const
{
	auto& hdr = globals::features::hdrDisplay;
	const bool hdrRequested = hdr.loaded && hdr.settings.enableHDR;
	if (hdrRequested && hdr.hdrTexture && hdr.hdrTexture->rtv && hdr.GetHDROutputCS())
		return { hdr.hdrTexture->resource.get(), hdr.hdrTexture->rtv.get(), true };

	auto& upscaling = globals::features::upscaling;
	if (upscaling.loaded && upscaling.d3d12SwapChainActive && !hdrRequested) {
		auto* wrapped = upscaling.dx12SwapChain.swapChainBufferWrapped;
		if (wrapped && wrapped->resource11 && wrapped->rtv)
			return { wrapped->resource11, wrapped->rtv };
	}

	auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
	if (!ViewsResource(framebuffer.RTV, framebuffer.texture))
		return {};
	return { framebuffer.texture, framebuffer.RTV };
}

ID3D11RenderTargetView* CameraFX::GetLensTarget(const SceneTarget& a_scene, bool& o_uiLayer) const
{
	o_uiLayer = false;
	auto& upscaling = globals::features::upscaling;
	if (upscaling.loaded && upscaling.d3d12SwapChainActive && upscaling.ShouldUseFrameGenerationThisFrame()) {
		auto* ui = upscaling.dx12SwapChain.uiBufferWrapped;
		if (ui && ui->rtv) {
			o_uiLayer = true;
			return ui->rtv;
		}
	}
	return a_scene.rtv;
}

bool CameraFX::EnsureSceneCopy(const D3D11_TEXTURE2D_DESC& a_desc)
{
	const DXGI_FORMAT format = GetTypedFormat(a_desc.Format);
	if (sceneCopy && sceneCopy->desc.Width == a_desc.Width && sceneCopy->desc.Height == a_desc.Height && sceneCopy->desc.Format == format)
		return true;

	sceneCopy.reset();

	UINT support = 0;
	const bool mipAutogen = SUCCEEDED(globals::d3d::device->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_desc.Width;
	desc.Height = a_desc.Height;
	desc.MipLevels = mipAutogen ? kSceneMipLevels : 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mipAutogen ? D3D11_BIND_RENDER_TARGET : 0);
	desc.MiscFlags = mipAutogen ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;

	try {
		sceneCopy = eastl::make_unique<Texture2D>(desc, "CameraFX::SceneCopy");
		sceneCopy->CreateSRV(srvDesc);
	} catch (const std::exception& e) {
		logger::error("[CameraFX] Failed to create the scene copy ({}), leaving screen effects to the game", e.what());
		sceneCopy.reset();
		sceneCopyFailed = true;
		return false;
	}
	return true;
}

void CameraFX::EnsureLensTextures(bool a_needFrost, float a_deltaTime)
{
	auto context = globals::d3d::context;
	auto bake = [&](eastl::unique_ptr<Texture2D>& a_texture, bool& a_failed, bool a_frost, UINT a_width, UINT a_height, DXGI_FORMAT a_format, const char* a_name) {
		if (a_texture || a_failed)
			return;
		auto* shader = GetBakeCS(a_frost);
		if (!shader) {
			a_failed = true;
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = a_format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = a_format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		try {
			a_texture = eastl::make_unique<Texture2D>(desc, a_name);
			a_texture->CreateSRV(srvDesc);
			a_texture->CreateUAV(uavDesc);
		} catch (const std::exception& e) {
			logger::error("[CameraFX] Failed to create {} ({}), using the procedural fallback", a_name, e.what());
			a_texture.reset();
			a_failed = true;
			return;
		}

		ID3D11UnorderedAccessView* uav = a_texture->uav.get();
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch((a_width + kBakeGroupSize - 1) / kBakeGroupSize, (a_height + kBakeGroupSize - 1) / kBakeGroupSize, 1);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	};

	bake(lensDirt, dirtBakeFailed, false, kDirtSize, kDirtSize, DXGI_FORMAT_R8_UNORM, "CameraFX::LensDirt");

	if (a_needFrost) {
		frostIdleSeconds = 0.0f;
		bake(frostMask, frostBakeFailed, true, kFrostWidth, kFrostHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "CameraFX::FrostMask");
	} else if (frostMask) {
		frostIdleSeconds += a_deltaTime;
		if (frostIdleSeconds > kFrostIdleSeconds)
			frostMask.reset();
	}

	frameData.LensLook.y = frostMask ? 1.0f : 0.0f;
	frameData.LensLook.z = lensDirt ? 1.0f : 0.0f;
}

void CameraFX::UpdateThemeClocks(float a_deltaTime)
{
	const float lowHealth = std::min(frameData.Themes.x, 1.0f);
	const float exhaustion = std::min(frameData.Themes.y, 1.0f);

	float beatsPerMinute = std::lerp(kCalmHeartRate, kLowHealthHeartRate, lowHealthSeverity);
	if (settings.PhotosensitiveSafe)
		beatsPerMinute = std::min(beatsPerMinute, kSafeHeartRate);
	heartPhase = std::fmod(heartPhase + a_deltaTime * beatsPerMinute / 60.0f, 1.0f);
	const float echo = settings.PhotosensitiveSafe ? 0.0f : 0.35f * Beat(heartPhase, 0.2f, 80.0f);
	const float pulse = lowHealth > 0.0f ? std::min(Beat(heartPhase, 0.0f, 60.0f) + echo, 1.0f) : 0.0f;

	float eyelid = 0.0f;
	if (exhaustion > 0.0f) {
		blinkTimer -= a_deltaTime;
		blinkAge += a_deltaTime;
		if (blinkTimer <= 0.0f) {
			static uint32_t blinkCount = 0;
			++blinkCount;
			const float jitter = 0.75f + 0.5f * std::fmod(blinkCount * 0.6180339f, 1.0f);
			blinkTimer = std::lerp(7.0f, 2.5f, exhaustion) * jitter;
			blinkAge = 0.0f;
		}
		const float duration = std::lerp(0.25f, 0.7f, exhaustion);
		const float closure = blinkAge < duration ? std::sin(RE::NI_PI * blinkAge / duration) : 0.0f;
		const float droop = 0.12f * exhaustion;
		eyelid = std::clamp(droop + (1.0f - droop) * closure * std::min(1.0f, 0.6f + 0.4f * exhaustion), 0.0f, 1.0f);
	} else {
		blinkTimer = 0.0f;
		blinkAge = 99.0f;
	}

	hungerPhase = std::fmod(hungerPhase + a_deltaTime * 0.22f, 1.0f);
	const float hungerPulse = 0.5f - 0.5f * std::cos(2.0f * RE::NI_PI * hungerPhase);

	float beastPulse = 0.0f;
	if (beastLevel > 0.0f) {
		beastPhase = std::fmod(beastPhase + a_deltaTime * kBeastBeatsPerMinute / 60.0f, 1.0f);
		beastPulse = std::max(Beat(beastPhase, 0.0f, 120.0f), 0.6f * Beat(beastPhase, 0.2f, 120.0f));
	} else {
		beastPhase = 0.0f;
	}

	frameData.ThemeParams = { pulse, eyelid, hungerPulse, time };
	frameData.Transform = { beastLevel, beastVampire ? 1.0f : 0.0f, beastPulse, 0.0f };
}

float CameraFX::UpdateLowHealth(float a_presence, float a_step)
{
	const float slow = ReadSetting(heartbeatSlowSetting, kDefaultHeartbeatSlow, 0.0f, 1.0f);
	const float fast = std::min(ReadSetting(heartbeatFastSetting, kDefaultHeartbeatFast, 0.0f, 1.0f), slow);
	const float health = CameraFXLens::GetHealthFraction(RE::PlayerCharacter::GetSingleton());
	lowHealthSeverity = slow > fast ? 1.0f - std::clamp((health - fast) / (slow - fast), 0.0f, 1.0f) : 1.0f;

	if (a_presence > kActiveEpsilon)
		lowHealthHoldSeconds = ReadSetting(lowHealthIntervalSetting, kDefaultLowHealthInterval, 0.5f, 10.0f) + kLowHealthHoldMargin;
	else if (health > slow + kLowHealthRecoveryMargin)
		lowHealthHoldSeconds = 0.0f;
	else
		lowHealthHoldSeconds = std::max(0.0f, lowHealthHoldSeconds - a_step);
	return lowHealthHoldSeconds > 0.0f ? std::lerp(kLowHealthFloor, 1.0f, lowHealthSeverity) : 0.0f;
}

void CameraFX::UpdateLights(RE::Sky* a_sky)
{
	frameData.SunScreen = {};
	frameData.SunColor = {};
	frameData.MoonScreen[0] = {};
	frameData.MoonScreen[1] = {};
	visibilityData.Lights[0] = {};
	visibilityData.Lights[1] = {};
	visibilityData.Lights[2] = {};

	if (!a_sky || a_sky->mode.get() != RE::Sky::Mode::kFull || Util::IsInterior())
		return;

	const auto& skySync = globals::features::skySync;
	const auto sunDirection = skySync.GetCelestialDirection(a_sky, SkySync::Caster::Sun);
	if (sunAmount > kActiveEpsilon) {
		float2 uv{};
		float onScreen = 0.0f;
		if (ProjectDirection(sunDirection, uv, onScreen)) {
			const float aspect = frameData.Resolution.x / std::max(frameData.Resolution.y, 1.0f);
			frameData.SunScreen = { uv.x, uv.y, onScreen, aspect };
			visibilityData.Lights[0] = { uv.x, uv.y, 1.0f, 0.0f };
		}
		const auto& sunColor = a_sky->skyColor[RE::TESWeather::ColorTypes::kSun];
		const float peak = std::max({ sunColor.red, sunColor.green, sunColor.blue, 1e-3f });
		frameData.SunColor = { sunColor.red / peak, sunColor.green / peak, sunColor.blue / peak, 0.0f };
	}

	const float night = Util::Smoothstep(0.0f, 0.1f, -sunDirection.z);
	const std::pair<RE::Moon*, SkySync::Caster> moons[2] = { { a_sky->masser, SkySync::Caster::Masser }, { a_sky->secunda, SkySync::Caster::Secunda } };
	for (size_t i = 0; i < 2; ++i) {
		const auto [moon, caster] = moons[i];
		if (moonAmount[i] <= kActiveEpsilon || !moon || night <= 0.0f)
			continue;
		float2 uv{};
		float onScreen = 0.0f;
		if (!ProjectDirection(skySync.GetCelestialDirection(a_sky, caster), uv, onScreen))
			continue;
		frameData.MoonScreen[i] = { uv.x, uv.y, moonAmount[i] * onScreen, i == 0 ? kMasserRadius : kSecundaRadius };
		visibilityData.Lights[i + 1] = { uv.x, uv.y, 1.0f, 0.0f };
	}
}

void CameraFX::UpdateFrame(float a_deltaTime, bool a_hasDepth)
{
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float step = paused ? 0.0f : a_deltaTime;
	time = std::fmod(time + step, 3600.0f);

	if (settings.ReplaceModifiers)
		CameraFXModifiers::Snapshot(modifierFrame);
	else
		modifierFrame.captures.clear();
	resolved = CameraFXModifiers::Resolve(modifierFrame, GetEnabledThemes(), GetModifierBlend());
	auto& lowHealthPresence = resolved.themes[static_cast<size_t>(Theme::kLowHealth)];
	lowHealthPresence = UpdateLowHealth(lowHealthPresence, step);

	auto theme = [&](Theme a_theme, bool a_enabled, float a_strength) {
		const auto index = static_cast<size_t>(a_theme);
		const float target = a_enabled ? resolved.themes[index] * a_strength : 0.0f;
		themeLevels[index] = Util::Approach(themeLevels[index], target, kThemeRiseRate, kThemeFallRate, step);
		return themeLevels[index];
	};
	const float coldTheme = theme(Theme::kCold, settings.ColdFrost, 1.0f);

	CameraFXLens::WeatherOptions weatherOptions;
	weatherOptions.rain = settings.RainDrops;
	weatherOptions.rainStrength = settings.RainStrength;
	weatherOptions.waterSheet = settings.WaterSheet;
	weatherOptions.weatherFrost = settings.WeatherFrost;
	weatherOptions.weatherFrostStrength = settings.WeatherFrostStrength;
	weatherOptions.coldBreath = settings.ColdBreath;
	weatherOptions.entryFog = settings.EntryFog;
	weatherOptions.condensation = settings.FogCondensation;
	weatherOptions.atmosphere = settings.Atmosphere;
	weatherOptions.atmosphereStrength = settings.AtmosphereStrength;
	weatherOptions.coldTheme = coldTheme;
	weather.Update(step, a_deltaTime, weatherOptions);

	CameraFXLens::CombatOptions combatOptions;
	combatOptions.blood = settings.Blood;
	combatOptions.bloodStrength = settings.BloodStrength;
	combatOptions.elemental = settings.ElementalSplats;
	combatOptions.flash = settings.ImpactFlash;
	combatOptions.flashStrength = settings.FlashStrength;
	combatOptions.slash = settings.SlashStreaks;
	combatOptions.shake = settings.HitShake;
	combatOptions.shakeStrength = settings.HitShakeStrength;
	combatOptions.hitBlur = settings.HitBlur;
	combatOptions.hitBlurStrength = settings.HitBlurStrength;
	combatOptions.stamina = settings.StaminaBreath;
	combatOptions.photosensitiveSafe = settings.PhotosensitiveSafe;
	combat.Update(step, a_deltaTime, settings.Combat, combatOptions);

	const auto& weatherState = weather.Get();
	const auto& combatState = combat.Get();
	const float frostTarget = std::max({ coldTheme * settings.FrostStrength, weatherState.frost, combatState.frost }) * (1.0f - weatherState.heat);
	const float meltRate = kFrostMeltRate * (1.0f + 3.0f * weatherState.heat);
	frost = frostTarget > frost ? std::min(frostTarget, frost + step * kFrostGrowRate) : std::max(frostTarget, frost - step * meltRate);

	frameData.Themes = { theme(Theme::kLowHealth, settings.LowHealth, settings.LowHealthStrength),
		theme(Theme::kExhaustion, settings.Exhaustion, settings.ExhaustionStrength),
		theme(Theme::kHunger, settings.Hunger, settings.HungerStrength),
		theme(Theme::kPoison, settings.Poison, settings.PoisonStrength) };

	const float werewolf = resolved.themes[static_cast<size_t>(Theme::kWerewolf)];
	const float vampireLord = resolved.themes[static_cast<size_t>(Theme::kVampireLord)];
	const float beastPresence = std::min(std::max(werewolf, vampireLord), 1.0f);
	if (beastPresence > kActiveEpsilon) {
		if (beastAbsentSeconds > kBeastRestartGap)
			beastAge = 0.0f;
		beastAbsentSeconds = 0.0f;
		beastVampire = vampireLord > werewolf;
	} else {
		beastAbsentSeconds = std::min(beastAbsentSeconds + step, 99.0f);
	}
	beastAge = std::min(beastAge + step, 99.0f);
	const float beastEnvelope = Util::Smoothstep(0.0f, 0.12f, beastAge) * (0.3f + 0.7f * std::exp(-beastAge * 0.8f)) * (1.0f - Util::Smoothstep(4.0f, 6.0f, beastAge));
	const float beastTarget = settings.Transformation ? beastPresence * beastEnvelope * settings.TransformationStrength : 0.0f;
	beastLevel = Util::Approach(beastLevel, beastTarget, kBeastRiseRate, kBeastFallRate, step);
	UpdateThemeClocks(a_deltaTime);

	auto* sky = globals::game::sky;
	float sunTarget = 0.0f;
	std::array<float, 2> moonTargets{};
	if (sky && sky->mode.get() == RE::Sky::Mode::kFull && !Util::IsInterior()) {
		const auto& skySync = globals::features::skySync;
		const float clearSky = BlendWeathers(sky, GetClearSky);
		const auto sunDirection = skySync.GetCelestialDirection(sky, SkySync::Caster::Sun);
		if (settings.SunLens) {
			const float height = sunDirection.z;
			const float low = Util::Smoothstep(-0.03f, 0.06f, height) * (1.0f - Util::Smoothstep(0.15f, std::max(settings.SunMaxHeight, kMinSunMaxHeight), height));
			const float glare = BlendWeathers(sky, GetSunGlare);
			sunTarget = ProceduralSun::GetSunVisibility() * clearSky * std::lerp(0.5f, 1.0f, glare) * low * settings.SunLensStrength;
		}
		if (settings.MoonLens) {
			const float night = Util::Smoothstep(0.0f, 0.1f, -sunDirection.z);
			const auto& syncSettings = skySync.settings;
			const RE::Moon* moons[2] = { sky->masser, sky->secunda };
			for (size_t i = 0; i < 2; ++i) {
				if (!moons[i])
					continue;
				const auto& baseColor = i == 0 ? Util::Moon::MasserBaseColor : Util::Moon::SecundaBaseColor;
				const float phase = Util::Moon::GetLightColor(moons[i], baseColor, syncSettings.NewMoonIntensity, syncSettings.CrescentMoonIntensity, syncSettings.FullMoonIntensity).GetColor().w;
				const float fullness = Util::Smoothstep(0.6f, 1.0f, phase / std::max(syncSettings.FullMoonIntensity, 1e-3f));
				moonTargets[i] = GetMoonDiscVisibility(moons[i]) * fullness * night * clearSky * settings.MoonLensStrength * (i == 0 ? 1.0f : kSecundaLensScale);
			}
		}
	}
	sunAmount = Ease(sunAmount, sunTarget, kLightEaseRate, step > 0.0f ? step : a_deltaTime);
	for (size_t i = 0; i < 2; ++i)
		moonAmount[i] = Ease(moonAmount[i], moonTargets[i], kLightEaseRate, step > 0.0f ? step : a_deltaTime);
	UpdateLights(sky);

	const auto& generic = resolved.generic;
	frameData.Grade = { generic.cinematic.x, generic.cinematic.y, generic.cinematic.z, time };
	frameData.TintColor = generic.tint;
	frameData.FadeColor = generic.fade;
	frameData.Radial = { generic.radialStrength, generic.radialStart, generic.radialRampUp, generic.radialDownStart };
	frameData.Radial2 = { generic.radialRampDown, generic.radialCenter.x, generic.radialCenter.y, 0.0f };
	frameData.DepthOfField = { generic.dofDistance, generic.dofRange, generic.dofMode, 0.0f };
	frameData.Lens = { frost, sunAmount, std::max(moonAmount[0], moonAmount[1]), settings.LensDirt };
	frameData.CameraData = Util::GetCameraData();

	const bool lensClear = !weatherState.underwater;
	const float breathVisible = settings.ColdBreath && combatState.firstPerson && lensClear ? Util::Smoothstep(0.15f, 0.5f, weatherState.breathCold) : 0.0f;
	const float extraDrops = lensClear ? std::max(weatherState.sheet * kSwimDropsFromSheet, weatherState.splash) : 0.0f;
	const float thaw = lensClear ? std::clamp(weatherState.heat * 1.5f, 0.0f, 1.0f) * Util::Smoothstep(kThawStart, 0.3f, frost) : 0.0f;
	frameData.Weather = { lensClear ? weatherState.rain : 0.0f, weatherState.lookUp, lensClear ? weatherState.sheet : 0.0f, weatherState.entryFog };
	frameData.Weather2 = { lensClear ? weatherState.condensation : 0.0f, lensClear ? weatherState.ash : 0.0f, lensClear ? weatherState.dust : 0.0f, lensClear ? weatherState.embers : 0.0f };
	frameData.Weather3 = { extraDrops, breathVisible, combatState.exhaleAge, combatState.exhaleStrength };
	windOffset = std::fmod(windOffset + weatherState.wind.x * step, 1000.0f);
	frameData.Wind = { weatherState.wind.x, windOffset, weatherState.windSpeed, combatState.firstPerson ? 1.0f : 0.0f };
	frameData.Combat = { combatState.flash, combatState.shock, combatState.fire, combatState.staminaLow };
	frameData.Combat3 = { combatState.hitU, combatState.hitV, combatState.hitAge, settings.PhotosensitiveSafe ? 1.0f : 0.0f };
	frameData.Combat4 = { combatState.shockAge, combatState.shockSeed, combatState.shockLasting, combatState.exhaleSeed };
	frameData.LensLook.x = thaw;
	frameData.HitBlur = { combatState.hitBlur, combatState.hitBlurU, combatState.hitBlurV, 0.0f };
	frameData.Blurs = { settings.BloodBlur ? settings.BloodBlurStrength : 0.0f, settings.HeartbeatBlur ? 1.0f : 0.0f, 0.0f, 0.0f };

	const uint32_t splatCount = static_cast<uint32_t>(std::min<size_t>(combatState.splats.size(), CameraFXLens::kMaxSplats));
	for (uint32_t i = 0; i < splatCount; ++i) {
		const auto& splat = combatState.splats[i];
		const float life = std::max(splat.life, 0.01f);
		const float fadeIn = std::clamp(splat.age / 0.08f, 0.0f, 1.0f);
		const float fadeOut = 1.0f - Util::Smoothstep(0.7f * life, life, splat.age);
		frameData.Splats[i * 2] = { splat.u, splat.v, splat.radius, std::clamp(splat.strength, 0.0f, 1.0f) * fadeIn * fadeOut };
		frameData.Splats[i * 2 + 1] = { static_cast<float>(splat.type), splat.seed, std::clamp(splat.age / life, 0.0f, 1.0f), splat.angle };
	}
	frameData.Combat2 = { combatState.slash, combatState.slashAngle, combatState.flashWarmth, static_cast<float>(splatCount) };
	const bool breathing = breathVisible > kActiveEpsilon && combatState.exhaleAge < 2.8f;
	const bool staminaPulse = combatState.staminaLow > kActiveEpsilon && combatState.exhaleAge < 1.0f;

	const float themesTotal = frameData.Themes.x + frameData.Themes.y + frameData.Themes.z + frameData.Themes.w;
	const bool hitBlurring = combatState.hitBlur > kActiveEpsilon;
	const bool transforming = beastLevel > kActiveEpsilon;
	sceneActive = generic.active || themesTotal > kActiveEpsilon || hitBlurring || transforming;
	lightsActive = a_hasDepth && ((sunAmount > kActiveEpsilon && frameData.SunScreen.z > 0.0f) || frameData.MoonScreen[0].z > kActiveEpsilon || frameData.MoonScreen[1].z > kActiveEpsilon);
	if (!lightsActive) {
		frameData.Lens.y = 0.0f;
		frameData.Lens.z = 0.0f;
		frameData.SunScreen.z = 0.0f;
		frameData.MoonScreen[0].z = 0.0f;
		frameData.MoonScreen[1].z = 0.0f;
	}

	const float weatherOverlay = frost + frameData.Weather.x + frameData.Weather.z + frameData.Weather.w + frameData.Weather2.x + frameData.Weather2.y +
	                             frameData.Weather2.z + extraDrops + thaw + combatState.slash + combatState.flash + combatState.fire;
	weatherPass.overlay = weatherOverlay > kActiveEpsilon || breathing || staminaPulse;
	weatherPass.additive = lightsActive || frameData.Weather2.w + frameData.Weather2.x + combatState.flash + combatState.fire > kActiveEpsilon;
	const bool discharging = combatState.shock > kActiveEpsilon && combatState.shockAge < 1.0f;
	combatPass.overlay = splatCount > 0 || discharging || combatState.shockLasting > kActiveEpsilon;
	combatPass.additive = combatPass.overlay;
	lensActive = weatherPass.overlay || weatherPass.additive || combatPass.overlay;
	sceneNeedsMips = generic.blurRadius > kMinBlurRadius || generic.dofStrength > kActiveEpsilon || frameData.Themes.y > kActiveEpsilon || hitBlurring ||
	                 transforming || (settings.HeartbeatBlur && frameData.Themes.x > kActiveEpsilon);
}

float CameraFX::GetSceneEncoding(bool a_hdrTarget) const
{
	if (!a_hdrTarget)
		return 0.0f;
	auto state = globals::state;
	if (state && state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing && !state->IsMainOrLoadingMenuOpen())
		return 2.0f;
	const bool linear = globals::features::linearLighting.IsLinearLightingActive() && !globals::features::effects11.ReplacedTonemapperThisFrame() &&
	                    !(state && state->IsFlatWorldMapOpen());
	return linear ? 1.0f : 0.0f;
}

void CameraFX::DrawBeforeInterface()
{
	if (!loaded)
		return;
	if (!settings.Enabled) {
		combat.Disable();
		return;
	}

	auto state = globals::state;
	auto context = globals::d3d::context;
	if (!state || !context || !globals::game::renderer || state->IsMainOrLoadingMenuOpen() || state->IsMenuSceneOpen())
		return;
	if (lastDrawnFrame == state->frameCount)
		return;
	lastDrawnFrame = state->frameCount;

	if (!pipelineReady && !EnsurePipeline()) {
		combat.Disable();
		return;
	}

	const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	const auto target = GetSceneTarget();
	sceneTargetAvailable = target.texture && target.rtv;
	if (!sceneTargetAvailable) {
		combat.Disable();
		return;
	}

	D3D11_TEXTURE2D_DESC targetDesc{};
	target.texture->GetDesc(&targetDesc);
	frameData.Resolution = { static_cast<float>(targetDesc.Width), static_cast<float>(targetDesc.Height), 1.0f / targetDesc.Width, 1.0f / targetDesc.Height };

	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	UpdateFrame(deltaTime, depthSRV != nullptr);

	if (!lightsActive && !visibilityCleared && visibility) {
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(visibility->uav.get(), zero);
		visibilityCleared = true;
	}

	if (!sceneActive && !lensActive) {
		idleSeconds += deltaTime;
		if (idleSeconds > kSceneCopyIdleSeconds) {
			sceneCopy.reset();
			frostMask.reset();
		}
		return;
	}
	idleSeconds = 0.0f;

	bool uiLayer = false;
	auto* lensTarget = lensActive ? GetLensTarget(target, uiLayer) : nullptr;
	const float encoding = GetSceneEncoding(target.hdr);
	auto& hdr = globals::features::hdrDisplay;
	const bool pqUILayer = uiLayer && hdr.loaded && hdr.settings.enableHDR;
	const float uiGain = pqUILayer ? std::pow(std::max(hdr.settings.hdrUIBrightness, 0.01f), 1.0f / 2.2f) : 1.0f;

	struct PassDraw
	{
		ID3D11PixelShader* shader = nullptr;
		bool overlay = false;
		bool additive = false;
	};
	auto plan = [&](const LensPass& a_pass, ID3D11PixelShader* a_shader) {
		PassDraw draw{ a_shader };
		if (!lensTarget || !a_shader)
			return draw;
		draw.overlay = pqUILayer ? a_pass.overlay : (a_pass.overlay || a_pass.additive);
		draw.additive = pqUILayer && a_pass.additive;
		return draw;
	};
	const PassDraw weatherDraw = plan(weatherPass, lensPixelShader.get());
	const PassDraw combatDraw = plan(combatPass, GetCombatLensPS());
	const bool lensDraws = weatherDraw.overlay || weatherDraw.additive || combatDraw.overlay || combatDraw.additive;
	const bool needsCopy = sceneActive || lensDraws;

	if (needsCopy && !EnsureSceneCopy(targetDesc))
		return;

	StateBackup backup(context);
	auto profiler = globals::profiler;
	profiler->BeginPass("CameraFX");

	if (lensDraws)
		EnsureLensTextures(frost > kActiveEpsilon, deltaTime);

	const auto& generic = resolved.generic;
	const float heightScale = frameData.Resolution.y / kReferenceHeight;
	const float lastMip = sceneCopy ? static_cast<float>(sceneCopy->desc.MipLevels - 1) : 0.0f;
	const float blurRadius = generic.blurRadius > kMinBlurRadius ? generic.blurRadius * kBlurPixelsPerUnit * heightScale : 0.0f;
	frameData.Blur = { blurRadius, generic.doubleVision * kDoubleVisionUVPerUnit, generic.dofStrength, lastMip };

	float2 depthScale{ 1.0f, 1.0f };
	float2 depthSize{ 1.0f, 1.0f };
	if (depthSRV) {
		winrt::com_ptr<ID3D11Resource> depthResource;
		depthSRV->GetResource(depthResource.put());
		winrt::com_ptr<ID3D11Texture2D> depthTexture;
		D3D11_TEXTURE2D_DESC depthDesc{};
		if (depthResource && SUCCEEDED(depthResource->QueryInterface(IID_PPV_ARGS(depthTexture.put())))) {
			depthTexture->GetDesc(&depthDesc);
			depthSize = { static_cast<float>(depthDesc.Width), static_cast<float>(depthDesc.Height) };
			auto& upscaling = globals::features::upscaling;
			const bool fullSize = depthDesc.Width + 1 >= targetDesc.Width;
			if (fullSize)
				depthScale = upscaling.loaded ? upscaling.resolutionScale : Util::ConvertToDynamic(float2{ 1.0f, 1.0f }, true);
		}
	}
	const float reverseZ = globals::features::reverseZ.IsActive() ? 1.0f : 0.0f;
	frameData.DepthParams = { depthSize.x * depthScale.x, depthSize.y * depthScale.y, reverseZ, depthSRV ? 1.0f : 0.0f };
	const float overlayMode = pqUILayer ? kLensModeUIOverlay : (uiLayer ? kLensModeUI : kLensModeScene);
	frameData.Output = { overlayMode, kContrastPivot, encoding, uiGain };
	frameConstants->Update(frameData);

	auto copyScene = [&](bool a_mips) {
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->CopySubresourceRegion(sceneCopy->resource.get(), 0, 0, 0, 0, target.texture, 0, nullptr);
		if (a_mips && sceneCopy->desc.MipLevels > 1)
			context->GenerateMips(sceneCopy->srv.get());
	};

	if (needsCopy)
		copyScene(sceneActive ? sceneNeedsMips : true);
	else
		context->OMSetRenderTargets(0, nullptr, nullptr);

	if (lightsActive && depthSRV && visibility && visibilityConstants) {
		if (auto cs = GetVisibilityCS()) {
			const float alpha = 1.0f - std::exp(-kVisibilityRate * deltaTime);
			for (auto& light : visibilityData.Lights)
				light.w = alpha;
			visibilityData.DepthParams = { depthSize.x * depthScale.x, depthSize.y * depthScale.y, reverseZ, 1.0f - std::exp(-kVisibilityFallRate * deltaTime) };
			visibilityData.DepthSize = { depthSize.x, depthSize.y, 0.0f, 0.0f };
			visibilityConstants->Update(visibilityData);

			ID3D11Buffer* buffer = visibilityConstants->CB();
			ID3D11UnorderedAccessView* uav = visibility->uav.get();
			context->CSSetShader(cs, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &buffer);
			context->CSSetShaderResources(0, 1, &depthSRV);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->Dispatch(1, 1, 1);

			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			visibilityCleared = false;
		}
	}

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, frameData.Resolution.x, frameData.Resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.get());
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vertexShader.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11SamplerState* samplers[kLensSamplerCount] = { linearSampler.get(), wrapSampler.get() };
	context->PSSetConstantBuffers(0, 1, &frameBuffer);
	context->PSSetSamplers(0, kLensSamplerCount, samplers);

	ID3D11ShaderResourceView* sceneSRV = sceneCopy && needsCopy ? sceneCopy->srv.get() : nullptr;
	if (sceneActive) {
		ID3D11RenderTargetView* renderTarget = target.rtv;
		context->OMSetRenderTargets(1, &renderTarget, nullptr);
		context->OMSetBlendState(opaqueBlend.get(), nullptr, 0xFFFFFFFF);
		ID3D11ShaderResourceView* resources[2] = { sceneSRV, depthSRV };
		context->PSSetShaderResources(0, 2, resources);
		context->PSSetShader(scenePixelShader.get(), nullptr, 0);
		context->Draw(3, 0);
	}

	if (lensDraws) {
		if (sceneActive) {
			ID3D11ShaderResourceView* nullResources[2] = {};
			context->PSSetShaderResources(0, 2, nullResources);
			copyScene(true);
		}
		ID3D11ShaderResourceView* resources[kLensResourceCount] = { sceneSRV, depthSRV, visibility ? visibility->srv.get() : nullptr,
			lensDirt ? lensDirt->srv.get() : nullptr, frostMask ? frostMask->srv.get() : nullptr };
		context->PSSetShaderResources(0, kLensResourceCount, resources);

		float currentMode = overlayMode;
		auto setMode = [&](float a_mode) {
			if (currentMode != a_mode) {
				currentMode = a_mode;
				frameData.Output.x = a_mode;
				frameConstants->Update(frameData);
			}
		};
		auto drawLens = [&](const PassDraw& a_draw) {
			if (!a_draw.overlay && !a_draw.additive)
				return;
			context->PSSetShader(a_draw.shader, nullptr, 0);
			if (a_draw.overlay) {
				setMode(overlayMode);
				context->OMSetRenderTargets(1, &lensTarget, nullptr);
				context->OMSetBlendState(uiLayer ? lensUIBlend.get() : lensSceneBlend.get(), nullptr, 0xFFFFFFFF);
				context->Draw(3, 0);
			}
			if (a_draw.additive) {
				setMode(kLensModeSceneAdditive);
				ID3D11RenderTargetView* renderTarget = target.rtv;
				context->OMSetRenderTargets(1, &renderTarget, nullptr);
				context->OMSetBlendState(lensSceneBlend.get(), nullptr, 0xFFFFFFFF);
				context->Draw(3, 0);
			}
		};
		drawLens(weatherDraw);
		drawLens(combatDraw);
	}

	ID3D11ShaderResourceView* nullResources[kLensResourceCount] = {};
	context->PSSetShaderResources(0, kLensResourceCount, nullResources);
	profiler->EndPass();
}

void CameraFX::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	auto featureDisabled = Util::DisableGuard(!settings.Enabled);

	ImGui::Checkbox(T(TKEY("replace_modifiers"), "Replace Imagespace Modifiers"), &settings.ReplaceModifiers);
	Util::HoverTip(T(TKEY("replace_modifiers_tooltip"), "Draws every game screen effect (damage, fades, spells, survival and more) itself, after upscaling and before the HUD. Off leaves them to the game."));
	ImGui::Checkbox(T(TKEY("photosensitive_safe"), "Photosensitive Safe"), &settings.PhotosensitiveSafe);
	Util::HoverTip(T(TKEY("photosensitive_safe_tooltip"), "Caps and spaces out flashes, keeps flicker slow and calms the low health heartbeat."));

	if (ImGui::TreeNodeEx(T(TKEY("themes"), "Themed Effects"), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (!settings.ReplaceModifiers)
			Util::SettingHint(T(TKEY("themes_need_replace"), "Turn on Replace Imagespace Modifiers to use these."), false);
		{
			auto themesDisabled = Util::DisableGuard(!settings.ReplaceModifiers);

			ImGui::Checkbox(T(TKEY("cold_frost"), "Cold: Lens Frost"), &settings.ColdFrost);
			Util::HoverTip(T(TKEY("cold_frost_tooltip"), "Survival Mode cold grows frost in from the lens edges instead of the blue blur."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.ColdFrost);
				Util::PercentageSlider(T(TKEY("frost_strength"), "Frost Strength"), &settings.FrostStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("low_health"), "Low Health: Heartbeat"), &settings.LowHealth);
			Util::HoverTip(T(TKEY("low_health_tooltip"), "Low health darkens and reddens the edges in time with a heartbeat that quickens as health drops."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.LowHealth);
				Util::PercentageSlider(T(TKEY("low_health_strength"), "Heartbeat Strength"), &settings.LowHealthStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
				ImGui::Checkbox(T(TKEY("heartbeat_blur"), "Heartbeat Blur"), &settings.HeartbeatBlur);
				Util::HoverTip(T(TKEY("heartbeat_blur_tooltip"), "The edges of the view soften with each heartbeat while health is low."));
			}

			ImGui::Checkbox(T(TKEY("exhaustion"), "Exhaustion: Heavy Eyes"), &settings.Exhaustion);
			Util::HoverTip(T(TKEY("exhaustion_tooltip"), "Survival Mode exhaustion makes the eyelids droop and blink slowly."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.Exhaustion);
				Util::PercentageSlider(T(TKEY("exhaustion_strength"), "Exhaustion Strength"), &settings.ExhaustionStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("hunger"), "Hunger: Fading Colour"), &settings.Hunger);
			Util::HoverTip(T(TKEY("hunger_tooltip"), "Survival Mode hunger drains colour from the view in slow waves."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.Hunger);
				Util::PercentageSlider(T(TKEY("hunger_strength"), "Hunger Strength"), &settings.HungerStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("poison"), "Poison: Sickly Edges"), &settings.Poison);
			Util::HoverTip(T(TKEY("poison_tooltip"), "Poison tints the edges green and makes the view sway."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.Poison);
				Util::PercentageSlider(T(TKEY("poison_strength"), "Poison Strength"), &settings.PoisonStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("transformation"), "Beast Transformation"), &settings.Transformation);
			Util::HoverTip(T(TKEY("transformation_tooltip"), "Turning into a werewolf or Vampire Lord floods the edges with a pounding, blurred red (or violet) haze on top of the game's own effect."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.Transformation);
				Util::PercentageSlider(T(TKEY("transformation_strength"), "Transformation Strength"), &settings.TransformationStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("lens"), "Sun and Moon on the Lens"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("sun_lens"), "Sun Glare"), &settings.SunLens);
		Util::HoverTip(T(TKEY("sun_lens_tooltip"), "A low sun in a clear sky veils the lens and lights its dust. Hidden as soon as anything stands between you and the sun."));
		{
			IndentScope indent;
			auto disabled = Util::DisableGuard(!settings.SunLens);
			Util::PercentageSlider(T(TKEY("sun_lens_strength"), "Glare Strength"), &settings.SunLensStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("sun_max_height"), "Sun Height Limit"), &settings.SunMaxHeight, kMinSunMaxHeight, kMaxSunMaxHeight, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			Util::HoverTip(T(TKEY("sun_max_height_tooltip"), "Sun elevation (as a sine) at which the glare has faded out. Lower keeps it to sunrise and sunset."));
		}

		ImGui::Checkbox(T(TKEY("moon_lens"), "Moon Glow"), &settings.MoonLens);
		Util::HoverTip(T(TKEY("moon_lens_tooltip"), "A full or nearly full moon on a clear night lights the lens gently."));
		{
			IndentScope indent;
			auto disabled = Util::DisableGuard(!settings.MoonLens);
			Util::PercentageSlider(T(TKEY("moon_lens_strength"), "Glow Strength"), &settings.MoonLensStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		}

		{
			auto disabled = Util::DisableGuard(!settings.SunLens && !settings.MoonLens);
			Util::PercentageSlider(T(TKEY("lens_dirt"), "Lens Dirt"), &settings.LensDirt, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			Util::HoverTip(T(TKEY("lens_dirt_tooltip"), "Dust and smudges on the lens that only show against the sun or moon."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("weather"), "Weather on the Lens"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("rain_drops"), "Rain Drops"), &settings.RainDrops);
		Util::HoverTip(T(TKEY("rain_drops_tooltip"), "Rain beads and runs on the lens while you are out in it. More lands when you look up; none under a roof."));
		{
			IndentScope indent;
			auto disabled = Util::DisableGuard(!settings.RainDrops);
			Util::PercentageSlider(T(TKEY("rain_strength"), "Rain Strength"), &settings.RainStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		}

		ImGui::Checkbox(T(TKEY("weather_frost"), "Snow and Blizzard Frost"), &settings.WeatherFrost);
		Util::HoverTip(T(TKEY("weather_frost_tooltip"), "Snowfall frosts the lens edges lightly, blizzards heavily. Melts near fires while Heat Haze is on."));
		{
			IndentScope indent;
			auto disabled = Util::DisableGuard(!settings.WeatherFrost);
			Util::PercentageSlider(T(TKEY("weather_frost_strength"), "Snow Frost Strength"), &settings.WeatherFrostStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		}

		ImGui::Checkbox(T(TKEY("atmosphere"), "Ash, Dust and Embers"), &settings.Atmosphere);
		Util::HoverTip(T(TKEY("atmosphere_tooltip"), "Ash storms settle grey ash on the lens, dry gales blow dust past it and burning skies carry embers."));
		{
			IndentScope indent;
			auto disabled = Util::DisableGuard(!settings.Atmosphere);
			Util::PercentageSlider(T(TKEY("atmosphere_strength"), "Ash and Dust Strength"), &settings.AtmosphereStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
		}

		ImGui::Checkbox(T(TKEY("water_sheet"), "Water After Swimming"), &settings.WaterSheet);
		Util::HoverTip(T(TKEY("water_sheet_tooltip"), "Swimming splashes the lens; surfacing or climbing out leaves a sheet of water that drains off it in drops and runs."));
		ImGui::Checkbox(T(TKEY("cold_breath"), "Cold Breath"), &settings.ColdBreath);
		Util::HoverTip(T(TKEY("cold_breath_tooltip"), "In first person your breath shows as vapour in the cold, faster when you are out of breath."));
		ImGui::Checkbox(T(TKEY("entry_fog"), "Fogged Lens Indoors"), &settings.EntryFog);
		Util::HoverTip(T(TKEY("entry_fog_tooltip"), "Stepping into a warm interior from the cold fogs the lens, which clears from the centre."));
		ImGui::Checkbox(T(TKEY("fog_condensation"), "Fog Condensation"), &settings.FogCondensation);
		Util::HoverTip(T(TKEY("fog_condensation_tooltip"), "Dense fog beads fine droplets on the lens edges."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("combat"), "Combat"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("combat_enabled"), "Combat Effects"), &settings.Combat);
		Util::HoverTip(T(TKEY("combat_enabled_tooltip"), "Lens and camera effects from fighting."));
		{
			IndentScope combatIndent;
			auto combatDisabled = Util::DisableGuard(!settings.Combat);

			ImGui::Checkbox(T(TKEY("blood"), "Blood Splatter"), &settings.Blood);
			Util::HoverTip(T(TKEY("blood_tooltip"), "Close hits spray blood (or oil from machines) onto the lens. Ghosts do not bleed."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.Blood);
				Util::PercentageSlider(T(TKEY("blood_strength"), "Blood Strength"), &settings.BloodStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("blood_blur"), "Blood Blur"), &settings.BloodBlur);
			Util::HoverTip(T(TKEY("blood_blur_tooltip"), "Blood, oil and venom on the lens blur what is behind them, and blood and oil leave a soft smudge around the splash."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.BloodBlur);
				Util::PercentageSlider(T(TKEY("blood_blur_strength"), "Blood Blur Strength"), &settings.BloodBlurStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("impact_flash"), "Impact Flash"), &settings.ImpactFlash);
			Util::HoverTip(T(TKEY("impact_flash_tooltip"), "Heavy hits and spells flash the view briefly."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.ImpactFlash);
				Util::PercentageSlider(T(TKEY("flash_strength"), "Flash Strength"), &settings.FlashStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("hit_shake"), "Hit Shake"), &settings.HitShake);
			Util::HoverTip(T(TKEY("hit_shake_tooltip"), "The camera jolts when you are hit, harder from power attacks and big creatures. Uses the game camera, so frame generation stays correct."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.HitShake);
				Util::PercentageSlider(T(TKEY("hit_shake_strength"), "Shake Strength"), &settings.HitShakeStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("hit_blur"), "Hit Blur"), &settings.HitBlur);
			Util::HoverTip(T(TKEY("hit_blur_tooltip"), "Taking a hit briefly smears the view outward from the attacker, harder from power attacks, bashes and big creatures. Blocked hits blur less."));
			{
				IndentScope indent;
				auto disabled = Util::DisableGuard(!settings.HitBlur);
				Util::PercentageSlider(T(TKEY("hit_blur_strength"), "Hit Blur Strength"), &settings.HitBlurStrength, 0.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::Checkbox(T(TKEY("elemental"), "Elemental Hits"), &settings.ElementalSplats);
			Util::HoverTip(T(TKEY("elemental_tooltip"), "Fire scorches, frost freezes, shock crackles and poison spatters the lens; giants and dragons throw up grit."));
			ImGui::Checkbox(T(TKEY("slash"), "Slash Streaks"), &settings.SlashStreaks);
			Util::HoverTip(T(TKEY("slash_tooltip"), "Melee hits you take leave a streak across the view from the side they came from."));
			ImGui::Checkbox(T(TKEY("stamina_breath"), "Stamina Breaths"), &settings.StaminaBreath);
			Util::HoverTip(T(TKEY("stamina_breath_tooltip"), "Low stamina darkens the edges in time with your breathing."));
		}
		ImGui::TreePop();
	}

	if (Util::ShowDebugSections() && ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"), ImGuiTreeNodeFlags_None)) {
		ImGui::Text(T(TKEY("debug_modifiers"), "Modifiers: %zu"), modifierFrame.captures.size());
		if (!modifierFrame.captures.empty() && ImGui::BeginTable("##CameraFXModifiers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T(TKEY("debug_form_id"), "FormID"));
			ImGui::TableSetupColumn(T(TKEY("debug_theme"), "Theme"));
			ImGui::TableSetupColumn(T(TKEY("strength"), "Strength"));
			ImGui::TableSetupColumn(T(TKEY("debug_activity"), "Activity"));
			ImGui::TableHeadersRow();
			for (const auto& capture : modifierFrame.captures) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%08X", capture.formID);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(CameraFXModifiers::GetThemeName(capture.theme));
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", capture.strength);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", CameraFXModifiers::GetActivity(capture));
			}
			ImGui::EndTable();
		}
		const auto& generic = resolved.generic;
		ImGui::TextWrapped(T(TKEY("debug_grade"), "Grade: sat %.2f bright %.2f contrast %.2f"), generic.cinematic.x, generic.cinematic.y, generic.cinematic.z);
		ImGui::TextWrapped(T(TKEY("debug_tint_fade"), "Tint %.2f  Fade %.2f  Blur %.2f  Double vision %.2f"), generic.tint.w, generic.fade.w, generic.blurRadius, generic.doubleVision);
		ImGui::TextWrapped(T(TKEY("debug_radial"), "Radial %.2f  DoF %.2f"), generic.radialStrength, generic.dofStrength);
		ImGui::TextWrapped(T(TKEY("debug_lights"), "Frost %.2f  Sun %.2f  Masser %.2f  Secunda %.2f"), frost, sunAmount, moonAmount[0], moonAmount[1]);
		ImGui::TextWrapped(T(TKEY("debug_themes"), "Low health %.2f  Exhaustion %.2f  Hunger %.2f  Poison %.2f"), frameData.Themes.x, frameData.Themes.y, frameData.Themes.z, frameData.Themes.w);
		ImGui::TextWrapped(T(TKEY("debug_heartbeat"), "Health severity %.2f  Heartbeat hold %.2f s"), lowHealthSeverity, lowHealthHoldSeconds);
		const auto& weatherState = weather.Get();
		const auto& combatState = combat.Get();
		ImGui::TextWrapped(T(TKEY("debug_weather"), "Rain %.2f  Roof exposure %.2f  Sheet %.2f  Splash %.2f  Entry fog %.2f  Condensation %.2f"), weatherState.rain, weatherState.exposure, weatherState.sheet, weatherState.splash, weatherState.entryFog, weatherState.condensation);
		ImGui::TextWrapped(T(TKEY("debug_water_state"), "Underwater %d  Swimming %d  Frost map %d  Dirt map %d"), weatherState.underwater ? 1 : 0, weatherState.swimming ? 1 : 0, frostMask ? 1 : 0, lensDirt ? 1 : 0);
		ImGui::TextWrapped(T(TKEY("debug_atmosphere"), "Ash %.2f  Dust %.2f  Embers %.2f  Heat %.2f  Breath cold %.2f"), weatherState.ash, weatherState.dust, weatherState.embers, weatherState.heat, weatherState.breathCold);
		ImGui::TextWrapped(T(TKEY("debug_combat"), "Splats %zu  Flash %.2f  Shock %.2f  Lasting shock %.2f  Fire %.2f  Exertion %.2f"), combatState.splats.size(), combatState.flash, combatState.shock, combatState.shockLasting, combatState.fire, combatState.exertion);
		ImGui::TextWrapped(T(TKEY("debug_blur"), "Hit blur %.2f  Transformation %.2f (%s, %.1f s)"), combatState.hitBlur, beastLevel,
			CameraFXModifiers::GetThemeName(beastVampire ? Theme::kVampireLord : Theme::kWerewolf), beastAge);
		ImGui::TreePop();
	}
}

void CameraFX::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = Util::ClampFinite(a_value, a_default, a_min, a_max);
	};
	sanitize(settings.FrostStrength, defaults.FrostStrength, 0.0f, kMaxStrength);
	sanitize(settings.LowHealthStrength, defaults.LowHealthStrength, 0.0f, kMaxStrength);
	sanitize(settings.ExhaustionStrength, defaults.ExhaustionStrength, 0.0f, kMaxStrength);
	sanitize(settings.HungerStrength, defaults.HungerStrength, 0.0f, kMaxStrength);
	sanitize(settings.PoisonStrength, defaults.PoisonStrength, 0.0f, kMaxStrength);
	sanitize(settings.TransformationStrength, defaults.TransformationStrength, 0.0f, kMaxStrength);
	sanitize(settings.SunLensStrength, defaults.SunLensStrength, 0.0f, kMaxStrength);
	sanitize(settings.LensDirt, defaults.LensDirt, 0.0f, 1.0f);
	sanitize(settings.SunMaxHeight, defaults.SunMaxHeight, kMinSunMaxHeight, kMaxSunMaxHeight);
	sanitize(settings.MoonLensStrength, defaults.MoonLensStrength, 0.0f, kMaxStrength);
	sanitize(settings.RainStrength, defaults.RainStrength, 0.0f, kMaxStrength);
	sanitize(settings.WeatherFrostStrength, defaults.WeatherFrostStrength, 0.0f, kMaxStrength);
	sanitize(settings.AtmosphereStrength, defaults.AtmosphereStrength, 0.0f, kMaxStrength);
	sanitize(settings.BloodStrength, defaults.BloodStrength, 0.0f, kMaxStrength);
	sanitize(settings.BloodBlurStrength, defaults.BloodBlurStrength, 0.0f, kMaxStrength);
	sanitize(settings.FlashStrength, defaults.FlashStrength, 0.0f, kMaxStrength);
	sanitize(settings.HitShakeStrength, defaults.HitShakeStrength, 0.0f, kMaxStrength);
	sanitize(settings.HitBlurStrength, defaults.HitBlurStrength, 0.0f, kMaxStrength);
}

void CameraFX::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CameraFX::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
