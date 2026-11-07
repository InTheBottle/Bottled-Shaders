#pragma once

namespace ImageSpaceModifiers
{
	struct Contribution
	{
		RE::ImageSpaceBaseData::Cinematic multiply{};
		RE::ImageSpaceBaseData::Cinematic add{};
		float strength = 0.f;
		RE::ImageSpaceBaseData::Tint tint{};
		float fadeAmount = 0.f;
		RE::NiColor fadeColor{};
		float depthOfField = 0.f;
	};

	struct Frame
	{
		std::vector<Contribution> contributions;
		bool captured = false;
	};

	struct Blend
	{
		float3 cinematic;
		float tint;
		float fade;
		float modifiers;
	};

	struct Result
	{
		float3 cinematic;
		float4 tint;
		float4 fade;
	};

	/** @brief The engine's per-modifier cinematic fold: lerp(value, value * multiply + add, strength). */
	inline float BlendValue(float a_value, float a_multiply, float a_add, float a_strength)
	{
		return a_value - (a_value - (a_value * a_multiply + a_add)) * a_strength;
	}

	void Install();
	void Snapshot(Frame& a_frame);
	bool IsNightEyeActive();
	bool WantsDepthOfField();
	Result Resolve(const RE::ImageSpaceData& a_data, const Frame& a_frame, const Blend& a_blend);
}
