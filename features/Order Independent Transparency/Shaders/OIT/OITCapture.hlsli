#ifndef __OIT_CAPTURE__
#define __OIT_CAPTURE__

#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"
#include "OIT/OITCommon.hlsli"

#if OIT == 1
#	include "OIT/FragmentList.hlsli"
#elif OIT == 2
#	include "OIT/AOIT.hlsli"
#elif OIT == 3
#	include "OIT/WBOIT.hlsli"
#endif

#if !defined(OIT_CAPTURE_IGNORE_ALPHA_THRESHOLD)
#	define OIT_CAPTURE_IGNORE_ALPHA_THRESHOLD 0
#endif

struct OITOutput
{
	float4 color;
	float writeDepth;
	bool captured;
#if OIT == 3
	float4 accumFront;
	float4 accumAll;
	float2 revealage;
#endif
};

bool OIT_IsNegligible(float4 layer)
{
	return max(max(layer.x, layer.y), max(layer.z, layer.w)) < OIT_MIN_CONTRIBUTION;
}

OITOutput OIT_Capture(int2 screenAddress, float4 color, float depth)
{
	uint flags = Permutation::ExtraFeatureDescriptor >> OIT_FEATURE_FLAGS_SHIFT;
	uint blend = flags & OIT_FLAGS_BLEND_MODES;
	bool writesDepth = (flags & OIT_FLAGS_DEPTH_WRITE) && color.w >= SharedData::orderIndependentTransparencySettings.WriteDepthThreshold;

	OITOutput output;
	output.color = color;
	output.captured = false;
	output.writeDepth = writesDepth ? depth : OIT_FarDepth();
#if OIT == 3
	output.accumFront = 0.0;
	output.accumAll = 0.0;
	output.revealage = 1.0;
#endif

	[branch] if (!(flags & OIT_FLAGS_DISABLED))
	{
		float alpha = saturate(color.w);
#if !OIT_CAPTURE_IGNORE_ALPHA_THRESHOLD
		float alphaThreshold = SharedData::orderIndependentTransparencySettings.AlphaThreshold;
		alpha = saturate((alpha - alphaThreshold) / (1.0 - alphaThreshold));
#endif

		float4 layer;
		if (blend == OIT_FLAGS_MULTIPLICATIVE_A)
			layer = float4(0.0, 0.0, 0.0, saturate(alpha - OIT_Luminance(color.xyz)));
		else if (blend == OIT_FLAGS_ADDITIVE)
			layer = float4(color.xyz * alpha, 0.0);
		else if (blend == OIT_FLAGS_MULTIPLICATIVE)
			layer = float4(0.0, 0.0, 0.0, 1.0 - saturate(OIT_Luminance(color.xyz)));
		else
			layer = float4(color.xyz * alpha, alpha);

#if OIT == 3
		layer = float4(clamp(layer.xyz, 0.0, WBOIT_MAX_COLOR), saturate(layer.w));
		if (layer.w == 0.0)
			layer.w = saturate(SharedData::orderIndependentTransparencySettings.WBOITAdditiveAlphaScale * sqrt(sqrt(length(layer.xyz) * 0.57735027)));

		if (!writesDepth && OIT_IsNegligible(layer))
			discard;

		float weight = WBOIT_Weight(layer.w, depth);
		bool frontOfWater = OIT_IsNearerOrEqual(depth, OITWaterDepthTexture[screenAddress]);
		output.accumAll = layer * weight;
		output.accumFront = frontOfWater ? output.accumAll : 0.0;
		output.revealage = float2(1.0 - layer.w, frontOfWater ? 1.0 - layer.w : 1.0);
		output.captured = true;
#else
		if (!writesDepth && OIT_IsNegligible(layer))
			discard;

		output.captured = OIT_CaptureImpl(screenAddress, layer, depth);
		if (output.captured)
			output.color = (blend & OIT_FLAGS_MULTIPLICATIVE) ? 1.0 : 0.0;
#endif
	}

	return output;
}

#endif
