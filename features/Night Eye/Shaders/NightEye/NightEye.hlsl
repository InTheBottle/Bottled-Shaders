// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#ifdef VSHADER
float4 mainVS(uint vertexId : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#ifdef PSHADER
#include "NightEye/Common.hlsli"
#include "NightEye/CustomVFX.hlsli"

Texture2D<float4> Scene : register(t0);
SamplerState SceneSampler : register(s0);

cbuffer NightEyeParameters : register(b0)
{
	float4 ExposureGammaLiftStrength;
	float4 Tint;
	float4 VFX;
	float4 BufferDim;
	float4 Anaglyph;
	float4 AnaglyphLeft;
	float4 AnaglyphRight;
	float4 HDRData;
};

static const float kGlowRadiusPixels = 2.0;
static const float kGrainFramesPerSecond = 60.0;
static const float kVignetteStart = 0.15;
static const float kVignetteEnd = 1.0;

float3 GradeScene(float4 source, float2 uv, float2 pixelPosition, float range)
{
	float3 color = saturate(source.rgb / range);
	float3 enhanced = pow(color, ExposureGammaLiftStrength.y) * exp2(ExposureGammaLiftStrength.x);
	enhanced += ExposureGammaLiftStrength.z * (1.0 - color);
	enhanced = lerp(enhanced, Color::RGBToLuminance(enhanced) * Tint.rgb, Tint.w);
	[branch] if (VFX.x > 0.0) {
		float3 neighbors = 0.0;
		const float2 offsets[] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
		[unroll] for (uint index = 0; index < 4; ++index)
			neighbors += Scene.SampleLevel(SceneSampler, uv + offsets[index] * BufferDim.xy * kGlowRadiusPixels, 0).rgb / range;
		enhanced += max(neighbors * 0.25 - color, 0.0) * VFX.x;
	}
	float2 centered = uv * 2.0 - 1.0;
	enhanced *= 1.0 - VFX.y * smoothstep(kVignetteStart, kVignetteEnd, dot(centered, centered));
	[branch] if (VFX.z > 0.0)
		enhanced += (Random::InterleavedGradientNoise(pixelPosition, (uint)(BufferDim.z * kGrainFramesPerSecond)) - 0.5) * VFX.z;
	[branch] if (VFX.w > 0.0) {
		float3 custom = ApplyNightEyeCustomVFX(enhanced, uv, pixelPosition, BufferDim.z);
		if (all(isfinite(custom)))
			enhanced = lerp(enhanced, custom, VFX.w);
	}
	return enhanced;
}

float4 mainPS(float4 position : SV_Position) : SV_Target0
{
	float2 uv = position.xy * BufferDim.xy;
	float4 source = Scene.SampleLevel(SceneSampler, uv, 0);
	float range = 1.0;
	if (BufferDim.w > 0.5 && HDRData.x > 0.5 && HDRData.y > 0.0)
		range = max(1.0, HDRData.z / HDRData.y);
	if (!isfinite(range)) range = 1.0;
	float3 enhanced = GradeScene(source, uv, position.xy, range);
	[branch] if (Anaglyph.x > 0.0) {
		float2 eyeOffsetPixels = Anaglyph.yz * 0.5;
		float2 eyeOffsetUV = eyeOffsetPixels * BufferDim.xy;
		float2 leftUV = uv - eyeOffsetUV;
		float2 rightUV = uv + eyeOffsetUV;
		float3 left = GradeScene(Scene.SampleLevel(SceneSampler, leftUV, 0), leftUV, position.xy - eyeOffsetPixels, range);
		float3 right = GradeScene(Scene.SampleLevel(SceneSampler, rightUV, 0), rightUV, position.xy + eyeOffsetPixels, range);
		left = lerp(left, Color::RGBToLuminance(left), saturate(Anaglyph.w));
		right = lerp(right, Color::RGBToLuminance(right), saturate(Anaglyph.w));
		enhanced = lerp(enhanced, left * AnaglyphLeft.rgb + right * AnaglyphRight.rgb, saturate(Anaglyph.x));
	}
	return float4(lerp(source.rgb, saturate(enhanced) * range, ExposureGammaLiftStrength.w), source.a);
}
#endif
