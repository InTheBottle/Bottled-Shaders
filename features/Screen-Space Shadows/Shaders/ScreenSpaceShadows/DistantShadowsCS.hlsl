#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepthTexture : register(t0);
#endif
Texture2D<unorm float2> ContactShadowsTexture : register(t1);
Texture2D<float2> HalfOcclusionTexture : register(t2);
RWTexture2D<unorm float2> OutputTexture : register(u0);
RWTexture2D<float2> HalfOcclusionRW : register(u1);

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
	uint2 HalfSize;
	float2 pad1;
};

static const float MinStepLength = 32.0;
static const float FirstStepDepthScale = 0.004;
static const float DepthBiasScale = 0.002;
static const float TerrainShadowSkipMargin = 64.0;
static const float UpsampleDepthTolerance = 0.05;
static const float HalfDepthScale = 1.0 / 1024.0;
static const float HalfDepthMax = 65504.0;

float GetViewDepth(uint2 pixel)
{
	return SharedData::GetScreenDepth(SceneDepthTexture[pixel]);
}

float TraceOcclusion(uint2 pixel, uint2 noisePixel, out float viewDepth)
{
	float occlusion = 0.0;

	float depth = SceneDepthTexture[pixel];
	viewDepth = SharedData::GetScreenDepth(depth);
	float firstStep = max(MinStepLength, viewDepth * FirstStepDepthScale);
	bool trace = depth != FrameBuffer::FarPlaneDepth() && viewDepth > StartDistance && firstStep < MaxRayLength;

	float3 positionWS = 0.0;
	[branch] if (trace)
	{
		float2 uv = (pixel + 0.5) * InvRenderSize;
		float4 unprojected = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
		positionWS = unprojected.xyz / unprojected.w;

#if defined(TERRAIN_SHADOWS)
		if (TerrainShadows::GetTerrainShadow(positionWS + FrameBuffer::CameraPosAdjust.xyz + float3(0.0, 0.0, TerrainShadowSkipMargin), LinearSampler) <= 0.0) {
			occlusion = 1.0;
			trace = false;
		}
#endif
	}

	[branch] if (trace)
	{
		float4 originCS = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
		float4 directionCS = mul(FrameBuffer::CameraViewProj, float4(SharedData::DirLightDirection.xyz, 0.0));
		float noise = Random::InterleavedGradientNoise(noisePixel, SharedData::FrameCount);

		float logGrowth = log2(MaxRayLength / firstStep) / SampleCount;
		float growth = exp2(logGrowth);
		float distanceTravelled = firstStep * exp2(logGrowth * noise);

		[loop] for (uint i = 0; i < SampleCount; i++, distanceTravelled *= growth)
		{
			float4 sampleCS = originCS + directionCS * distanceTravelled;
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
	}

	return occlusion;
}

[numthreads(8, 8, 1)] void TraceCS(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= HalfSize))
		return;

	uint2 pixel = min(dispatchID.xy * 2, uint2(RenderSize) - 1);
	float viewDepth;
	float occlusion = TraceOcclusion(pixel, dispatchID.xy, viewDepth);
	HalfOcclusionRW[dispatchID.xy] = float2(occlusion, min(viewDepth * HalfDepthScale, HalfDepthMax));
}

float UpsampleOcclusion(uint2 pixel, float viewDepth)
{
	int2 base = int2(pixel >> 1);
	int2 maxHalf = int2(HalfSize) - 1;
	float2 subPixel = float2(pixel & 1) * 0.5;
	float scaledDepth = viewDepth * HalfDepthScale;
	float rcpTolerance = rcp(max(scaledDepth * UpsampleDepthTolerance, MinStepLength * HalfDepthScale));

	float weightSum = 0.0;
	float occlusionSum = 0.0;
	float closestDelta = 3.402823466e+38;
	float closestOcclusion = 0.0;

	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 offset = int2(i & 1, i >> 1);
		int2 halfPixel = min(base + offset, maxHalf);
		float2 halfSample = HalfOcclusionTexture[halfPixel];
		float sampleOcclusion = halfSample.x;
		float depthDelta = abs(halfSample.y - scaledDepth);

		float2 bilinear = lerp(1.0 - subPixel, subPixel, float2(offset));
		float weight = bilinear.x * bilinear.y * exp2(-depthDelta * rcpTolerance);
		weightSum += weight;
		occlusionSum += weight * sampleOcclusion;

		if (bilinear.x * bilinear.y > 0.0 && depthDelta < closestDelta) {
			closestDelta = depthDelta;
			closestOcclusion = sampleOcclusion;
		}
	}

	return weightSum > 1e-4 ? occlusionSum / weightSum : closestOcclusion;
}

[numthreads(8, 8, 1)] void ResolveCS(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(RenderSize)))
		return;

	float depth = SceneDepthTexture[dispatchID.xy];
	if (depth == FrameBuffer::FarPlaneDepth())
		return;

	float viewDepth = SharedData::GetScreenDepth(depth);
	float fade = saturate((viewDepth - StartDistance) / FadeLength);
	if (fade <= 0.0)
		return;

	float shadow = 1.0 - UpsampleOcclusion(dispatchID.xy, viewDepth) * fade * Intensity;
	float2 contactShadows = UseContactShadows ? ContactShadowsTexture[dispatchID.xy] : float2(1.0, 1.0);
	OutputTexture[dispatchID.xy] = contactShadows * shadow;
}
