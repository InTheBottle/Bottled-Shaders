#pragma once

#include <array>
#include <vector>

namespace CameraFXModifiers
{
	enum class Theme : uint8_t
	{
		kGeneric,
		kCold,
		kLowHealth,
		kExhaustion,
		kHunger,
		kPoison,
		kCount
	};

	inline constexpr size_t kThemeCount = static_cast<size_t>(Theme::kCount);

	struct Capture
	{
		RE::FormID formID = 0;
		Theme theme = Theme::kGeneric;
		float weight = 1.0f;
		float strength = 0.0f;
		RE::ImageSpaceBaseData::Cinematic multiply{};
		RE::ImageSpaceBaseData::Cinematic add{};
		RE::ImageSpaceBaseData::Tint tint{};
		std::array<float, RE::ImageSpaceModData::kTotal> mod{};
	};

	struct Frame
	{
		RE::ImageSpaceBaseData::Cinematic base{ 1.0f, 1.0f, 1.0f };
		RE::ImageSpaceBaseData::Tint baseTint{};
		std::vector<Capture> captures;
	};

	struct Blend
	{
		float3 baseCinematic{ 1.0f, 1.0f, 1.0f };
		float baseTint = 1.0f;
		float modifiers = 1.0f;
		float fade = 1.0f;
	};

	struct Generic
	{
		float3 cinematic{ 1.0f, 1.0f, 1.0f };
		float4 tint{};
		float4 fade{};
		float blurRadius = 0.0f;
		float doubleVision = 0.0f;
		float radialStrength = 0.0f;
		float radialRampUp = 0.0f;
		float radialStart = 0.0f;
		float radialRampDown = 0.0f;
		float radialDownStart = 0.0f;
		float2 radialCenter{ 0.5f, 0.5f };
		float dofStrength = 0.0f;
		float dofDistance = 0.0f;
		float dofRange = 0.0f;
		float dofMode = 0.0f;
		bool active = false;
	};

	struct Resolved
	{
		Generic generic;
		std::array<float, kThemeCount> themes{};
	};

	void ResolveForms();
	void BeginFrame(const RE::ImageSpaceBaseData* a_base);
	void Record(const RE::TESImageSpaceModifier* a_form, const RE::ImageSpaceBaseData& a_multiply, const RE::ImageSpaceBaseData& a_add, const RE::ImageSpaceModData& a_mod, float a_strength);
	void Snapshot(Frame& a_frame);
	Resolved Resolve(const Frame& a_frame, const std::array<bool, kThemeCount>& a_themesEnabled, const Blend& a_blend);
	float GetActivity(const Capture& a_capture);
	const char* GetThemeName(Theme a_theme);
}
