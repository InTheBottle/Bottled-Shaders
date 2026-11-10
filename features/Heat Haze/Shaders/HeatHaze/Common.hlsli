#ifndef __HEAT_HAZE_COMMON_HLSLI__
#define __HEAT_HAZE_COMMON_HLSLI__

namespace HeatHaze
{
	static const uint MaxPlumes = 12;
	static const float PlumeWidthScale = 1.6;
	static const float PlumeHeightScale = 7.0;

	cbuffer HeatHazeFrame : register(b0)
	{
		float4 Plumes[MaxPlumes];
		float4 PlumeParams[MaxPlumes];
		float2 UVMax;
		float Time;
		float Strength;
	};

	struct Interpolants
	{
		float4 Position : SV_POSITION;
		float2 PlumeCoord : TEXCOORD0;
		nointerpolation float Fade : TEXCOORD1;
		nointerpolation float Seed : TEXCOORD2;
		float ViewDepth : TEXCOORD3;
	};
}

#endif
