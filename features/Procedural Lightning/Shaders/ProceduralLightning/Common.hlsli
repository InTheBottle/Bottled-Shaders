#ifndef __PROCEDURAL_LIGHTNING_COMMON_HLSLI__
#define __PROCEDURAL_LIGHTNING_COMMON_HLSLI__

namespace ProceduralLightning
{
	static const uint MaxStrikes = 4;
	static const float KindSegment = 0.0;
	static const float KindCloudGlow = 1.0;

	struct Segment
	{
		float3 Start;
		float StartEnergy;
		float3 End;
		float EndEnergy;
		float StartArrival;
		float EndArrival;
		float Width;
		uint Packed;
	};

	struct Strike
	{
		float4 Color;
		float4 Glow;
		float Main;
		float Branch;
		float Front;
		float CloudGlow;
	};

	struct VertexOutput
	{
		float4 Position : SV_POSITION;
		nointerpolation float4 Line : TEXCOORD0;
		nointerpolation float4 Ends : TEXCOORD1;
		nointerpolation float4 Shape : TEXCOORD2;
		nointerpolation float4 Tint : COLOR0;
		nointerpolation float2 Params : TEXCOORD3;
		float2 Corner : TEXCOORD4;
	};

	cbuffer LightningFrame : register(b0)
	{
		Strike Strikes[MaxStrikes];
		float4 FogParams;
		float2 RenderSize;
		float Intensity;
		float FogAbsorption;
		float MinSigma;
		float MaxSigma;
		uint SegmentCount;
		float WidthToGlow;
	};

	float GetFogFactor(float distance)
	{
		if (FogParams.y <= 0.0)
			return 0.0;
		float distanceFactor = saturate(distance * FogParams.y - FogParams.x);
		return distanceFactor > 0.0 ? min(FogParams.w, exp2(FogParams.z * log2(distanceFactor))) : 0.0;
	}
}

#endif
