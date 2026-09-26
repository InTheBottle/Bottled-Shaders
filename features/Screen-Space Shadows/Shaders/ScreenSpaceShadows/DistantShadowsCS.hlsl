#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepthTexture : register(t0);
#endif
Texture2D<unorm float2> ContactShadowsTexture : register(t1);
RWTexture2D<unorm float2> OutputTexture : register(u0);

#if defined(TERRAIN_SHADOWS)
SamplerState LinearSampler : register(s0);
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif

cbuffer DistantShadowsCB : register(b1)
{
	float2 RenderSize;
	float2 InvRenderSize;
	float StartDistance;
	float FadeLength;
	float MaxRayLength;
	float Intensity;
	float ThicknessScale;
	uint SampleCount;
	uint UseContactShadows;
	float pad0;
};

static const float MinStepLength = 32.0;
static const float FirstStepDepthScale = 0.004;
static const float DepthBiasScale = 0.002;
static const float TerrainShadowSkipMargin = 64.0;

float GetViewDepth(uint2 pixel)
{
	return SharedData::GetScreenDepth(SceneDepthTexture[pixel]);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(RenderSize)))
		return;

	float depth = SceneDepthTexture[dispatchID.xy];
	if (depth == FrameBuffer::FarPlaneDepth())
		return;

	float viewDepth = SharedData::GetScreenDepth(depth);
	float fade = saturate((viewDepth - StartDistance) / FadeLength);
	if (fade <= 0.0)
		return;

	float firstStep = max(MinStepLength, viewDepth * FirstStepDepthScale);
	if (firstStep >= MaxRayLength)
		return;

	float2 uv = (dispatchID.xy + 0.5) * InvRenderSize;
	float4 positionWS = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
	positionWS.xyz /= positionWS.w;

#if defined(TERRAIN_SHADOWS)
	if (TerrainShadows::GetTerrainShadow(positionWS.xyz + FrameBuffer::CameraPosAdjust.xyz + float3(0.0, 0.0, TerrainShadowSkipMargin), LinearSampler) <= 0.0)
		return;
#endif

	float3 lightDirection = SharedData::DirLightDirection.xyz;
	float noise = Random::InterleavedGradientNoise(dispatchID.xy, SharedData::FrameCount);

	float logGrowth = log2(MaxRayLength / firstStep) / SampleCount;
	float growth = exp2(logGrowth);
	float distanceTravelled = firstStep * exp2(logGrowth * noise);

	float occlusion = 0.0;
	[loop] for (uint i = 0; i < SampleCount; i++, distanceTravelled *= growth)
	{
		float4 sampleCS = mul(FrameBuffer::CameraViewProj, float4(positionWS.xyz + lightDirection * distanceTravelled, 1.0));
		if (sampleCS.w <= 0.0)
			break;

		float2 sampleNDC = sampleCS.xy / sampleCS.w;
		if (any(abs(sampleNDC) >= 1.0))
			break;

		uint2 samplePixel = uint2((sampleNDC * float2(0.5, -0.5) + 0.5) * RenderSize);
		float depthDelta = sampleCS.w - GetViewDepth(samplePixel);

		float bias = max(MinStepLength, sampleCS.w * DepthBiasScale);
		float thickness = max(distanceTravelled * ThicknessScale, 2.0 * bias);

		float sampleOcclusion = saturate((depthDelta - bias) / bias) * saturate((thickness - depthDelta) / (0.25 * thickness));
		occlusion = max(occlusion, sampleOcclusion);

		if (occlusion >= 1.0)
			break;
	}

	float shadow = 1.0 - occlusion * fade * Intensity;
	float2 contactShadows = UseContactShadows ? ContactShadowsTexture[dispatchID.xy] : float2(1.0, 1.0);
	OutputTexture[dispatchID.xy] = contactShadows * shadow;
}
