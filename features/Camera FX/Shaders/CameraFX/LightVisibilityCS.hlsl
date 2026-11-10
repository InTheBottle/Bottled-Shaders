#include "Common/FlareOcclusion.hlsli"

cbuffer CameraFXVisibility : register(b0)
{
	float4 Lights[3];
	float4 DepthParams;
	float4 DepthSize;
};

Texture2D<float> DepthTexture : register(t0);
RWTexture2D<float> Visibility : register(u0);

groupshared uint VisibleCount[3];

[numthreads(16, 3, 1)] void main(uint3 threadID : SV_GroupThreadID)
{
	uint tap = threadID.x;
	uint light = threadID.y;
	if (tap == 0)
		VisibleCount[light] = 0;
	GroupMemoryBarrierWithGroupSync();

	float4 source = Lights[light];
	if (source.z > 0.5) {
		float2 uv = source.xy + FlareOcclusion::GetSampleOffset(tap);
		if (all(uv >= 0.0) && all(uv < 1.0)) {
			float depth = DepthTexture.Load(int3(uv * DepthParams.xy, 0));
			bool sky = DepthParams.z > 0.5 ? depth <= 1e-6 : depth >= 0.999999;
			if (sky)
				InterlockedAdd(VisibleCount[light], 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (tap == 0) {
		uint2 texel = uint2(light, 0);
		float current = Visibility[texel];
		float target = FlareOcclusion::GetVisibility(VisibleCount[light]);
		float rate = target < current ? DepthParams.w : source.w;
		Visibility[texel] = source.z > 0.5 ? lerp(current, target, saturate(rate)) : 0.0;
	}
}
