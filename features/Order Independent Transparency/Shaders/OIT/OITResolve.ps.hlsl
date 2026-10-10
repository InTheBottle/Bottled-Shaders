#include "OIT/OITCommon.hlsli"
#include "Upscaling/UpscaleVS.hlsl"

#define OIT_RESOLVE

#if defined(OIT_BLENDED)
#	include "OIT/WBOITResolve.hlsli"
#elif defined(OIT_ROV)
#	include "OIT/AOITResolve.hlsli"
#else
#	include "OIT/FragmentListResolve.hlsli"
#endif

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 Alpha: SV_Target1;
};

PS_OUTPUT main(VS_OUTPUT input)
{
	float4 front;
	float4 composite;
	[branch] if (!OIT_Resolve(uint2(input.Position.xy), front, composite))
		discard;

	float coverage = 1.0 - composite.w;
	PS_OUTPUT psout;
	psout.Color = float4(front.xyz, 1.0 - front.w);
	psout.Alpha = float4(coverage > 0.0 ? composite.xyz / max(coverage, 1e-6) : composite.xyz, coverage);
	return psout;
}
