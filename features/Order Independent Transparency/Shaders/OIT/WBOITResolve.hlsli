#ifndef __WBOIT_RESOLVE__
#define __WBOIT_RESOLVE__

Texture2D<float4> TexWBOITAccumFront : register(t0);
Texture2D<float4> TexWBOITAccumAll : register(t1);
Texture2D<float2> TexWBOITRevealage : register(t2);

bool OIT_Resolve(uint2 address, out float4 front, out float4 composite)
{
	front = float4(0.0, 0.0, 0.0, 1.0);
	composite = float4(0.0, 0.0, 0.0, 1.0);

	float2 revealage = saturate(TexWBOITRevealage[address]);
	bool covered = revealage.x < 1.0;
	[branch] if (covered)
	{
		float4 accumFront = TexWBOITAccumFront[address];
		float4 accumAll = TexWBOITAccumAll[address];
		front = float4(accumFront.xyz / (0.000001 + accumFront.w) * (1.0 - revealage.y), revealage.y);
		composite = float4(accumAll.xyz / (0.000001 + accumAll.w) * (1.0 - revealage.x), revealage.x);
	}
	return covered;
}

#endif
