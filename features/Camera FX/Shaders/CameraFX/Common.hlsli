#ifndef __CAMERA_FX_COMMON_HLSLI__
#define __CAMERA_FX_COMMON_HLSLI__

#include "Common/TransferFunctions.hlsli"

namespace CameraFX
{
	static const uint MaxSplats = 24;

	static const float3x3 BT709ToBT2020 = {
		0.627403914928436279296875f, 0.3292830288410186767578125f, 0.0433130674064159393310546875f,
		0.069097287952899932861328125f, 0.9195404052734375f, 0.011362315155565738677978515625f,
		0.01639143936336040496826171875f, 0.08801330626010894775390625f, 0.895595252513885498046875f
	};

	static const float3x3 BT2020ToBT709 = {
		1.66049098968505859375f, -0.58764111995697021484375f, -0.072849862277507781982421875f,
		-0.12455047667026519775390625f, 1.13289988040924072265625f, -0.0083494223654270172119140625f,
		-0.01815076358616352081298828125f, -0.100578896701335906982421875f, 1.11872971057891845703125f
	};

	cbuffer CameraFXFrame : register(b0)
	{
		float4 Resolution;
		float4 Grade;
		float4 TintColor;
		float4 FadeColor;
		float4 Blur;
		float4 Radial;
		float4 Radial2;
		float4 DepthOfField;
		float4 DepthParams;
		float4 CameraData;
		float4 Themes;
		float4 ThemeParams;
		float4 Lens;
		float4 SunScreen;
		float4 SunColor;
		float4 MoonScreen[2];
		float4 Output;
		float4 Weather;
		float4 Weather2;
		float4 Weather3;
		float4 Wind;
		float4 Combat;
		float4 Combat2;
		float4 Combat3;
		float4 Combat4;
		float4 LensLook;
		float4 Splats[MaxSplats * 2];
	};

	struct Interpolants
	{
		float4 Position : SV_POSITION;
		float2 TexCoord : TEXCOORD0;
	};

	float Luminance(float3 color)
	{
		return dot(color, float3(0.2126, 0.7152, 0.0722));
	}

	float3 ToWorking(float3 color)
	{
		if (Output.z > 1.5)
			color = mul(BT2020ToBT709, color);
		if (Output.z > 0.5)
			color = TransferFunctions::LinearToSignedGamma22(color);
		return color;
	}

	float3 FromWorking(float3 color)
	{
		if (Output.z > 0.5)
			color = TransferFunctions::SignedGamma22ToLinear(color);
		if (Output.z > 1.5)
			color = mul(BT709ToBT2020, color);
		return color;
	}

	float Hash12(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * 0.1031);
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	float2 Hash22(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.xx + p3.yz) * p3.zy);
	}

	float ValueNoise(float2 p)
	{
		float2 cell = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		float a = Hash12(cell);
		float b = Hash12(cell + float2(1, 0));
		float c = Hash12(cell + float2(0, 1));
		float d = Hash12(cell + float2(1, 1));
		return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
	}

	float Fbm(float2 p, uint octaves)
	{
		float sum = 0.0;
		float amplitude = 0.5;
		[loop] for (uint i = 0; i < octaves; i++)
		{
			sum += amplitude * ValueNoise(p);
			p = p * 2.03 + float2(17.1, 9.7);
			amplitude *= 0.5;
		}
		return sum;
	}

	float Ridge(float2 p)
	{
		return 1.0 - abs(ValueNoise(p) * 2.0 - 1.0);
	}

	float2 Rotate(float2 p, float angle)
	{
		float s, c;
		sincos(angle, s, c);
		return float2(p.x * c - p.y * s, p.x * s + p.y * c);
	}

	float EdgeDistance(float2 uv)
	{
		float2 centered = (uv - 0.5) * float2(Resolution.x * Resolution.w, 1.0);
		return length(centered) / length(float2(0.5 * Resolution.x * Resolution.w, 0.5));
	}

	float SuperellipseRadius(float2 uv, float n)
	{
		float2 x = max(abs(uv - 0.5) * 2.0, 1e-4);
		return pow(pow(x.x, n) + pow(x.y, n), 1.0 / n);
	}

	float FrameArrival(float2 uv, float n)
	{
		return saturate(1.0 - SuperellipseRadius(uv, n) / pow(2.0, 1.0 / n));
	}
}

#endif
