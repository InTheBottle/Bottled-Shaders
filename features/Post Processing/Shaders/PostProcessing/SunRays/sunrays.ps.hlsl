#include "PostProcessing/fullscreen.hlsli"

Texture2D<float4> TexColor : register(t0);
Texture2D<float> RaysTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
#if defined(MASK) && defined(CLOUD_SHADOWS)
TextureCube<float> CloudShadowsTexture : register(t3);
#endif

SamplerState LinearSampler : register(s0);

cbuffer SunRaysCB : register(b1)
{
	row_major float4x4 InvViewProj;
	float2 LightUV;
	float2 UVScale;
	float2 UVMax;
	float BlurFactor;
	uint StepCount;
	float3 LightDirection;
	float LightBillboardTan;
	float3 RaysColor;
	float MaskBrightness;
	float MaskExponent;
	uint WeightedSteps;
	uint UseNoise;
	float InvScreenWidth;
	uint ReverseZ;
	float2 DynRes;
	uint FrameCount;
};

float SampleRays(float2 uv)
{
	return RaysTexture.SampleLevel(LinearSampler, min(saturate(uv) * UVScale, UVMax), 0);
}

float GetNoise(float2 pixel)
{
	float frameStep = float(FrameCount % 16) * 0.0625;
	float2 shifted = pixel + frameStep * float2(4.7526, 3.1914);
	float noise = frac(52.9829189 * frac(dot(shifted, float2(0.06711056, 0.00583715))));
	return UseNoise ? noise : 0.5;
}

float RadialBlur(float2 uv, float2 target, float noise)
{
	float stepInv = rcp(float(StepCount));
	float shift = -stepInv * noise;
	float rays = 0.0;
	[loop] for (uint i = 0; i < StepCount; i++)
	{
		shift += stepInv;
		float weight = WeightedSteps ? 1.0 - shift : 1.0;
		rays += SampleRays(lerp(uv, target, shift)) * weight;
	}
	return rays * stepInv;
}

#if defined(MASK)
#	if defined(CLOUD_SHADOWS)
float GetCloudTransmittance(float3 direction)
{
	return saturate(1.0 - CloudShadowsTexture.SampleLevel(LinearSampler, direction, 0));
}
#	endif

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 uv = input.TexCoord;
	float depth = DepthTexture.Load(int3(min(uint2(uv * DynRes), uint2(max(DynRes, 1.0) - 1.0)), 0));
	bool isSky = ReverseZ ? depth <= 0.000003 : depth >= 0.999997;
	[branch] if (!isSky)
		return 0.0;

	float4 positionCS = float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, 0.5, 1.0);
	float4 positionWS = mul(InvViewProj, positionCS);
	float3 viewDirection = normalize(positionWS.xyz / positionWS.w);

	float cosAngle = dot(viewDirection, LightDirection);
	[branch] if (cosAngle <= 0.0)
		return 0.0;

	float tanAngleSq = max(1.0 - cosAngle * cosAngle, 0.0) / (cosAngle * cosAngle);
	float radiusSq = tanAngleSq / max(LightBillboardTan * LightBillboardTan, 1e-8);
	float source = pow(saturate(1.0 - radiusSq), MaskExponent);

	float mask = MaskBrightness * source;

#	if defined(CLOUD_SHADOWS)
	mask *= GetCloudTransmittance(viewDirection);
#	endif

	float2 edge = abs(uv * 2.0 - 1.0);
	float vignette = max(edge.x, edge.y);
	return mask * (1.0 - vignette * vignette);
}
#elif defined(BLUR)
float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 uv = input.TexCoord;
	return RadialBlur(uv, lerp(uv, LightUV, BlurFactor), GetNoise(input.Position.xy));
}
#elif defined(COMPOSITE)
float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 uv = input.TexCoord;
	float3 direction = normalize(float3(LightUV - uv, 0.035));
	float2 target = uv + direction.xy * (InvScreenWidth * float(StepCount) * 6.0);
	float rays = RadialBlur(uv, target, GetNoise(input.Position.xy));
	float4 scene = TexColor[uint2(input.Position.xy)];
	return float4(scene.rgb + rays * RaysColor, scene.a);
}
#endif
