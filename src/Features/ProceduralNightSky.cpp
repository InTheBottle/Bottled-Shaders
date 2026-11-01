#include "ProceduralNightSky.h"

#include "Deferred.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/ProceduralSun.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.procedural_night_sky."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralNightSky::Settings,
	enabled,
	starIntensity,
	starTwinkle,
	auroraIntensity,
	auroraSpeed,
	auroraLighting)

namespace
{
	constexpr std::string_view kAuroraModelPrefix = "Sky\\SkyrimAurora";

	struct AuroraPalette
	{
		float3 low;
		float3 high;
	};

	const AuroraPalette kAuroraDefault{ { 0.35f, 1.0f, 0.67f }, { 0.76f, 0.46f, 0.98f } };
	const AuroraPalette kAuroraGreenBlue{ { 0.35f, 1.0f, 0.76f }, { 0.35f, 0.62f, 1.0f } };
	const AuroraPalette kAuroraBlueViolet{ { 0.46f, 0.70f, 1.0f }, { 0.85f, 0.42f, 1.0f } };
	const AuroraPalette kAuroraRedYellow{ { 1.0f, 0.86f, 0.46f }, { 1.0f, 0.38f, 0.50f } };

	bool HasAurora(const RE::TESWeather* a_weather)
	{
		if (!a_weather)
			return false;
		const char* model = const_cast<RE::TESWeather*>(a_weather)->aurora.GetModel();
		return model && _strnicmp(model, kAuroraModelPrefix.data(), kAuroraModelPrefix.size()) == 0;
	}

	bool SameAurora(const RE::TESWeather* a_first, const RE::TESWeather* a_second)
	{
		return _stricmp(const_cast<RE::TESWeather*>(a_first)->aurora.GetModel(), const_cast<RE::TESWeather*>(a_second)->aurora.GetModel()) == 0;
	}

	const AuroraPalette& GetModelPalette(const RE::TESWeather* a_weather)
	{
		const char* variant = const_cast<RE::TESWeather*>(a_weather)->aurora.GetModel() + kAuroraModelPrefix.size();
		if (_strnicmp(variant, "GB", 2) == 0)
			return kAuroraGreenBlue;
		if (_strnicmp(variant, "BV", 2) == 0)
			return kAuroraBlueViolet;
		if (_strnicmp(variant, "RY", 2) == 0)
			return kAuroraRedYellow;
		return kAuroraDefault;
	}

	AuroraPalette GetAuroraPalette(const RE::Sky* a_sky)
	{
		const auto* current = a_sky->currentWeather;
		const auto* previous = a_sky->lastWeather;
		if (!HasAurora(previous))
			return HasAurora(current) ? GetModelPalette(current) : kAuroraDefault;
		if (!HasAurora(current))
			return GetModelPalette(previous);

		const AuroraPalette& from = GetModelPalette(previous);
		const AuroraPalette& to = GetModelPalette(current);
		const float blend = a_sky->currentWeatherPct;
		return { float3::Lerp(from.low, to.low, blend), float3::Lerp(from.high, to.high, blend) };
	}

	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			globals::features::proceduralNightSky.Render(a_accumulator);
			func(a_accumulator, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

float ProceduralNightSky::GetAuroraVisibility(const RE::Sky* a_sky)
{
	const auto* current = a_sky->currentWeather;
	const auto* previous = a_sky->lastWeather;
	const bool currentHasAurora = HasAurora(current);
	const bool previousHasAurora = HasAurora(previous);
	if (!current || (!currentHasAurora && !previousHasAurora))
		return 0.0f;

	float weatherWeight = 1.0f;
	if (previous) {
		if (!currentHasAurora)
			weatherWeight = 1.0f - a_sky->currentWeatherPct;
		else if (!previousHasAurora || !SameAurora(current, previous))
			weatherWeight = a_sky->currentWeatherPct;
	}

	float timeWeight = 1.0f;
	if (!current->data.flags.any(RE::TESWeather::WeatherDataFlag::kPermAurora)) {
		const float hour = a_sky->currentGameHour;
		const float inStart = a_sky->auroraInStart;
		const float inEnd = a_sky->auroraIn;
		const float outStart = a_sky->auroraOutStart;
		const float outEnd = a_sky->auroraOut;
		if (inStart > hour && hour > outEnd) {
			timeWeight = 0.0f;
		} else if (hour <= inEnd && hour >= outStart) {
			const float fade = (hour <= inStart || inEnd <= hour) ? (hour - outEnd) / (outStart - outEnd) : (hour - inStart) / (inEnd - inStart);
			timeWeight = std::clamp(fade, 0.0f, 1.0f);
		}
	}

	return weatherWeight * timeWeight;
}

ProceduralNightSky::PerFrameData ProceduralNightSky::GetCommonBufferData() const
{
	PerFrameData data{};
	if (!loaded || !settings.enabled)
		return data;

	const auto sky = globals::game::sky;

	data.enabled = true;
	data.starIntensity = settings.starIntensity;
	data.starTwinkle = settings.starTwinkle;
	if (sky) {
		const AuroraPalette palette = GetAuroraPalette(sky);
		data.auroraLowColor = palette.low;
		data.auroraHighColor = palette.high;
		data.auroraIntensity = settings.auroraIntensity * GetAuroraVisibility(sky);
	}
	data.auroraSpeed = settings.auroraSpeed;
	data.auroraLighting = settings.auroraLighting;
	data.radianceLimit = ProceduralSun::GetMainTargetRadianceLimit();
	return data;
}

void ProceduralNightSky::SetupResources()
{
	auto device = globals::d3d::device;

	{
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ONE;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "ProceduralNightSky::BlendState");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "ProceduralNightSky::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "ProceduralNightSky::DepthStencilState");
	}

	GetAuroraVS();
	GetAuroraPS();
}

void ProceduralNightSky::ClearShaderCache()
{
	auroraVS = nullptr;
	auroraPS = nullptr;
	shaderCompileFailed = false;

	GetAuroraVS();
	GetAuroraPS();
}

ID3D11VertexShader* ProceduralNightSky::GetAuroraVS()
{
	if (!auroraVS && !shaderCompileFailed) {
		auroraVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralNightSky\\AuroraVS.hlsl", {}, "vs_5_0")));
		shaderCompileFailed = !auroraVS;
	}
	return auroraVS.get();
}

ID3D11PixelShader* ProceduralNightSky::GetAuroraPS()
{
	if (!auroraPS && !shaderCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		auroraPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralNightSky\\AuroraPS.hlsl", defines, "ps_5_0")));
		shaderCompileFailed = !auroraPS;
	}
	return auroraPS.get();
}

void ProceduralNightSky::PostPostLoad()
{
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));
	logger::info("[ProceduralNightSky] Installed hooks");
}

void ProceduralNightSky::Reset()
{
	const auto sky = globals::game::sky;
	const auto aurora = sky ? sky->auroraRoot.get() : nullptr;
	if (!aurora)
		return;

	const bool replaced = globals::shaderCache->IsEnabled() && auroraVS && auroraPS;
	const bool weatherHasAurora = HasAurora(sky->currentWeather) || HasAurora(sky->lastWeather);
	aurora->SetAppCulled(replaced && settings.enabled && settings.auroraIntensity > 0.0f && weatherHasAurora);
}

bool ProceduralNightSky::HasAuroraLight() const
{
	const auto sky = globals::game::sky;
	return loaded && sky && settings.enabled && settings.auroraIntensity > 0.0f && settings.auroraLighting > 0.0f && GetAuroraVisibility(sky) > 0.0f;
}

void ProceduralNightSky::Render(RE::BSShaderAccumulator* a_accumulator)
{
	if (!loaded || !a_accumulator || !settings.enabled || settings.auroraIntensity <= 0.0f)
		return;

	auto state = globals::state;
	if (!state || !state->inWorld || !globals::shaderCache->IsEnabled())
		return;
	if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
		return;

	auto smState = globals::game::smState;
	const auto* accumulatorData = a_accumulator->GetRuntimeData();
	if (!accumulatorData || accumulatorData->firstPerson || !smState || accumulatorData->activeShadowSceneNode != smState->shadowSceneNode[0])
		return;

	if (lastRenderedFrame == state->frameCount)
		return;
	lastRenderedFrame = state->frameCount;

	auto sky = globals::game::sky;
	if (sky && sky->mode.get() == RE::Sky::Mode::kFull && GetAuroraVisibility(sky) > 0.0f)
		DrawAurora();
}

void ProceduralNightSky::DrawAurora()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	auto vertexShader = GetAuroraVS();
	auto pixelShader = GetAuroraPS();
	if (!vertexShader || !pixelShader)
		return;

	auto mainTarget = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto sceneDepth = Util::GetCurrentSceneDepthSRV(false);
	if (!mainTarget.RTV || !mainTarget.texture || !sceneDepth)
		return;

	D3D11_TEXTURE2D_DESC textureDesc{};
	mainTarget.texture->GetDesc(&textureDesc);

	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(context);

	auto profiler = globals::profiler;
	profiler->BeginPass("ProceduralNightSky::Aurora");

	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height) });
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	context->OMSetRenderTargets(1, &mainTarget.RTV, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D11Buffer* perFrame = *globals::game::perFrame.get();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->VSSetShader(vertexShader, nullptr, 0);
	context->PSSetConstantBuffers(5, 2, sharedBuffers);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetShaderResources(0, 1, &sceneDepth);
	context->PSSetShader(pixelShader, nullptr, 0);

	context->Draw(3, 0);

	profiler->EndPass();
	backup.Restore(context);
}

void ProceduralNightSky::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Replaces the vanilla stars and aurora. Each only shows when the weather and time of day would show it."));

	if (ImGui::TreeNodeEx(T(TKEY("stars"), "Stars"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("star_intensity"), "Brightness"), &settings.starIntensity, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("star_twinkle"), "Twinkle"), &settings.starTwinkle, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("aurora"), "Aurora"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("aurora_intensity"), "Aurora Brightness"), &settings.auroraIntensity, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("aurora_intensity_tooltip"), "Appears in weathers that have an aurora and replaces the vanilla one. 0 brings the vanilla aurora back."));
		ImGui::SliderFloat(T(TKEY("aurora_speed"), "Aurora Speed"), &settings.auroraSpeed, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("aurora_lighting"), "Aurora Lighting"), &settings.auroraLighting, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("aurora_lighting_tooltip"), "How strongly the aurora lights the world through Image Based Lighting. 0 turns it off."));
		ImGui::TreePop();
	}
}

void ProceduralNightSky::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.starIntensity, defaults.starIntensity, 0.0f, 4.0f);
	sanitize(settings.starTwinkle, defaults.starTwinkle, 0.0f, 1.0f);
	sanitize(settings.auroraIntensity, defaults.auroraIntensity, 0.0f, 4.0f);
	sanitize(settings.auroraSpeed, defaults.auroraSpeed, 0.0f, 4.0f);
	sanitize(settings.auroraLighting, defaults.auroraLighting, 0.0f, 4.0f);
}

void ProceduralNightSky::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ProceduralNightSky::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
