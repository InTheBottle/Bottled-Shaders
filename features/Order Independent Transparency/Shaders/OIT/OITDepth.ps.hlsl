#include "OIT/OITCommon.hlsli"
#include "Upscaling/UpscaleVS.hlsl"

Texture2D<float> TexWriteDepth : register(t0);

float main(VS_OUTPUT input) : SV_Depth
{
	float depth = TexWriteDepth[uint2(input.Position.xy)];
	[branch] if (depth == OIT_FarDepth())
		discard;
	return depth;
}
