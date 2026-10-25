#include "ImageSpaceModifiers.h"

#include "Features/NightEye.h"
#include "Utils/ColorSpace.h"
#include "Utils/VersionedRelocation.h"

#include <algorithm>
#include <mutex>

namespace ImageSpaceModifiers
{
	namespace
	{
		std::mutex lock;
		Frame pending;
		bool nightEyeActive = false;
		bool installed = false;
		const RE::TESImageSpaceModifier* currentModifier = nullptr;

		struct ImageSpaceModifierInstanceForm_Apply
		{
			static void thunk(RE::ImageSpaceModifierInstanceForm* a_instance)
			{
				currentModifier = a_instance ? a_instance->imod : nullptr;
				func(a_instance);
				currentModifier = nullptr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct TESImageSpaceModifier_Apply_ApplyModifier
		{
			static void thunk(RE::ImageSpaceManager* a_manager, const RE::ImageSpaceBaseData* a_multiply, const RE::ImageSpaceBaseData* a_add, const RE::ImageSpaceModData* a_mod, float a_strength)
			{
				auto& nightEye = globals::features::nightEye;
				if (nightEye.ReplacesImageSpaceModifier(currentModifier)) {
					if (a_strength > 0.f && nightEye.IsVisionModifier(currentModifier)) {
						const std::scoped_lock guard(lock);
						nightEyeActive = true;
					}
					return;
				}
				func(a_manager, a_multiply, a_add, a_mod, a_strength);
				if (!a_multiply || !a_add || !a_mod)
					return;
				const auto& mod = a_mod->data;
				const std::scoped_lock guard(lock);
				pending.contributions.push_back({ .multiply = a_multiply->cinematic,
					.add = a_add->cinematic,
					.strength = a_strength,
					.tint = a_multiply->tint,
					.fadeAmount = mod[RE::ImageSpaceModData::kFadeAmount],
					.fadeColor = { mod[RE::ImageSpaceModData::kFadeR], mod[RE::ImageSpaceModData::kFadeG], mod[RE::ImageSpaceModData::kFadeB] },
					.depthOfField = mod[RE::ImageSpaceModData::kDOFStrength] });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ImageSpaceManager_ResetModifiers_SetBaseData
		{
			static void thunk(RE::ImageSpaceData* a_data, const RE::ImageSpaceBaseData* a_base)
			{
				func(a_data, a_base);
				const std::scoped_lock guard(lock);
				pending.contributions.clear();
				pending.captured = true;
				nightEyeActive = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		float BlendValue(float a_value, float a_multiply, float a_add, float a_strength)
		{
			return a_value - (a_value - (a_value * a_multiply + a_add)) * a_strength;
		}
	}

	void Install()
	{
		if (installed || REL::Module::IsVR())
			return;
		installed = true;

		const auto applySite = REL::RelocationID(18257, 18651).address() + REL::Relocate(0x638, 0x63C);
		const auto resetSite = REL::RelocationID(99036, 105687).address() + REL::Relocate(0x57, 0x57);
		if (!Util::VersionedRelocation::IsCallTo(applySite, REL::RelocationID(99037, 105688).address()) ||
			!Util::VersionedRelocation::IsCallTo(resetSite, REL::RelocationID(100400, 107114).address())) {
			logger::warn("[PostProcessing] Imagespace modifier call sites differ from the expected code, game modifiers use the engine values");
			return;
		}

		constexpr std::size_t kStubBytes = 2 * 14;
		if (SKSE::GetTrampoline().free_size() < kStubBytes) {
			logger::warn("[PostProcessing] Imagespace modifier hooks skipped: trampoline has {} bytes free, needs {}", SKSE::GetTrampoline().free_size(), kStubBytes);
			return;
		}

		stl::write_vfunc<0x26, ImageSpaceModifierInstanceForm_Apply>(RE::VTABLE_ImageSpaceModifierInstanceForm[0]);
		stl::write_thunk_call<TESImageSpaceModifier_Apply_ApplyModifier>(applySite);
		stl::write_thunk_call<ImageSpaceManager_ResetModifiers_SetBaseData>(resetSite);
		logger::info("[PostProcessing] Hooked imagespace modifier accumulation");
	}

	void Snapshot(Frame& a_frame)
	{
		const std::scoped_lock guard(lock);
		a_frame.contributions.assign(pending.contributions.begin(), pending.contributions.end());
		a_frame.captured = pending.captured;
	}

	bool IsNightEyeActive()
	{
		const std::scoped_lock guard(lock);
		return nightEyeActive;
	}

	bool WantsDepthOfField()
	{
		const std::scoped_lock guard(lock);
		return std::ranges::any_of(pending.contributions, [](const Contribution& a_contribution) { return a_contribution.depthOfField > 0.f; });
	}

	Result Resolve(const RE::ImageSpaceData& a_data, const Frame& a_frame, const Blend& a_blend)
	{
		const auto& base = a_data.baseData;
		const auto& mod = a_data.modData.data;
		const float3 start{
			std::lerp(1.f, base.cinematic.saturation, a_blend.cinematic.x),
			std::lerp(1.f, base.cinematic.brightness, a_blend.cinematic.y),
			std::lerp(1.f, base.cinematic.contrast, a_blend.cinematic.z)
		};
		const float baseTint = base.tint.amount * a_blend.tint;
		const float3 baseTintColor{ base.tint.color.red, base.tint.color.green, base.tint.color.blue };

		Result result{
			.cinematic = start,
			.tint = { baseTintColor.x, baseTintColor.y, baseTintColor.z, baseTint },
			.fade = { mod[RE::ImageSpaceModData::kFadeR], mod[RE::ImageSpaceModData::kFadeG], mod[RE::ImageSpaceModData::kFadeB], mod[RE::ImageSpaceModData::kFadeAmount] * a_blend.fade }
		};
		if (!a_frame.captured)
			return result;

		float3 cinematic = start;
		float tintTotal = baseTint;
		float tintMax = baseTint;
		float3 tintWeighted = baseTintColor * baseTint;
		float fadeTotal = 0.f;
		float fadeMax = 0.f;
		float3 fadeWeighted{};
		for (const auto& contribution : a_frame.contributions) {
			const float strength = contribution.strength * a_blend.modifiers;
			cinematic.x = BlendValue(cinematic.x, contribution.multiply.saturation, contribution.add.saturation, strength);
			cinematic.y = BlendValue(cinematic.y, contribution.multiply.brightness, contribution.add.brightness, strength);
			cinematic.z = BlendValue(cinematic.z, contribution.multiply.contrast, contribution.add.contrast, strength);

			const float tint = contribution.tint.amount * a_blend.modifiers;
			tintTotal += tint;
			tintMax = std::max(tintMax, tint);
			tintWeighted += float3{ contribution.tint.color.red, contribution.tint.color.green, contribution.tint.color.blue } * tint;

			fadeTotal += contribution.fadeAmount;
			fadeMax = std::max(fadeMax, contribution.fadeAmount);
			fadeWeighted += float3{ contribution.fadeColor.red, contribution.fadeColor.green, contribution.fadeColor.blue } * contribution.fadeAmount;
		}

		const float brightness = std::max(cinematic.y, 0.f);
		result.cinematic = {
			cinematic.x,
			start.y > 0.f ? start.y * std::pow(brightness / start.y, Util::ColorSpace::GAME_GAMMA) : brightness,
			cinematic.z
		};
		if (tintTotal > 0.f) {
			const float3 color = tintWeighted / tintTotal;
			result.tint = { color.x, color.y, color.z, tintMax };
		}
		if (fadeTotal > 0.f) {
			const float3 color = fadeWeighted / fadeTotal;
			result.fade = { color.x, color.y, color.z, fadeMax * a_blend.fade };
		} else {
			result.fade = {};
		}
		return result;
	}
}
