#include "Common/FlareOcclusion.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

SamplerState LinearSampler : register(s0);

Texture2D<float> SceneDepth : register(t0);

cbuffer SunMaskData : register(b0)
{
	float4 SunDirectionWS;
	float2 SunUV;
};

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

float4 main() : SV_Target
{
	uint width, height;
	SceneDepth.GetDimensions(width, height);

	uint visibleSamples = 0;
	[unroll] for (uint i = 0; i < FlareOcclusion::SampleCount; i++)
	{
		float2 sampleUV = SunUV + FlareOcclusion::GetSampleOffset(i);
		bool outsideFrame = any(sampleUV < 0.0) || any(sampleUV > 1.0);
		visibleSamples += !outsideFrame && SceneDepth.Load(int3(sampleUV * float2(width, height), 0)) >= 1.0;
	}

	float visibility = FlareOcclusion::GetVisibility(visibleSamples);
#if defined(CLOUD_SHADOWS)
	visibility *= saturate(1.0 - CloudShadows::CloudShadowsTexture.SampleLevel(LinearSampler, SunDirectionWS.xyz, 0));
#endif
	return visibility;
}
