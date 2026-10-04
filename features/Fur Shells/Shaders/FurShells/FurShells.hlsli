#ifndef __FUR_SHELLS_HLSLI__
#define __FUR_SHELLS_HLSLI__

namespace FurShells
{
	cbuffer FurShellData : register(b13)
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float Pad;
	};

#if defined(PSHADER)
	Texture2D<float4> TexShell : register(t122);
#endif

	float GetShell(uint instanceID)
	{
		return saturate((ShellCount - float(instanceID)) / max(ShellCount, 1.0));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
