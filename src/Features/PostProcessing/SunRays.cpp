#include "SunRays.h"

#include "Features/CloudShadows.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Features/ProceduralSun.h"
#include "Features/ReverseZ.h"
#include "Features/SkySync.h"
#include "I18n/I18n.h"
#include "RasterPass.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/ColorSpace.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/Moon.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SunRays::Settings,
	EnableSunRays,
	EnableMoonRays,
	SunRaysMultiplier,
	MoonRaysMultiplier,
	SkyColorAmount,
	Quality,
	Type,
	MoonType,
	UseLinearMath)

namespace
{
	constexpr int kQualityLevels = 5;
	constexpr int kTypeCount = 5;
	constexpr float kDefaultSunBillboardTan = 425.0f / 400.0f;
	constexpr uint32_t kBlurSteps[3][kQualityLevels] = { { 4, 4, 5, 6, 7 }, { 4, 5, 6, 6, 7 }, { 4, 5, 6, 7, 7 } };
	constexpr uint32_t kCompositeSteps[kQualityLevels] = { 4, 6, 7, 8, 8 };
	constexpr float kShortBlur[kTypeCount] = { 0.15f, 0.3f, 0.6f, 0.3f, 0.9f };
	constexpr float kLongBlur[kTypeCount] = { 0.4f, 0.7f, 0.6f, 1.0f, 0.9f };

	float3 ToWorking(const float3& a_color)
	{
		auto& linearLighting = globals::features::linearLighting;
		if (linearLighting.IsLinearLightingActive()) {
			const auto working = linearLighting.SRGBToWorking(RE::NiColor{ a_color.x, a_color.y, a_color.z });
			return { working.red, working.green, working.blue };
		}
		auto decode = [](float a_value) { return std::pow(std::max(a_value, 0.0f), Util::ColorSpace::GAME_GAMMA); };
		return { decode(a_color.x), decode(a_color.y), decode(a_color.z) };
	}
}

void SunRays::DrawSettings()
{
	ImGui::Checkbox(T("feature.post_processing.sun_rays.enable_sun_rays", "Sun Rays"), &settings.EnableSunRays);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.sun_rays.enable_sun_rays_tooltip", "Rays from the sun, cut by clouds and anything standing in front of it."));
	ImGui::Checkbox(T("feature.post_processing.sun_rays.enable_moon_rays", "Moon Rays"), &settings.EnableMoonRays);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.sun_rays.enable_moon_rays_tooltip", "Faint rays from the moon that lights the night, following Sky Sync's moon light source."));

	ImGui::SliderFloat(T("feature.post_processing.sun_rays.sun_rays_multiplier", "Sun Rays Intensity"), &settings.SunRaysMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.sun_rays.moon_rays_multiplier", "Moon Rays Intensity"), &settings.MoonRaysMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.sun_rays.sky_color_amount", "Sky Colour Amount"), &settings.SkyColorAmount, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.sun_rays.sky_color_amount_tooltip", "Adds the horizon sky colour to the sun rays."));

	const char* qualityNames[] = {
		T("feature.post_processing.sun_rays.quality_low", "Low"),
		T("feature.post_processing.sun_rays.quality_medium", "Medium"),
		T("feature.post_processing.sun_rays.quality_high", "High"),
		T("feature.post_processing.sun_rays.quality_very_high", "Very High"),
		T("feature.post_processing.sun_rays.quality_extreme", "Extreme")
	};
	ImGui::Combo(T("feature.post_processing.sun_rays.quality", "Quality"), &settings.Quality, qualityNames, IM_ARRAYSIZE(qualityNames));

	const char* typeNames[] = {
		T("feature.post_processing.sun_rays.type_short", "Short"),
		T("feature.post_processing.sun_rays.type_medium", "Medium"),
		T("feature.post_processing.sun_rays.type_even", "Even"),
		T("feature.post_processing.sun_rays.type_long", "Long"),
		T("feature.post_processing.sun_rays.type_wide", "Wide")
	};
	ImGui::Combo(T("feature.post_processing.sun_rays.type", "Sun Ray Shape"), &settings.Type, typeNames, IM_ARRAYSIZE(typeNames));
	ImGui::Combo(T("feature.post_processing.sun_rays.moon_type", "Moon Ray Shape"), &settings.MoonType, typeNames, IM_ARRAYSIZE(typeNames));

	ImGui::Checkbox(T("feature.post_processing.sun_rays.use_linear_math", "Linear Colour"), &settings.UseLinearMath);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.sun_rays.use_linear_math_tooltip", "Colours the rays by the sun's hue alone, without its brightness, as Effects 11 does with UseLinearMath."));
}

void SunRays::RestoreDefaultSettings()
{
	settings = {};
}

void SunRays::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	settings.SunRaysMultiplier = Util::ClampFinite(settings.SunRaysMultiplier, defaults.SunRaysMultiplier, 0.0f, 100.0f);
	settings.MoonRaysMultiplier = Util::ClampFinite(settings.MoonRaysMultiplier, defaults.MoonRaysMultiplier, 0.0f, 100.0f);
	settings.SkyColorAmount = Util::ClampFinite(settings.SkyColorAmount, defaults.SkyColorAmount, 0.0f, 100.0f);
	settings.Quality = std::clamp(settings.Quality, 0, kQualityLevels - 1);
	settings.Type = std::clamp(settings.Type, 0, kTypeCount - 1);
	settings.MoonType = std::clamp(settings.MoonType, 0, kTypeCount - 1);
}

void SunRays::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SunRays::SetupResources()
{
	raysCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SunRaysCB>(), "SunRays::Constants");

	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
		.MaxAnisotropy = 1,
		.MinLOD = 0,
		.MaxLOD = D3D11_FLOAT32_MAX
	};
	linearSampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "SunRays::LinearSampler");

	CompileRasterShaders();
}

void SunRays::ClearShaderCache()
{
	BumpShaderGeneration();
	const auto shaderPtrs = std::array{ &maskPS, &blurPS, &compositePS };
	{
		std::lock_guard lock(shaderMutex);
		for (auto shader : shaderPtrs)
			if (*shader) {
				(*shader)->Release();
				shader->detach();
			}
	}
	globals::shaderCache->InvalidateShaderSources();
	CompileRasterShaders();
}

void SunRays::CompileRasterShaders()
{
	std::vector<std::pair<const char*, const char*>> maskDefines = { { "MASK", "" } };
	if (globals::features::cloudShadows.loaded)
		maskDefines.push_back({ "CLOUD_SHADOWS", "" });

	const std::vector<PixelShaderCompileInfo> shaderInfos = {
		{ &maskPS, "sunrays.ps.hlsl", maskDefines },
		{ &blurPS, "sunrays.ps.hlsl", { { "BLUR", "" } } },
		{ &compositePS, "sunrays.ps.hlsl", { { "COMPOSITE", "" } } },
	};

	CompileRasterShadersAsync(L"Data\\Shaders\\PostProcessing\\SunRays", {}, shaderInfos);
}

bool SunRays::SelectLight(RE::Sky* a_sky, Light& a_light) const
{
	const auto& skySync = globals::features::skySync;
	const auto& viewer = a_sky->root ? a_sky->root->world.translate : RE::NiPoint3{};

	if (settings.EnableSunRays && ProceduralSun::GetSunVisibility() > 0.0f && a_sky->sun && a_sky->sun->sunBase) {
		if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(a_sky->sun->sunBase->GetGeometryRuntimeData().shaderProperty.get())) {
			const auto& blend = prop->kBlendColor;
			a_light = { float3(blend.red, blend.green, blend.blue) * blend.alpha, skySync.GetCelestialDirection(a_sky, SkySync::Caster::Sun),
				Util::GetBillboardHalfTan(a_sky->sun->sunBase.get(), viewer, kDefaultSunBillboardTan), 32.0f, true };
			return true;
		}
	}

	if (!settings.EnableMoonRays)
		return false;

	const auto caster = skySync.GetMoonLightCaster(a_sky);
	if (caster == SkySync::Caster::Zenith)
		return false;

	const bool masser = caster == SkySync::Caster::Masser;
	auto* moon = masser ? a_sky->masser : a_sky->secunda;
	if (!moon || !moon->moonMesh)
		return false;
	const auto moonColor = Util::Moon::GetLightColor(moon, masser ? Util::Moon::MasserBaseColor : Util::Moon::SecundaBaseColor,
		skySync.settings.NewMoonIntensity, skySync.settings.CrescentMoonIntensity, skySync.settings.FullMoonIntensity)
	                           .GetColor();
	a_light = { float3(moonColor.x, moonColor.y, moonColor.z), skySync.GetCelestialDirection(a_sky, caster),
		Util::GetBillboardHalfTan(moon->moonMesh.get(), viewer, 0.0f), 4.0f, false };
	return true;
}

float3 SunRays::GetRaysColor(const Light& a_light, float a_lightPeak, RE::Sky* a_sky) const
{
	if (!a_light.isSun)
		return a_light.color * settings.MoonRaysMultiplier;

	const auto& horizon = a_sky->skyColor[RE::TESWeather::ColorTypes::kHorizon];
	const float3 skyColor{ horizon.red, horizon.green, horizon.blue };
	const float3 tint = (a_light.color + float3(1e-5f)) / a_lightPeak;
	const float multiplier = settings.SunRaysMultiplier;
	const float skyAmount = settings.SkyColorAmount * multiplier;
	if (settings.UseLinearMath)
		return tint * multiplier + skyColor * skyAmount;

	const float sunAmount = std::clamp((a_light.color.x + a_light.color.y + a_light.color.z) * 3.0f, 0.0f, 1.0f);
	return a_light.color * tint * (1.5f * multiplier) + skyColor * tint * (sunAmount * skyAmount * 0.5f);
}

bool SunRays::EnsureTargets(const D3D11_TEXTURE2D_DESC& a_sceneDesc)
{
	const UINT halfWidth = (a_sceneDesc.Width + 1) / 2;
	const UINT halfHeight = (a_sceneDesc.Height + 1) / 2;
	if (texOutput && texOutput->desc.Width == a_sceneDesc.Width && texOutput->desc.Height == a_sceneDesc.Height && texOutput->desc.Format == a_sceneDesc.Format &&
		texRaysA && texRaysA->desc.Width == halfWidth && texRaysA->desc.Height == halfHeight)
		return true;

	auto makeTarget = [](UINT a_width, UINT a_height, DXGI_FORMAT a_format, const char* a_name) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		auto texture = eastl::make_unique<Texture2D>(desc, a_name);
		texture->CreateSRV({ .Format = a_format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		texture->CreateRTV({ .Format = a_format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
		return texture;
	};

	try {
		texRaysA = makeTarget(halfWidth, halfHeight, DXGI_FORMAT_R16_FLOAT, "SunRays::RaysA");
		texRaysB = makeTarget(halfWidth, halfHeight, DXGI_FORMAT_R16_FLOAT, "SunRays::RaysB");
		texOutput = makeTarget(a_sceneDesc.Width, a_sceneDesc.Height, a_sceneDesc.Format, "SunRays::Output");
	} catch (const std::exception& e) {
		logger::error("[PostProcessing] Sun Rays could not create its targets: {}", e.what());
		texRaysA.reset();
		texRaysB.reset();
		texOutput.reset();
		return false;
	}
	return true;
}

void SunRays::Draw(TextureInfo& inout_tex)
{
	if (!owner || !owner->GetFullscreenVS() || !inout_tex.tex || !inout_tex.srv)
		return;
	if (!settings.EnableSunRays && !settings.EnableMoonRays)
		return;
	if (!AllShadersReady({ &maskPS, &blurPS, &compositePS }))
		return;
	if (Util::IsInterior() || globals::state->IsFullScreenMenuOpen() || globals::state->isMapMenuOpen)
		return;

	auto* sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || sky->flags.any(RE::Sky::Flags::kHideSky))
		return;

	Light light;
	if (!SelectLight(sky, light))
		return;
	const float lightPeak = std::max({ light.color.x, light.color.y, light.color.z });
	if (lightPeak <= 1e-4f || light.billboardTan <= 0.0f)
		return;

	const auto viewProj = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
	const auto clip = DirectX::SimpleMath::Vector4::Transform(DirectX::SimpleMath::Vector4(light.direction.x, light.direction.y, light.direction.z, 0.0f), viewProj);
	if (clip.w <= 1e-6f)
		return;
	const float2 lightNdc = { clip.x / clip.w, clip.y / clip.w };
	if (std::max(std::abs(lightNdc.x), std::abs(lightNdc.y)) * 0.4f >= 0.99999f)
		return;

	const float3 raysColor = ToWorking(GetRaysColor(light, lightPeak, sky));
	if (raysColor.x <= 0.0f && raysColor.y <= 0.0f && raysColor.z <= 0.0f)
		return;

	auto* depthSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	if (!depthSRV)
		return;

	D3D11_TEXTURE2D_DESC sceneDesc{};
	inout_tex.tex->GetDesc(&sceneDesc);
	if (!EnsureTargets(sceneDesc))
		return;

	globals::profiler->BeginPass("PostProcessing::SunRays");
	auto context = globals::d3d::context;

	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(sceneDesc.Width), static_cast<float>(sceneDesc.Height) });
	const UINT halfTexWidth = texRaysA->desc.Width;
	const UINT halfTexHeight = texRaysA->desc.Height;
	const UINT halfDynWidth = (static_cast<UINT>(resolution.x) + 1) / 2;
	const UINT halfDynHeight = (static_cast<UINT>(resolution.y) + 1) / 2;
	const int quality = std::clamp(settings.Quality, 0, kQualityLevels - 1);
	const int type = std::clamp(light.isSun ? settings.Type : settings.MoonType, 0, kTypeCount - 1);

	SunRaysCB data{};
	data.InvViewProj = globals::game::frameBufferCached.GetCameraViewProjInverse();
	data.LightUV = { lightNdc.x * 0.5f + 0.5f, 0.5f - lightNdc.y * 0.5f };
	data.UVScale = { static_cast<float>(halfDynWidth) / halfTexWidth, static_cast<float>(halfDynHeight) / halfTexHeight };
	data.UVMax = { (halfDynWidth - 0.5f) / halfTexWidth, (halfDynHeight - 0.5f) / halfTexHeight };
	data.LightDirection = { light.direction.x, light.direction.y, light.direction.z };
	data.LightBillboardTan = light.billboardTan;
	data.RaysColor = raysColor * globals::state->preExposure;
	data.MaskBrightness = lightPeak;
	data.MaskExponent = light.maskExponent;
	data.InvScreenWidth = 1.0f / std::max(resolution.x, 1.0f);
	data.ReverseZ = globals::features::reverseZ.IsActive() ? 1u : 0u;
	data.DynRes = resolution;
	data.FrameCount = globals::state->frameCount;

	{
		PostProcessingRaster::RasterPass pass(context);

		ID3D11Buffer* cb = raysCB->CB();
		ID3D11SamplerState* sampler = linearSampler.get();
		context->PSSetConstantBuffers(1, 1, &cb);
		context->PSSetSamplers(0, 1, &sampler);

		ID3D11ShaderResourceView* cloudSRV = nullptr;
		auto& cloudShadows = globals::features::cloudShadows;
		if (cloudShadows.loaded && cloudShadows.texCubemapCloudOccCopy)
			cloudSRV = cloudShadows.texCubemapCloudOccCopy->srv.get();

		auto drawPass = [&](ID3D11PixelShader* a_shader, ID3D11RenderTargetView* a_rtv, ID3D11ShaderResourceView* a_rays, float a_width, float a_height) {
			raysCB->Update(data);
			ID3D11ShaderResourceView* srvs[4] = { inout_tex.srv, a_rays, depthSRV, cloudSRV };
			ID3D11ShaderResourceView* nullSRVs[4] = {};
			context->PSSetShaderResources(0, 4, nullSRVs);
			pass.SetTargets({ a_rtv }, a_width, a_height);
			context->PSSetShaderResources(0, 4, srvs);
			pass.SetShaders(owner->GetFullscreenVS(), a_shader);
			pass.Draw();
		};

		const float halfW = static_cast<float>(halfDynWidth);
		const float halfH = static_cast<float>(halfDynHeight);
		drawPass(maskPS.get(), texRaysA->rtv.get(), nullptr, halfW, halfH);

		data.BlurFactor = kShortBlur[type];
		data.StepCount = kBlurSteps[0][quality];
		data.WeightedSteps = 0;
		data.UseNoise = 0;
		drawPass(blurPS.get(), texRaysB->rtv.get(), texRaysA->srv.get(), halfW, halfH);

		data.BlurFactor = kLongBlur[type];
		data.StepCount = kBlurSteps[1][quality];
		data.WeightedSteps = 1;
		data.UseNoise = 1;
		drawPass(blurPS.get(), texRaysA->rtv.get(), texRaysB->srv.get(), halfW, halfH);

		data.BlurFactor = kShortBlur[type];
		data.StepCount = kBlurSteps[2][quality];
		data.WeightedSteps = 0;
		drawPass(blurPS.get(), texRaysB->rtv.get(), texRaysA->srv.get(), halfW, halfH);

		data.StepCount = kCompositeSteps[quality];
		data.UseNoise = quality == 0 ? 1u : 0u;
		drawPass(compositePS.get(), texOutput->rtv.get(), texRaysB->srv.get(), resolution.x, resolution.y);

		ID3D11ShaderResourceView* nullSRVs[4] = {};
		cb = nullptr;
		sampler = nullptr;
		context->PSSetShaderResources(0, 4, nullSRVs);
		context->PSSetConstantBuffers(1, 1, &cb);
		context->PSSetSamplers(0, 1, &sampler);
	}

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	globals::profiler->EndPass();
}
