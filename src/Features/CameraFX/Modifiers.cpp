#include "Modifiers.h"

#include "Features/PostProcessing/ImageSpaceModifiers.h"
#include "I18n/I18n.h"
#include "Utils/MathUtils.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace CameraFXModifiers
{
	namespace
	{
		struct Entry
		{
			RE::FormID localID;
			std::string_view plugin;
			Theme theme;
			float weight;
		};

		constexpr std::string_view kSkyrim = "Skyrim.esm";
		constexpr std::string_view kSurvival = "ccQDRSSE001-SurvivalMode.esl";

		constexpr Entry kEntries[] = {
			{ 0x10C445, kSkyrim, Theme::kLowHealth, 1.0f },
			{ 0x10E3D2, kSkyrim, Theme::kPoison, 1.0f },
			{ 0x10E3D3, kSkyrim, Theme::kPoison, 0.6f },
			{ 0x0E64A4, kSkyrim, Theme::kPoison, 1.0f },
			{ 0x000998, kSurvival, Theme::kCold, 0.5f },
			{ 0x00099E, kSurvival, Theme::kCold, 0.4f },
			{ 0x0009A0, kSurvival, Theme::kCold, 0.4f },
			{ 0x000999, kSurvival, Theme::kCold, 0.4f },
			{ 0x0008A0, kSurvival, Theme::kCold, 0.7f },
			{ 0x0008A1, kSurvival, Theme::kCold, 0.7f },
			{ 0x00099F, kSurvival, Theme::kCold, 0.7f },
			{ 0x00099B, kSurvival, Theme::kCold, 1.0f },
			{ 0x00099C, kSurvival, Theme::kCold, 1.0f },
			{ 0x00099D, kSurvival, Theme::kCold, 1.0f },
			{ 0x00099A, kSurvival, Theme::kExhaustion, 0.3f },
			{ 0x0009A1, kSurvival, Theme::kExhaustion, 0.4f },
			{ 0x0009A2, kSurvival, Theme::kExhaustion, 0.5f },
			{ 0x0009A3, kSurvival, Theme::kExhaustion, 0.6f },
			{ 0x000A15, kSurvival, Theme::kExhaustion, 0.7f },
			{ 0x000A17, kSurvival, Theme::kExhaustion, 0.7f },
			{ 0x000A14, kSurvival, Theme::kExhaustion, 0.7f },
			{ 0x00089F, kSurvival, Theme::kExhaustion, 1.0f },
			{ 0x00089D, kSurvival, Theme::kExhaustion, 1.0f },
			{ 0x000A16, kSurvival, Theme::kExhaustion, 1.0f },
			{ 0x000A13, kSurvival, Theme::kHunger, 0.3f },
			{ 0x000A10, kSurvival, Theme::kHunger, 0.5f },
			{ 0x000A11, kSurvival, Theme::kHunger, 0.7f },
			{ 0x00089E, kSurvival, Theme::kHunger, 1.0f },
			{ 0x00089C, kSurvival, Theme::kHunger, 1.0f },
			{ 0x000A12, kSurvival, Theme::kHunger, 1.0f },
		};

		constexpr float kActivityReference = 0.25f;
		constexpr float kBlurReference = 8.0f;
		constexpr float kRatioEpsilon = 1e-4f;

		std::mutex lock;
		Frame pending;
		std::unordered_map<RE::FormID, std::pair<Theme, float>> classification;

		float Ratio(float a_final, float a_base)
		{
			return std::abs(a_base) > kRatioEpsilon ? a_final / a_base : 1.0f;
		}
	}

	void ResolveForms()
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler)
			return;

		std::unordered_map<RE::FormID, std::pair<Theme, float>> resolved;
		for (const auto& entry : kEntries) {
			const auto formID = dataHandler->LookupFormID(entry.localID, entry.plugin);
			if (formID)
				resolved[formID] = { entry.theme, entry.weight };
		}

		const std::scoped_lock guard(lock);
		classification = std::move(resolved);
		logger::info("[CameraFX] Recognised {} themed imagespace modifiers", classification.size());
	}

	void BeginFrame(const RE::ImageSpaceBaseData* a_base)
	{
		const std::scoped_lock guard(lock);
		pending.captures.clear();
		pending.base = a_base ? a_base->cinematic : RE::ImageSpaceBaseData::Cinematic{ 1.0f, 1.0f, 1.0f };
		pending.baseTint = a_base ? a_base->tint : RE::ImageSpaceBaseData::Tint{};
	}

	void Record(const RE::TESImageSpaceModifier* a_form, const RE::ImageSpaceBaseData& a_multiply, const RE::ImageSpaceBaseData& a_add, const RE::ImageSpaceModData& a_mod, float a_strength)
	{
		Capture capture;
		capture.formID = a_form ? a_form->GetFormID() : 0;
		capture.strength = a_strength;
		capture.multiply = a_multiply.cinematic;
		capture.add = a_add.cinematic;
		capture.tint = a_multiply.tint;
		std::copy(std::begin(a_mod.data), std::end(a_mod.data), capture.mod.begin());

		const std::scoped_lock guard(lock);
		if (const auto it = classification.find(capture.formID); it != classification.end()) {
			capture.theme = it->second.first;
			capture.weight = it->second.second;
		}
		pending.captures.push_back(capture);
	}

	void Snapshot(Frame& a_frame)
	{
		const std::scoped_lock guard(lock);
		a_frame.base = pending.base;
		a_frame.baseTint = pending.baseTint;
		a_frame.captures.assign(pending.captures.begin(), pending.captures.end());
	}

	float GetActivity(const Capture& a_capture)
	{
		auto cinematic = [&](float a_multiply, float a_add) { return std::abs(a_multiply + a_add - 1.0f) * a_capture.strength; };
		const auto& mod = a_capture.mod;
		const float magnitude = std::max({ cinematic(a_capture.multiply.saturation, a_capture.add.saturation),
			cinematic(a_capture.multiply.brightness, a_capture.add.brightness),
			cinematic(a_capture.multiply.contrast, a_capture.add.contrast),
			a_capture.tint.amount,
			mod[RE::ImageSpaceModData::kFadeAmount],
			mod[RE::ImageSpaceModData::kBlurRadius] / kBlurReference,
			mod[RE::ImageSpaceModData::kDoubleVisionStrength] / kBlurReference,
			mod[RE::ImageSpaceModData::kRadialBlurStrength] });
		return std::clamp(magnitude / kActivityReference, 0.0f, 1.0f);
	}

	Resolved Resolve(const Frame& a_frame, const std::array<bool, kThemeCount>& a_themesEnabled, const Blend& a_blend)
	{
		Resolved result;
		auto& generic = result.generic;

		const auto& base = a_frame.base;
		const float3 start{ std::lerp(1.0f, base.saturation, a_blend.baseCinematic.x), std::lerp(1.0f, base.brightness, a_blend.baseCinematic.y),
			std::lerp(1.0f, base.contrast, a_blend.baseCinematic.z) };
		float3 cinematic = start;
		const float baseTint = Util::ClampFinite(a_frame.baseTint.amount * a_blend.baseTint, 0.0f, 0.0f, 1.0f);
		const float3 baseTintColor{ a_frame.baseTint.color.red, a_frame.baseTint.color.green, a_frame.baseTint.color.blue };
		float tintTotal = baseTint;
		float tintMax = baseTint;
		float3 tintWeighted = baseTintColor * baseTint;
		float fadeTotal = 0.0f;
		float fadeMax = 0.0f;
		float3 fadeWeighted{};

		using Mod = RE::ImageSpaceModData;
		for (const auto& capture : a_frame.captures) {
			const auto themeIndex = static_cast<size_t>(capture.theme);
			if (capture.theme != Theme::kGeneric && a_themesEnabled[themeIndex]) {
				result.themes[themeIndex] = std::max(result.themes[themeIndex], capture.weight * Util::ClampFinite(capture.strength, 0.0f, 0.0f, 1.0f));
				continue;
			}

			const float strength = capture.strength * a_blend.modifiers;
			cinematic.x = ImageSpaceModifiers::BlendValue(cinematic.x, capture.multiply.saturation, capture.add.saturation, strength);
			cinematic.y = ImageSpaceModifiers::BlendValue(cinematic.y, capture.multiply.brightness, capture.add.brightness, strength);
			cinematic.z = ImageSpaceModifiers::BlendValue(cinematic.z, capture.multiply.contrast, capture.add.contrast, strength);

			const float tint = std::max(capture.tint.amount, 0.0f) * a_blend.modifiers;
			tintTotal += tint;
			tintMax = std::max(tintMax, tint);
			tintWeighted += float3{ capture.tint.color.red, capture.tint.color.green, capture.tint.color.blue } * tint;

			const auto& mod = capture.mod;
			const float fade = std::max(mod[Mod::kFadeAmount], 0.0f) * a_blend.fade;
			fadeTotal += fade;
			fadeMax = std::max(fadeMax, fade);
			fadeWeighted += float3{ mod[Mod::kFadeR], mod[Mod::kFadeG], mod[Mod::kFadeB] } * fade;

			generic.blurRadius = std::max(generic.blurRadius, mod[Mod::kBlurRadius]);
			generic.doubleVision = std::max(generic.doubleVision, mod[Mod::kDoubleVisionStrength]);
			if (mod[Mod::kRadialBlurStrength] > generic.radialStrength) {
				generic.radialStrength = mod[Mod::kRadialBlurStrength];
				generic.radialRampUp = mod[Mod::kRadialBlurRampup];
				generic.radialStart = mod[Mod::kRadialBlurStart];
				generic.radialRampDown = mod[Mod::kRadialBlurRampdown];
				generic.radialDownStart = mod[Mod::kRadialBlurDownStart];
				generic.radialCenter = { mod[Mod::kRadialBlurCenterX], mod[Mod::kRadialBlurCenterY] };
			}
			if (mod[Mod::kDOFStrength] > generic.dofStrength) {
				generic.dofStrength = mod[Mod::kDOFStrength];
				generic.dofDistance = mod[Mod::kDOFDistance];
				generic.dofRange = mod[Mod::kDOFRange];
				generic.dofMode = mod[Mod::kDOFMode];
			}
		}

		constexpr float kIdentityEpsilon = 1e-3f;
		generic.cinematic = { Ratio(cinematic.x, start.x), Ratio(cinematic.y, start.y), Ratio(cinematic.z, start.z) };
		const float tintAmount = std::min(tintMax, 1.0f);
		if (tintAmount > baseTint + kIdentityEpsilon && tintTotal > 0.0f) {
			const float3 merged = tintWeighted / tintTotal;
			const float residual = std::clamp((tintAmount - baseTint) / std::max(1.0f - baseTint, kIdentityEpsilon), 0.0f, 1.0f);
			const float3 color = (merged * tintAmount - baseTintColor * (baseTint * (1.0f - residual))) / std::max(residual, kIdentityEpsilon);
			generic.tint = { std::max(color.x, 0.0f), std::max(color.y, 0.0f), std::max(color.z, 0.0f), residual };
		}
		if (fadeTotal > 0.0f) {
			const float3 color = fadeWeighted / fadeTotal;
			generic.fade = { color.x, color.y, color.z, std::min(fadeMax, 1.0f) };
		}

		generic.active = std::abs(generic.cinematic.x - 1.0f) > kIdentityEpsilon || std::abs(generic.cinematic.y - 1.0f) > kIdentityEpsilon ||
		                 std::abs(generic.cinematic.z - 1.0f) > kIdentityEpsilon || generic.tint.w > kIdentityEpsilon || generic.fade.w > kIdentityEpsilon ||
		                 generic.blurRadius > 0.01f || generic.doubleVision > 0.01f || generic.radialStrength > kIdentityEpsilon || generic.dofStrength > kIdentityEpsilon;
		return result;
	}

	const char* GetThemeName(Theme a_theme)
	{
		switch (a_theme) {
		case Theme::kCold:
			return T("feature.camera_fx.theme_cold", "Cold");
		case Theme::kLowHealth:
			return T("feature.camera_fx.theme_low_health", "Low Health");
		case Theme::kExhaustion:
			return T("feature.camera_fx.theme_exhaustion", "Exhaustion");
		case Theme::kHunger:
			return T("feature.camera_fx.theme_hunger", "Hunger");
		case Theme::kPoison:
			return T("feature.camera_fx.theme_poison", "Poison");
		default:
			return T("feature.camera_fx.theme_generic", "Generic");
		}
	}
}
