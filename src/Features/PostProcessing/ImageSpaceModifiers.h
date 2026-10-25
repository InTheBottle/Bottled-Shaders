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

	void Install();
	void Snapshot(Frame& a_frame);
	bool IsNightEyeActive();
	bool WantsDepthOfField();
	Result Resolve(const RE::ImageSpaceData& a_data, const Frame& a_frame, const Blend& a_blend);
}
