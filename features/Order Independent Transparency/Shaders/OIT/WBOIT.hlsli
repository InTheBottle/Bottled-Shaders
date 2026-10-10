#ifndef __WBOIT__
#define __WBOIT__

#include "Common/SharedData.hlsli"
#include "OIT/OITCommon.hlsli"

Texture2D<float> OITWaterDepthTexture : register(t67);

static const float WBOIT_MAX_COLOR = 64.0;

float WBOIT_Weight(float alpha, float depth)
{
	float d = max(SharedData::orderIndependentTransparencySettings.WBOITMinProjectedDistance, OIT_Nearness(depth));
	float w = max(0.01, 3000.0 * d * d * d);
	return clamp(alpha * w, SharedData::orderIndependentTransparencySettings.WBOITWeightMin, SharedData::orderIndependentTransparencySettings.WBOITWeightMax);
}

#endif
