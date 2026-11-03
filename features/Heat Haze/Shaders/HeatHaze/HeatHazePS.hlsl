#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "HeatHaze/Common.hlsli"

Texture2D<float4> SceneColor : register(t0);
#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t1);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t1);
#endif

SamplerState LinearSampler : register(s0);

static const float ForegroundMargin = 8.0;
static const float MaxOffset = 0.006;
static const float DistortionScale = 0.4;

float Hash(float2 p)
{
	float2 q = frac(p * float2(123.34, 456.21));
	q += dot(q, q + 45.32);
	return frac(q.x * q.y);
}

float ValueNoise(float2 p)
{
	float2 cell = floor(p);
	float2 blend = p - cell;
	blend = blend * blend * (3.0 - 2.0 * blend);
	cell -= 256.0 * floor(cell / 256.0);
	float bottom = lerp(Hash(cell), Hash(cell + float2(1.0, 0.0)), blend.x);
	float top = lerp(Hash(cell + float2(0.0, 1.0)), Hash(cell + float2(1.0, 1.0)), blend.x);
	return lerp(bottom, top, blend.y);
}

float2 Shimmer(float2 p)
{
	float2 coarse = float2(ValueNoise(p), ValueNoise(p + float2(17.3, 5.1)));
	float2 fine = float2(ValueNoise(p * 2.3 + float2(3.7, 9.2)), ValueNoise(p * 2.3 + float2(11.9, 2.4)));
	return (coarse - 0.5) * 2.0 + (fine - 0.5);
}

float4 main(HeatHaze::Interpolants input) : SV_Target
{
	float across = input.PlumeCoord.x;
	float height = input.PlumeCoord.y;
	float falloff = 1.0 - height;
	float mask = exp(-3.0 * across * across) * smoothstep(0.0, 0.12, height) * falloff * falloff * input.Fade;
	if (mask <= 0.002)
		discard;

	float sceneViewDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	if (sceneViewDepth < input.ViewDepth - ForegroundMargin)
		discard;

	float2 noiseCoord = float2(across * 1.4, height * 4.5 - HeatHaze::Time * 1.1) + input.Seed * 37.0;
	float2 offset = Shimmer(noiseCoord) * mask * HeatHaze::Strength * DistortionScale / max(input.ViewDepth, 24.0);
	offset = clamp(offset, -MaxOffset, MaxOffset);

	float2 screenUV = input.Position.xy * SharedData::BufferDim.zw;
	float2 sampleUV = min(saturate(screenUV + offset), HeatHaze::UVMax);

	float sampleViewDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(sampleUV * SharedData::BufferDim.xy, 0)));
	if (sampleViewDepth < input.ViewDepth - ForegroundMargin)
		sampleUV = screenUV;

	return float4(SceneColor.SampleLevel(LinearSampler, sampleUV, 0).rgb, saturate(mask * 4.0));
}
