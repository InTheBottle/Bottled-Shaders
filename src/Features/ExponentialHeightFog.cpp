#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "WeatherVariableRegistry.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.exp_height_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExponentialHeightFog::Settings,
	enabled,
	useDynamicCubemaps,
	startDistance,
	fogHeight,
	fogHeightFalloff,
	fogDensity,
	fogHeight2,
	fogHeightFalloff2,
	fogDensity2,
	directionalInscatteringMultiplier,
	directionalInscatteringAnisotropy,
	useSkyIBL,
	inscatteringTint,
	cubemapMipLevel,
	sunlightAttenuationAmount,
	respectVanillaFogFade,
	disableVanillaFog,
	fogInscatteringColor,
	originalFogColorAmount,
	volumetricFogEnabled,
	volumetricGridPixelSize,
	volumetricGridSizeZ,
	volumetricFogDistance,
	volumetricFogStartDistance,
	volumetricFogNearFadeInDistance,
	volumetricFogExtinctionScale,
	volumetricFogScatteringDistribution,
	volumetricFogAlbedo,
	volumetricFogEmissive,
	volumetricDirectionalScatteringIntensity,
	volumetricShadowBias,
	volumetricDepthDistributionScale,
	volumetricSkyLightingIntensity,
	volumetricHistoryWeight,
	volumetricHistoryMissSampleCount,
	volumetricSampleJitterMultiplier,
	volumetricUpsampleJitterMultiplier,
	volumetricNearGridDistance,
	volumetricFarGridPixelSize,
	volumetricFarGridSizeZ,
	volumetricFogNoiseScale,
	volumetricFogNoiseThreshold,
	volumetricFogNoiseVelocity,
	volumetricLocalLightScatteringIntensity,
	useVanillaFogSettings,
	vanillaFogStrength,
	fogLightingInfluence,
	distanceHazeMaxOpacity,
	distanceHazeStartDistance,
	distanceHazeFadeDistance)

namespace
{
	constexpr float kMinimumFogRange = 1.0f;
	constexpr float kMinimumFogPower = 0.01f;
	constexpr float kMinimumFogTransmittance = 0.0001f;
	constexpr float kReferenceOpacityFraction = 0.5f;
	constexpr float kMaximumWeatherHistoryChange = 0.1f;
	constexpr float kAnalyticalExtinctionScale = 0.001f * std::numbers::ln2_v<float> * std::numbers::ln2_v<float>;
	constexpr float4 kFallbackFogColor{ 0.85f, 0.88f, 0.92f, 1.0f };

	constexpr float kMinDistanceHazeFadeDistance = 1.0f;
	constexpr float kMaxDistanceHazeDistance = 200000.0f;

	float ClampFinite(float a_value, float a_min, float a_max, float a_default)
	{
		return std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_default;
	}

	void ClampFollowAndHazeSettings(ExponentialHeightFog::Settings& a_settings)
	{
		const ExponentialHeightFog::Settings defaults{};
		a_settings.vanillaFogStrength = ClampFinite(a_settings.vanillaFogStrength, 0.0f, 4.0f, defaults.vanillaFogStrength);
		a_settings.fogLightingInfluence = ClampFinite(a_settings.fogLightingInfluence, 0.0f, 1.0f, defaults.fogLightingInfluence);
		a_settings.distanceHazeMaxOpacity = ClampFinite(a_settings.distanceHazeMaxOpacity, 0.0f, 1.0f, defaults.distanceHazeMaxOpacity);
		a_settings.distanceHazeStartDistance = ClampFinite(a_settings.distanceHazeStartDistance, 0.0f, kMaxDistanceHazeDistance, defaults.distanceHazeStartDistance);
		a_settings.distanceHazeFadeDistance = ClampFinite(a_settings.distanceHazeFadeDistance, kMinDistanceHazeFadeDistance, kMaxDistanceHazeDistance, defaults.distanceHazeFadeDistance);
	}

	bool CanReuseFogHistory(const ExponentialHeightFog::Settings& current, const ExponentialHeightFog::Settings& previous)
	{
		using Settings = ExponentialHeightFog::Settings;
		if (current.useVanillaFogSettings != previous.useVanillaFogSettings)
			return false;
		for (const auto field : {
				 &Settings::vanillaFogStrength, &Settings::fogLightingInfluence,
				 &Settings::fogDensity, &Settings::fogHeight, &Settings::fogHeightFalloff,
				 &Settings::fogDensity2, &Settings::fogHeight2, &Settings::fogHeightFalloff2,
				 &Settings::volumetricFogDistance, &Settings::volumetricFogStartDistance, &Settings::volumetricNearGridDistance,
				 &Settings::volumetricFogNearFadeInDistance, &Settings::volumetricFogExtinctionScale,
				 &Settings::volumetricDepthDistributionScale }) {
			const float value = current.*field;
			const float previousValue = previous.*field;
			if (std::abs(value - previousValue) > kMaximumWeatherHistoryChange * std::max(std::abs(value), std::abs(previousValue)))
				return false;
		}
		if (current.useVanillaFogSettings) {
			if (std::abs(current.vanillaFogDensity - previous.vanillaFogDensity) >
				kMaximumWeatherHistoryChange * std::max(current.vanillaFogDensity, previous.vanillaFogDensity))
				return false;
			auto weatherHistoryMatches = [](float value, float previousValue) {
				return std::abs(value - previousValue) <= kMaximumWeatherHistoryChange * std::max({ 1.0f, std::abs(value), std::abs(previousValue) });
			};
			for (const auto field : { &Settings::vanillaFogNear, &Settings::vanillaFogFar, &Settings::vanillaFogMaxOpacity, &Settings::vanillaFogPower }) {
				if (!weatherHistoryMatches(current.*field, previous.*field))
					return false;
			}
			for (const auto field : { &Settings::vanillaFogNearColor, &Settings::vanillaFogFarColor }) {
				const auto& color = current.*field;
				const auto& previousColor = previous.*field;
				if (!weatherHistoryMatches(color.x, previousColor.x) ||
					!weatherHistoryMatches(color.y, previousColor.y) ||
					!weatherHistoryMatches(color.z, previousColor.z))
					return false;
			}
		}
		return true;
	}

	float Halton(uint32_t a_index, uint32_t a_base)
	{
		float result = 0.0f;
		float invBase = 1.0f / static_cast<float>(a_base);
		float fraction = invBase;
		while (a_index > 0) {
			result += static_cast<float>(a_index % a_base) * fraction;
			a_index /= a_base;
			fraction *= invBase;
		}
		return result;
	}
}

void ExponentialHeightFog::RestoreDefaultSettings()
{
	settings = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampFollowAndHazeSettings(settings);
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

ExponentialHeightFog::Settings ExponentialHeightFog::GetCommonBufferData() const
{
	Settings data = settings;
	ClampFollowAndHazeSettings(data);
	data.vanillaFogDensity = 0.0f;

	if (IsSuppressed()) {
		data.enabled = 0;
		data.disableVanillaFog = 0;
		return data;
	}

	const auto* sky = globals::game::sky;
	const bool hasUnboundedFogRange = sky && (sky->fogNear == std::numeric_limits<float>::infinity() || sky->fogFar == std::numeric_limits<float>::infinity());
	const float fogNear = sky ? std::max(std::isfinite(sky->fogNear) ? sky->fogNear : 0.0f, 0.0f) : 0.0f;
	const float fogFar = sky ? std::max(std::isfinite(sky->fogFar) ? sky->fogFar : fogNear + kMinimumFogRange, fogNear + kMinimumFogRange) : Settings{}.vanillaFogFar;
	const float fogPower = sky ? std::max(std::isfinite(sky->fogPower) ? sky->fogPower : 1.0f, kMinimumFogPower) : 1.0f;
	const float fogClamp = sky && !hasUnboundedFogRange ? std::clamp(std::isfinite(sky->fogClamp) ? sky->fogClamp : 0.0f, 0.0f, 1.0f - kMinimumFogTransmittance) : 0.0f;

	data.vanillaFogNear = fogNear;
	data.vanillaFogFar = fogFar;
	data.vanillaFogMaxOpacity = fogClamp;
	data.vanillaFogPower = fogPower;
	data.vanillaFogNearColor = kFallbackFogColor;
	data.vanillaFogFarColor = kFallbackFogColor;
	if (sky) {
		auto sanitizeColor = [](const RE::NiColor& color, const float4& fallback) {
			return float4{
				std::max(std::isfinite(color.red) ? color.red : fallback.x, 0.0f),
				std::max(std::isfinite(color.green) ? color.green : fallback.y, 0.0f),
				std::max(std::isfinite(color.blue) ? color.blue : fallback.z, 0.0f), 1.0f
			};
		};
		data.vanillaFogFarColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogFar)], kFallbackFogColor);
		data.vanillaFogNearColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogNear)], kFallbackFogColor);
	}
	if (!data.useVanillaFogSettings)
		return data;

	data.disableVanillaFog = 1;
	data.startDistance = fogNear;
	const float targetOpacity = data.vanillaFogMaxOpacity * kReferenceOpacityFraction;
	const float normalizedReference = std::pow(targetOpacity, 1.0f / fogPower);
	const float referenceDistance = std::max((fogFar - fogNear) * normalizedReference, kMinimumFogRange);
	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const float calibratedOpacity = linearLightingData.enableLinearLighting ?
	                                    std::pow(targetOpacity, linearLightingData.fogAlphaGamma) :
	                                    targetOpacity;
	data.vanillaFogDensity = -std::log(std::max(1.0f - calibratedOpacity, kMinimumFogTransmittance)) / (referenceDistance * kAnalyticalExtinctionScale);
	return data;
}

bool ExponentialHeightFog::IsSuppressed() const
{
	// The world/local map keeps its vanilla fog; height fog tuned for eye level washes it out
	return globals::state->isMapMenuOpen;
}

void ExponentialHeightFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_exp_height_fog"), "Enable Exponential Height Fog"), (bool*)&settings.enabled);
	if (!ImGui::BeginTabBar("##ExponentialHeightFogTabs"))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawGeneralSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(T(TKEY("volumetric_fog"), "Volumetric Fog"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawVolumetricSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

void ExponentialHeightFog::DrawGeneralSettings()
{
	ImGui::Checkbox(T(TKEY("use_vanilla_fog_settings"), "Follow Vanilla Fog"), (bool*)&settings.useVanillaFogSettings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_vanilla_fog_settings_tooltip"), "Derives fog density, start distance, and colors from the active weather while keeping exponential height falloff. Weather Fog Strength scales density. Replaces vanilla distance fog."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("vanilla_strength"), "Weather Fog Strength"), this, "vanillaFogStrength", &settings.vanillaFogStrength, 0.0f, 4.0f, "%.2f");
	Util::WeatherUI::SliderFloat(T(TKEY("lighting_influence"), "Weather Lighting Influence"), this, "fogLightingInfluence", &settings.fogLightingInfluence, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("density_height"), "Density and Height"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("fog_density"), "Fog Density"), this, "fogDensity", &settings.fogDensity, 0.0f, 1.0f, "%.3f");
	Util::WeatherUI::SliderFloat(T(TKEY("start_distance"), "Start Distance"), this, "startDistance", &settings.startDistance, 0.0f, 100000.0f, "%.1f");
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("fog_height"), "Fog Height"), this, "fogHeight", &settings.fogHeight, -22000.0f, 22000.0f, "%.1f");
	Util::WeatherUI::SliderFloat(T(TKEY("fog_height_falloff"), "Fog Height Falloff"), this, "fogHeightFalloff", &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f");
	if (ImGui::TreeNode(T(TKEY("second_fog_layer"), "Second Fog Layer"))) {
		Util::WeatherUI::SliderFloat(T(TKEY("fog_height_2"), "Fog Height 2"), this, "fogHeight2", &settings.fogHeight2, -22000.0f, 22000.0f, "%.1f");
		Util::WeatherUI::SliderFloat(T(TKEY("fog_height_falloff_2"), "Fog Height Falloff 2"), this, "fogHeightFalloff2", &settings.fogHeightFalloff2, 0.001f, 2.0f, "%.3f");
		Util::WeatherUI::SliderFloat(T(TKEY("fog_density_2"), "Fog Density 2"), this, "fogDensity2", &settings.fogDensity2, 0.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("second_fog_layer_tooltip"),
								  "Adds a second stacked exponential height fog layer with its own base height, density and height falloff.\n"
								  "The two line integrals are summed.\n"
								  "Use it for high-altitude haze above the ground layer or a distinct low-lying ground fog."));
		}
		ImGui::TreePop();
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0 && settings.volumetricFogEnabled == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_extinction_scale"), "Extinction Scale"), this, "volumetricFogExtinctionScale", &settings.volumetricFogExtinctionScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_extinction_scale_tooltip"), "Scales fog density when following vanilla fog, including when volumetric fog is off. In manual mode, affects only volumetric fog."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	Util::WeatherUI::ColorEdit4(T(TKEY("fog_inscattering_color"), "Fog Inscattering Color"), this, "fogInscatteringColor", (float*)&settings.fogInscatteringColor);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("original_fog_color_amount"), "Original Fog Color Amount"), this, "originalFogColorAmount", &settings.originalFogColorAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("dir_inscattering_mul"), "Directional Light Inscattering Multiplier"), this, "directionalInscatteringMultiplier", &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(settings.directionalInscatteringMultiplier <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("dir_inscattering_anisotropy"), "Directional Light Inscattering Anisotropy"), this, "directionalInscatteringAnisotropy", &settings.directionalInscatteringAnisotropy, -0.99f, 0.99f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dir_inscattering_anisotropy_tooltip"),
							  "Controls the asymmetry of inscattering via the Henyey-Greenstein phase function.\n"
							  "Positive values produce forward scattering (glow around sun).\n"
							  "Zero is isotropic. Negative values produce back scattering."));
	}
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("sunlight_attenuation"), "Sunlight Attenuation Amount"), this, "sunlightAttenuationAmount", &settings.sunlightAttenuationAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("vanilla_fog"), "Vanilla Fog"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::Checkbox(T(TKEY("disable_vanilla_fog"), "Disable Vanilla Fog"), (bool*)&settings.disableVanillaFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_vanilla_fog_tooltip"), "Disables the vanilla fog entirely. Only exponential height fog will be applied."));
	}
	ImGui::EndDisabled();
	Util::WeatherUI::Checkbox(T(TKEY("apply_vanilla_fade"), "Apply Vanilla Fade"), this, "respectVanillaFogFade", (bool*)&settings.respectVanillaFogFade);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_vanilla_fade_tooltip"), "Applies vanilla fade brightness to exponential height fog."));
	}

	ImGui::SeparatorText(T(TKEY("inscattering_cubemap"), "Inscattering Cubemap"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::BeginDisabled(!globals::features::ibl.loaded);
	ImGui::Checkbox(T(TKEY("use_sky_ibl"), "Use Sky IBL for Exterior Inscattering"), (bool*)&settings.useSkyIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_sky_ibl_tooltip"), "Adds the sky IBL color to the fog inscattering in exteriors. Requires the Image Based Lighting feature."));
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!globals::features::dynamicCubemaps.loaded);
	ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps"), "Use Dynamic Cubemaps for Interior Inscattering"), (bool*)&settings.useDynamicCubemaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_tooltip"), "Adds the dynamic environment cubemap color to the fog inscattering in interiors."));
	}
	ImGui::EndDisabled();
	const bool hasCubemapInscattering = (settings.useSkyIBL != 0 && globals::features::ibl.loaded) ||
	                                    (settings.useDynamicCubemaps != 0 && globals::features::dynamicCubemaps.loaded);
	ImGui::BeginDisabled(!hasCubemapInscattering);
	Util::WeatherUI::ColorEdit4(T(TKEY("inscattering_cubemap_tint"), "Inscattering Cubemap Tint"), this, "inscatteringTint", (float*)&settings.inscatteringTint);
	ImGui::BeginDisabled(settings.inscatteringTint.w <= 0.0f);
	ImGui::SliderFloat(T(TKEY("cubemap_mip_level"), "Cubemap Mip Level"), &settings.cubemapMipLevel, 1.0f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("distance_haze"), "Distance Haze"));
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_max_opacity"), "Haze Maximum Opacity"), this, "distanceHazeMaxOpacity", &settings.distanceHazeMaxOpacity, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_max_opacity_tooltip"), "Adds haze at all heights using the fog colors above, while preserving dense height fog. Zero disables distance haze."));
	}
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_start_distance"), "Haze Start Distance"), this, "distanceHazeStartDistance", &settings.distanceHazeStartDistance, 0.0f, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_start_distance_tooltip"), "Horizontal distance from the camera where haze begins, in game units. Independent of height fog Start Distance."));
	}
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_fade_distance"), "Haze Fade Distance"), this, "distanceHazeFadeDistance", &settings.distanceHazeFadeDistance, kMinDistanceHazeFadeDistance, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_fade_distance_tooltip"), "Horizontal distance beyond Haze Start Distance over which haze smoothly reaches its maximum opacity, in game units."));
	}
}

void ExponentialHeightFog::DrawVolumetricSettings()
{
	Util::WeatherUI::Checkbox(T(TKEY("enable_volumetric_fog"), "Enable Volumetric Fog"), this, "volumetricFogEnabled", (bool*)&settings.volumetricFogEnabled);
	ImGui::BeginDisabled(settings.volumetricFogEnabled == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_view_distance"), "Volumetric View Distance"), this, "volumetricFogDistance", &settings.volumetricFogDistance, 1000.0f, 200000.0f, "%.0f");
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_start_distance"), "Volumetric Start Distance"), this, "volumetricFogStartDistance", &settings.volumetricFogStartDistance, 0.0f, 20000.0f, "%.0f");
	Util::WeatherUI::SliderFloat(T(TKEY("near_fade_in_distance"), "Near Fade In Distance"), this, "volumetricFogNearFadeInDistance", &settings.volumetricFogNearFadeInDistance, 0.0f, 20000.0f, "%.0f");
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_near_grid_distance"), "Near Grid Distance"), this, "volumetricNearGridDistance", &settings.volumetricNearGridDistance, 256.0f, 50000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_near_grid_distance_tooltip"),
							  "Distance covered by the full-resolution near volume.\n"
							  "A second, quarter-lattice far volume covers the remaining distance up to the Volumetric View Distance.\n"
							  "Smaller values improve near-field resolution; larger values move the low-resolution far volume farther away."));
	}
	if (ImGui::TreeNode(T(TKEY("volumetric_noise"), "Volumetric Noise"))) {
		Util::WeatherUI::SliderFloat(T(TKEY("volumetric_noise_scale"), "Noise Scale"), this, "volumetricFogNoiseScale", &settings.volumetricFogNoiseScale, 0.0f, 0.01f, "%.6f");
		Util::WeatherUI::SliderFloat(T(TKEY("volumetric_noise_threshold"), "Noise Threshold"), this, "volumetricFogNoiseThreshold", &settings.volumetricFogNoiseThreshold, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat3(T(TKEY("volumetric_noise_velocity"), "Noise Velocity"), &settings.volumetricFogNoiseVelocity.x, -1.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("volumetric_noise_tooltip"),
								  "Modulates the volumetric fog density with a 3D value noise field.\n"
								  "Noise Scale: spatial frequency of the fog clumps (0 = disabled).\n"
								  "Noise Threshold: soft cutoff that carves clumps out of the noise.\n"
								  "Noise Velocity: animation drift of the noise field, scaled by time."));
		}
		ImGui::TreePop();
	}

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	Util::WeatherUI::ColorEdit4(T(TKEY("volumetric_albedo"), "Volumetric Albedo"), this, "volumetricFogAlbedo", (float*)&settings.volumetricFogAlbedo);
	Util::WeatherUI::ColorEdit4(T(TKEY("volumetric_emissive"), "Volumetric Emissive"), this, "volumetricFogEmissive", (float*)&settings.volumetricFogEmissive);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("directional_scattering_intensity"), "Directional Scattering Intensity"), this, "volumetricDirectionalScatteringIntensity", &settings.volumetricDirectionalScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(settings.volumetricDirectionalScatteringIntensity <= 0.0f);
	ImGui::SliderFloat(T(TKEY("directional_shadow_bias"), "Directional Shadow Bias"), &settings.volumetricShadowBias, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("sky_lighting_scattering_intensity"), "Sky Lighting Scattering Intensity"), this, "volumetricSkyLightingIntensity", &settings.volumetricSkyLightingIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(!globals::features::lightLimitFix.loaded);
	Util::WeatherUI::SliderFloat(T(TKEY("local_light_scattering_intensity"), "Local Light Scattering Intensity"), this, "volumetricLocalLightScatteringIntensity", &settings.volumetricLocalLightScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::EndDisabled();
	const bool hasScattering = settings.volumetricDirectionalScatteringIntensity > 0.0f || settings.volumetricSkyLightingIntensity > 0.0f ||
	                           (globals::features::lightLimitFix.loaded && settings.volumetricLocalLightScatteringIntensity > 0.0f);
	ImGui::BeginDisabled(!hasScattering);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_scattering_distribution"), "Volumetric Scattering Distribution"), this, "volumetricFogScatteringDistribution", &settings.volumetricFogScatteringDistribution, -0.9f, 0.9f, "%.2f");
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	if (ImGui::TreeNode(T(TKEY("debug"), "Quality and Temporal Filtering"))) {
		uint32_t minGridPixelSize = 4;
		uint32_t maxGridPixelSize = 64;
		uint32_t minGridSizeZ = 16;
		uint32_t maxGridSizeZ = 160;
		ImGui::SliderScalar(T(TKEY("grid_pixel_size"), "Grid Pixel Size"), ImGuiDataType_U32, &settings.volumetricGridPixelSize, &minGridPixelSize, &maxGridPixelSize, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("grid_depth_slices"), "Grid Depth Slices"), ImGuiDataType_U32, &settings.volumetricGridSizeZ, &minGridSizeZ, &maxGridSizeZ, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("far_grid_pixel_size"), "Far Grid Pixel Size"), ImGuiDataType_U32, &settings.volumetricFarGridPixelSize, &minGridPixelSize, &maxGridPixelSize, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("far_grid_depth_slices"), "Far Grid Depth Slices"), ImGuiDataType_U32, &settings.volumetricFarGridSizeZ, &minGridSizeZ, &maxGridSizeZ, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("depth_distribution_scale"), "Depth Distribution Scale"), &settings.volumetricDepthDistributionScale, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		const bool temporalReprojection = Util::GetTemporal();
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("temporal_history_weight"), "Temporal History Weight"), &settings.volumetricHistoryWeight, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		uint32_t minHistoryMissSampleCount = 1;
		uint32_t maxHistoryMissSampleCount = 16;
		ImGui::SliderScalar(T(TKEY("history_miss_samples"), "History Miss Samples"), ImGuiDataType_U32, &settings.volumetricHistoryMissSampleCount, &minHistoryMissSampleCount, &maxHistoryMissSampleCount, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("sample_jitter_multiplier"), "Sample Jitter Multiplier"), &settings.volumetricSampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("sample_jitter_multiplier_tooltip"),
								  "Adds per-voxel random offset on top of the Halton sequence."));
		}
		ImGui::EndDisabled();
		ImGui::SliderFloat(T(TKEY("upsample_jitter_multiplier"), "Upsample Jitter Multiplier"), &settings.volumetricUpsampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("upsample_jitter_multiplier_tooltip"),
								  "Jitters the final 3D fog lookup in screen space to hide\n"
								  "low-resolution froxel pixelization."));
		}
		ImGui::TreePop();
	}
	ImGui::EndDisabled();
}

void ExponentialHeightFog::SetupResources()
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "ExponentialHeightFog::LinearSampler");

	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
	Util::SetResourceName(shadowSampler.get(), "ExponentialHeightFog::ShadowSampler");

	volumetricFogCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricFogCB>(), "ExponentialHeightFog::VolumetricFogCB");
}

void ExponentialHeightFog::ClearShaderCache()
{
	if (materialSetupCS) {
		materialSetupCS->Release();
		materialSetupCS = nullptr;
	}
	if (farMaterialSetupCS) {
		farMaterialSetupCS->Release();
		farMaterialSetupCS = nullptr;
	}
	if (conservativeDepthCS) {
		conservativeDepthCS->Release();
		conservativeDepthCS = nullptr;
	}
	if (farConservativeDepthCS) {
		farConservativeDepthCS->Release();
		farConservativeDepthCS = nullptr;
	}
	if (lightScatteringCS) {
		lightScatteringCS->Release();
		lightScatteringCS = nullptr;
	}
	if (farLightScatteringCS) {
		farLightScatteringCS->Release();
		farLightScatteringCS = nullptr;
	}
	if (integrationCS) {
		integrationCS->Release();
		integrationCS = nullptr;
	}
	if (farIntegrationCS) {
		farIntegrationCS->Release();
		farIntegrationCS = nullptr;
	}
}

void ExponentialHeightFog::CaptureDirectionalShadowMap()
{
	ID3D11ShaderResourceView* shadowMap = nullptr;
	globals::d3d::context->PSGetShaderResources(4, 1, &shadowMap);
	directionalShadowMap.copy_from(shadowMap);
	if (shadowMap)
		shadowMap->Release();
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
	uint32_t pixelSize = std::clamp(settings.volumetricGridPixelSize, 4u, 64u);
	const uint32_t gridZ = std::clamp(settings.volumetricGridSizeZ, 16u, 160u);
	uint32_t farPixelSize = std::clamp(settings.volumetricFarGridPixelSize, 4u, 64u);
	const uint32_t farGridZ = std::clamp(settings.volumetricFarGridSizeZ, 16u, 160u);
	float2 screenSz{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSz);

	auto getGridSize = [&renderSize](uint32_t a_pixelSize, uint32_t a_gridZ) {
		return DirectX::XMUINT4{
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
			a_gridZ,
			0u
		};
	};
	DirectX::XMUINT4 gridSize = getGridSize(pixelSize, gridZ);

	constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
	while (pixelSize < 64u &&
		   static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
		pixelSize++;
		gridSize = getGridSize(pixelSize, gridZ);
	}

	// The far volume must be coarser than the near volume.
	farPixelSize = std::max(farPixelSize, pixelSize);
	DirectX::XMUINT4 farGridSize = getGridSize(farPixelSize, farGridZ);
	while (farPixelSize < 64u &&
		   static_cast<uint64_t>(farGridSize.x) * farGridSize.y * farGridSize.z > maxVolumeVoxels / 4ull) {
		farPixelSize++;
		farGridSize = getGridSize(farPixelSize, farGridZ);
	}

	if (vBufferA &&
		currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z &&
		currentFarGridSize.x == farGridSize.x && currentFarGridSize.y == farGridSize.y && currentFarGridSize.z == farGridSize.z)
		return;

	currentGridSize = gridSize;
	currentFarGridSize = farGridSize;

	auto make3D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE3D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.Depth = a_size.z;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.MipSlice = 0;
		uavDesc.Texture3D.FirstWSlice = 0;
		uavDesc.Texture3D.WSize = a_size.z;

		auto tex = std::make_unique<Texture3D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	auto make2D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		auto tex = std::make_unique<Texture2D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	vBufferA = make3D(gridSize, "ExponentialHeightFog::VBufferA");
	conservativeDepth = make2D(gridSize, "ExponentialHeightFog::ConservativeDepth");
	lightScattering = make3D(gridSize, "ExponentialHeightFog::LightScattering");
	integratedLightScattering = make3D(gridSize, "ExponentialHeightFog::IntegratedLightScattering");

	conservativeDepthHistory = std::make_unique<Texture2D>(conservativeDepth->desc, "ExponentialHeightFog::ConservativeDepthHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepth->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthHistory->CreateSRV(srvDesc);
	}

	lightScatteringHistory = std::make_unique<Texture3D>(lightScattering->desc, "ExponentialHeightFog::LightScatteringHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScattering->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringHistory->CreateSRV(srvDesc);
	}

	vBufferAFar = make3D(farGridSize, "ExponentialHeightFog::VBufferAFar");
	conservativeDepthFar = make2D(farGridSize, "ExponentialHeightFog::ConservativeDepthFar");
	lightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::LightScatteringFar");
	integratedLightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::IntegratedLightScatteringFar");

	conservativeDepthFarHistory = std::make_unique<Texture2D>(conservativeDepthFar->desc, "ExponentialHeightFog::ConservativeDepthFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepthFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthFarHistory->CreateSRV(srvDesc);
	}

	lightScatteringFarHistory = std::make_unique<Texture3D>(lightScatteringFar->desc, "ExponentialHeightFog::LightScatteringFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScatteringFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringFarHistory->CreateSRV(srvDesc);
	}

	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
	vBufferA.reset();
	vBufferAFar.reset();
	conservativeDepth.reset();
	conservativeDepthHistory.reset();
	conservativeDepthFar.reset();
	conservativeDepthFarHistory.reset();
	lightScattering.reset();
	lightScatteringHistory.reset();
	lightScatteringFar.reset();
	lightScatteringFarHistory.reset();
	integratedLightScattering.reset();
	integratedLightScatteringFar.reset();
	currentGridSize = {};
	currentFarGridSize = {};
	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &nullSRV);
	globals::d3d::context->PSSetShaderResources(22, 1, &nullSRV);
}

void ExponentialHeightFog::BindIntegratedLightScattering()
{
	ID3D11ShaderResourceView* srv = integratedLightScattering ? integratedLightScattering->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &srv);
	ID3D11ShaderResourceView* farSrv = integratedLightScatteringFar ? integratedLightScatteringFar->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(22, 1, &farSrv);
}

ID3D11ComputeShader* ExponentialHeightFog::GetMaterialSetupCS()
{
	if (!materialSetupCS)
		materialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", {}, "cs_5_0"));
	return materialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetConservativeDepthCS()
{
	if (!conservativeDepthCS)
		conservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", {}, "cs_5_0"));
	return conservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetLightScatteringCS()
{
	if (!lightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		lightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return lightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarLightScatteringCS()
{
	if (!farLightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.emplace_back("VOLUMETRIC_FOG_FAR_GRID", "");
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		farLightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return farLightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarMaterialSetupCS()
{
	if (!farMaterialSetupCS)
		farMaterialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farMaterialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarConservativeDepthCS()
{
	if (!farConservativeDepthCS)
		farConservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farConservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetIntegrationCS()
{
	if (!integrationCS)
		integrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", {}, "cs_5_0"));
	return integrationCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarIntegrationCS()
{
	if (!farIntegrationCS)
		farIntegrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farIntegrationCS;
}

void ExponentialHeightFog::Prepass()
{
	if (!settings.enabled || !settings.volumetricFogEnabled || settings.volumetricFogExtinctionScale <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	if (IsSuppressed())
		return;

	const auto cameraData = Util::GetCameraData();
	const float volumeStart = settings.useVanillaFogSettings ? 0.0f : std::max(settings.volumetricFogStartDistance, 0.0f);
	if (settings.volumetricFogDistance <= std::max(cameraData.y, volumeStart) + 10.0f) {
		ReleaseVolumetricResources();
		return;
	}
	const Settings frameSettings = GetCommonBufferData();
	const float fogDensity = frameSettings.useVanillaFogSettings ? frameSettings.vanillaFogDensity * frameSettings.vanillaFogStrength : frameSettings.fogDensity;
	if (fogDensity <= 0.0f && frameSettings.fogDensity2 <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	const double nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(volumeStart));
	const double totalFarPlane = std::max(nearPlane + 1.0, static_cast<double>(settings.volumetricFogDistance));
	const double nearEndDepth = std::min(
		std::max(static_cast<double>(std::max(settings.volumetricNearGridDistance, 0.0f)), nearPlane + 1.0),
		totalFarPlane);
	const bool farGridEnabled = nearEndDepth + 1.0 < totalFarPlane;

	auto* conservativeDepthShader = GetConservativeDepthCS();
	auto* materialSetupShader = GetMaterialSetupCS();
	auto* lightScatteringShader = GetLightScatteringCS();
	auto* integrationShader = GetIntegrationCS();
	if (!conservativeDepthShader || !materialSetupShader || !lightScatteringShader || !integrationShader) {
		ReleaseVolumetricResources();
		return;
	}
	if (farGridEnabled && (!GetFarConservativeDepthCS() || !GetFarMaterialSetupCS() || !GetFarLightScatteringCS() || !GetFarIntegrationCS())) {
		ReleaseVolumetricResources();
		return;
	}
	EnsureVolumetricResources();
	if (lastPrepassFrame == UINT32_MAX || !CanReuseFogHistory(frameSettings, previousFogSettings)) {
		hasLightScatteringHistory = false;
		hasLightScatteringFarHistory = false;
	}
	previousFogSettings = frameSettings;

	ID3D11ShaderResourceView* directionalShadowLightData = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool hasLocalLightData =
		lightLimitFix.loaded &&
		lightLimitFix.lights &&
		lightLimitFix.lightIndexList &&
		lightLimitFix.lightGrid;
	auto* depthSrv = Util::GetCurrentSceneDepthSRV(true);
	auto& ibl = globals::features::ibl;
	auto& skylighting = globals::features::skylighting;
	const bool hasIBL = ibl.loaded &&
	                    ibl.settings.EnableIBL != 0 &&
	                    !ibl.IsDisabledForCurrentScene() &&
	                    ibl.envIBLTexture &&
	                    ibl.skyIBLTexture;
	const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

	const bool temporalReprojection = Util::GetTemporal();
	const bool temporalHistoryValid =
		temporalReprojection &&
		hasLightScatteringHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;
	const bool temporalHistoryValidFar =
		temporalReprojection &&
		hasLightScatteringFarHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;

	auto computeGridZParams = [](double a_nearPlane, double a_farPlane, uint32_t a_gridZ, float a_distributionScale) {
		const double nearWithOffset = a_nearPlane + 0.095 * 100.0;
		const double depthDistributionScale = std::max(static_cast<double>(a_distributionScale), static_cast<double>(a_gridZ) / 120.0);
		const double farExp = std::exp2(std::min(static_cast<double>(a_gridZ) / depthDistributionScale, 120.0));
		const double gridZOffset = (a_farPlane - nearWithOffset * farExp) / (a_farPlane - nearWithOffset);
		const double gridZScale = (1.0 - gridZOffset) / nearWithOffset;
		return DirectX::XMFLOAT4{
			static_cast<float>(gridZScale),
			static_cast<float>(gridZOffset),
			static_cast<float>(depthDistributionScale),
			0.0f
		};
	};

	const float nearFadeInDistanceInv = !settings.useVanillaFogSettings && settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f;

	VolumetricFogCB cb{};
	cb.gridSizeAndFlags = {
		currentGridSize.x,
		currentGridSize.y,
		currentGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValid && hasConservativeDepthHistory ? 16u : 0u) |
			(hasLocalLightData ? 32u : 0u)
	};
	cb.invGridSizeAndNearFade = {
		1.0f / static_cast<float>(currentGridSize.x),
		1.0f / static_cast<float>(currentGridSize.y),
		1.0f / static_cast<float>(currentGridSize.z),
		nearFadeInDistanceInv
	};
	cb.gridZParams = computeGridZParams(nearPlane, nearEndDepth, currentGridSize.z, settings.volumetricDepthDistributionScale);

	cb.farGridSizeAndFlags = {
		currentFarGridSize.x,
		currentFarGridSize.y,
		currentFarGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValidFar && hasConservativeDepthFarHistory ? 16u : 0u)
	};
	cb.farInvGridSizeAndNearFade = {
		1.0f / static_cast<float>(currentFarGridSize.x),
		1.0f / static_cast<float>(currentFarGridSize.y),
		1.0f / static_cast<float>(currentFarGridSize.z),
		nearFadeInDistanceInv
	};
	cb.farGridZParams = computeGridZParams(nearEndDepth, totalFarPlane, currentFarGridSize.z, settings.volumetricDepthDistributionScale);
	cb.farRange = { static_cast<float>(nearEndDepth), static_cast<float>(totalFarPlane), 0.0f, 0.0f };

	cb.clipToWorld = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Invert();

	for (uint32_t i = 0; i < std::size(cb.frameJitterOffsets); i++) {
		const uint32_t temporalFrame = (globals::state->frameCount - i) & 1023u;
		cb.frameJitterOffsets[i] = {
			temporalReprojection ? Halton(temporalFrame, 2) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 3) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 5) : 0.5f,
			0.0f
		};
	}
	cb.historyParameters = {
		temporalHistoryValid ? std::clamp(settings.volumetricHistoryWeight, 0.0f, 0.99f) : 0.0f,
		static_cast<float>(std::clamp(settings.volumetricHistoryMissSampleCount, 1u, 16u)),
		temporalHistoryValidFar ? std::clamp(settings.volumetricHistoryWeight, 0.0f, 0.99f) : 0.0f,
		0.0f
	};
	cb.jitterParameters = {
		temporalReprojection ? std::max(settings.volumetricSampleJitterMultiplier, 0.0f) : 0.0f,
		static_cast<float>(globals::state->frameCount % 8u),
		0.0f,
		0.0f
	};
	volumetricFogCB->Update(cb);

	auto context = globals::d3d::context;
	ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbuffers);

	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);

	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShaderResources(17, 1, &depthSrv);
	ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.texProbeArray->srv.get() : nullptr;
	ID3D11ShaderResourceView* iblSrvs[2]{
		hasIBL ? ibl.envIBLTexture->srv.get() : nullptr,
		hasIBL ? ibl.skyIBLTexture->srv.get() : nullptr
	};
	context->CSSetShaderResources(50, 1, &skylightingSrv);
	context->CSSetShaderResources(76, 2, iblSrvs);

	struct VolumetricPassDesc
	{
		DirectX::XMUINT4 gridSize;
		Texture3D* vBuffer;
		Texture2D* conservativeDepth;
		Texture2D* conservativeDepthHistory;  // may be null
		Texture3D* scattering;
		Texture3D* scatteringHistory;  // may be null
		Texture3D* integrated;
		ID3D11ComputeShader* materialSetupCS;
		ID3D11ComputeShader* conservativeDepthCS;
		ID3D11ComputeShader* lightScatteringCS;
		ID3D11ComputeShader* integrationCS;
		bool hasPrevConservativeDepth;
		const char* name;
	};

	auto runVolumetricPass = [&](const VolumetricPassDesc& p) {
		const uint32_t groupX = (p.gridSize.x + 7) / 8;
		const uint32_t groupY = (p.gridSize.y + 7) / 8;
		const uint32_t groupZ = (p.gridSize.z + 3) / 4;

		if (depthSrv) {
			ID3D11UnorderedAccessView* uavs[1]{ p.conservativeDepth->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.conservativeDepthCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11UnorderedAccessView* uavs[1]{ p.vBuffer->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.materialSetupCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[5]{
				p.vBuffer->srv.get(),
				directionalShadowMap.get(),
				p.scatteringHistory ? p.scatteringHistory->srv.get() : nullptr,
				p.conservativeDepth->srv.get(),
				p.hasPrevConservativeDepth && p.conservativeDepthHistory ? p.conservativeDepthHistory->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* localLightSrvs[3]{
				hasLocalLightData ? lightLimitFix.lights->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightIndexList->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightGrid->srv.get() : nullptr
			};
			ID3D11UnorderedAccessView* uavs[1]{ p.scattering->uav.get() };
			context->CSSetShaderResources(0, 5, srvs);
			context->CSSetShaderResources(35, 3, localLightSrvs);
			context->CSSetShaderResources(98, 1, &directionalShadowLightData);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.lightScatteringCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[1]{ p.scattering->srv.get() };
			ID3D11UnorderedAccessView* uavs[1]{ p.integrated->uav.get() };
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.integrationCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
		}
	};

	runVolumetricPass({ currentGridSize,
		vBufferA.get(),
		conservativeDepth.get(),
		temporalHistoryValid && hasConservativeDepthHistory ? conservativeDepthHistory.get() : nullptr,
		lightScattering.get(),
		temporalHistoryValid ? lightScatteringHistory.get() : nullptr,
		integratedLightScattering.get(),
		materialSetupShader,
		conservativeDepthShader,
		lightScatteringShader,
		integrationShader,
		temporalHistoryValid && hasConservativeDepthHistory,
		"ExponentialHeightFog::NearVolume" });

	if (farGridEnabled) {
		runVolumetricPass({ currentFarGridSize,
			vBufferAFar.get(),
			conservativeDepthFar.get(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory ? conservativeDepthFarHistory.get() : nullptr,
			lightScatteringFar.get(),
			temporalHistoryValidFar ? lightScatteringFarHistory.get() : nullptr,
			integratedLightScatteringFar.get(),
			GetFarMaterialSetupCS(),
			GetFarConservativeDepthCS(),
			GetFarLightScatteringCS(),
			GetFarIntegrationCS(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory,
			"ExponentialHeightFog::FarVolume" });
	} else {
		const float clearValue[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		context->ClearUnorderedAccessViewFloat(integratedLightScatteringFar->uav.get(), clearValue);
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	ID3D11ShaderResourceView* nullSrvs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11ShaderResourceView* nullDepthSrv[1]{ nullptr };
	ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	ID3D11Buffer* nullCb[1]{ nullptr };
	context->CSSetShaderResources(0, 5, nullSrvs);
	context->CSSetShaderResources(17, 1, nullDepthSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(50, 1, nullDepthSrv);
	context->CSSetShaderResources(76, 2, nullSrvs);
	context->CSSetShaderResources(98, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (temporalReprojection) {
		context->CopyResource(lightScatteringHistory->resource.get(), lightScattering->resource.get());
		hasLightScatteringHistory = true;
		if (depthSrv) {
			context->CopyResource(conservativeDepthHistory->resource.get(), conservativeDepth->resource.get());
			hasConservativeDepthHistory = true;
		} else {
			hasConservativeDepthHistory = false;
		}
		if (farGridEnabled) {
			context->CopyResource(lightScatteringFarHistory->resource.get(), lightScatteringFar->resource.get());
			hasLightScatteringFarHistory = true;
			if (depthSrv) {
				context->CopyResource(conservativeDepthFarHistory->resource.get(), conservativeDepthFar->resource.get());
				hasConservativeDepthFarHistory = true;
			} else {
				hasConservativeDepthFarHistory = false;
			}
		} else {
			hasLightScatteringFarHistory = false;
			hasConservativeDepthFarHistory = false;
		}
	} else {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	lastPrepassFrame = globals::state->frameCount;
	BindIntegratedLightScattering();
}

void ExponentialHeightFog::RegisterWeatherVariables()
{
	auto* registry = WeatherVariables::GlobalWeatherRegistry::GetSingleton()->GetOrCreateFeatureRegistry(GetShortName());
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"startDistance",
		"Start Distance",
		"Start distance of the fog, from the camera",
		&settings.startDistance,
		0.0f,
		0.0f, 100000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogHeight",
		"Fog Height",
		"Base height of the fog effect",
		&settings.fogHeight,
		0.0f,
		-22000.0f, 22000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogHeightFalloff",
		"Fog Height Falloff",
		"Height density factor controls how the density increases as height decreases",
		&settings.fogHeightFalloff,
		0.2f,
		0.001f, 2.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"fogInscatteringColor",
		"Fog Inscattering Color",
		"Color added to the fog inscattering contribution",
		&settings.fogInscatteringColor,
		float4{ 0.0f, 0.0f, 0.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"originalFogColorAmount",
		"Original Fog Color Amount",
		"Amount of the original fog color added to fog inscattering",
		&settings.originalFogColorAmount,
		1.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogDensity",
		"Fog Density",
		"Overall density of the fog",
		&settings.fogDensity,
		0.02f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogHeight2",
		"Fog Height 2",
		"Base height of the second fog layer",
		&settings.fogHeight2,
		0.0f,
		-22000.0f, 22000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogHeightFalloff2",
		"Fog Height Falloff 2",
		"Height density factor of the second fog layer controlling how the density increases as height decreases",
		&settings.fogHeightFalloff2,
		0.2f,
		0.001f, 2.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogDensity2",
		"Fog Density 2",
		"Overall density of the second fog layer",
		&settings.fogDensity2,
		0.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"directionalInscatteringMultiplier",
		"Directional Inscattering Multiplier",
		"Multiplier for directional light inscattering",
		&settings.directionalInscatteringMultiplier,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"sunlightAttenuationAmount",
		"Sunlight Attenuation Amount",
		"Amount of fog attenuation applied to direct sunlight",
		&settings.sunlightAttenuationAmount,
		1.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"directionalInscatteringAnisotropy",
		"Directional Inscattering Anisotropy",
		"Henyey-Greenstein asymmetry parameter. Positive = forward scattering, 0 = isotropic, negative = back scattering.",
		&settings.directionalInscatteringAnisotropy,
		0.2f,
		-0.99f, 0.99f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"inscatteringTint",
		"Inscattering Cubemap Tint",
		"RGB tint for the inscattering cubemap with alpha for intensity",
		&settings.inscatteringTint,
		float4{ 1.0f, 1.0f, 1.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"respectVanillaFogFade",
		"Apply Vanilla Fade",
		"Apply vanilla fade brightness to exponential height fog",
		(bool*)&settings.respectVanillaFogFade,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"disableVanillaFog",
		"Disable Vanilla Fog",
		"Disables vanilla fog entirely, only exponential height fog is applied",
		(bool*)&settings.disableVanillaFog,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"volumetricFogEnabled",
		"Enable Volumetric Fog",
		"Enables froxel-based volumetric fog for exponential height fog",
		(bool*)&settings.volumetricFogEnabled,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogDistance",
		"Volumetric View Distance",
		"Maximum distance covered by exponential height volumetric fog",
		&settings.volumetricFogDistance,
		60000.0f,
		1000.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricNearGridDistance",
		"Volumetric Near Grid Distance",
		"Distance covered by the full-resolution near volume; a quarter-lattice far volume covers the rest",
		&settings.volumetricNearGridDistance,
		8000.0f,
		256.0f, 50000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogNoiseScale",
		"Volumetric Fog Noise Scale",
		"Spatial frequency of the volumetric fog noise field (0 = disabled)",
		&settings.volumetricFogNoiseScale,
		0.0f,
		0.0f, 0.01f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogNoiseThreshold",
		"Volumetric Fog Noise Threshold",
		"Soft cutoff that carves clumps out of the volumetric fog noise field",
		&settings.volumetricFogNoiseThreshold,
		0.5f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>(
		"volumetricFogNoiseVelocity",
		"Volumetric Fog Noise Velocity",
		"Animation drift of the volumetric fog noise field",
		&settings.volumetricFogNoiseVelocity,
		float3{ 0.0f, 0.0f, 0.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogStartDistance",
		"Volumetric Start Distance",
		"Start distance of volumetric fog from the camera",
		&settings.volumetricFogStartDistance,
		0.0f,
		0.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogNearFadeInDistance",
		"Volumetric Near Fade In Distance",
		"Distance over which volumetric fog fades in near the camera",
		&settings.volumetricFogNearFadeInDistance,
		1000.0f,
		0.0f, 20000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogExtinctionScale",
		"Volumetric Extinction Scale",
		"Scale applied to volumetric fog extinction",
		&settings.volumetricFogExtinctionScale,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricFogScatteringDistribution",
		"Volumetric Scattering Distribution",
		"Henyey-Greenstein scattering distribution for volumetric fog",
		&settings.volumetricFogScatteringDistribution,
		0.2f,
		-0.9f, 0.9f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricDirectionalScatteringIntensity",
		"Volumetric Directional Scattering Intensity",
		"Scale applied to volumetric fog directional light scattering",
		&settings.volumetricDirectionalScatteringIntensity,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"volumetricFogAlbedo",
		"Volumetric Albedo",
		"Volumetric fog albedo color",
		&settings.volumetricFogAlbedo,
		float4{ 1.0f, 1.0f, 1.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"volumetricFogEmissive",
		"Volumetric Emissive",
		"Volumetric fog emissive color",
		&settings.volumetricFogEmissive,
		float4{ 0.0f, 0.0f, 0.0f, 0.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricSkyLightingIntensity",
		"Volumetric Sky Lighting Intensity",
		"Scale applied to volumetric fog sky lighting",
		&settings.volumetricSkyLightingIntensity,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"volumetricLocalLightScatteringIntensity",
		"Volumetric Local Light Scattering Intensity",
		"Scale applied to volumetric fog local light scattering",
		&settings.volumetricLocalLightScatteringIntensity,
		1.0f,
		0.0f, 100.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"vanillaFogStrength",
		"Weather Fog Strength",
		"Scales the fog density derived from the active weather when following vanilla fog",
		&settings.vanillaFogStrength,
		1.25f,
		0.0f, 4.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogLightingInfluence",
		"Weather Lighting Influence",
		"How strongly sun, sky and local lights shape fog that follows vanilla fog",
		&settings.fogLightingInfluence,
		0.35f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeMaxOpacity",
		"Haze Maximum Opacity",
		"Maximum opacity of distance haze added at all heights",
		&settings.distanceHazeMaxOpacity,
		0.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeStartDistance",
		"Haze Start Distance",
		"Horizontal distance from the camera where distance haze begins",
		&settings.distanceHazeStartDistance,
		15000.0f,
		0.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeFadeDistance",
		"Haze Fade Distance",
		"Distance over which distance haze reaches its maximum opacity",
		&settings.distanceHazeFadeDistance,
		60000.0f,
		1.0f, 200000.0f));
}
#undef I18N_KEY_PREFIX
