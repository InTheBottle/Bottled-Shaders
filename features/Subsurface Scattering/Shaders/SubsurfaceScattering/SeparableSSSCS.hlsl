RWTexture2D<float4> SSSRW : register(u0);

Texture2D<float4> ColorTexture : register(t0);
Texture2D<float4> DepthTexture : register(t1);
Texture2D<float4> MaskTexture : register(t2);
Texture2D<float4> AlbedoTexture : register(t3);
Texture2D<float4> NormalTexture : register(t4);

SamplerState PointSampler : register(s0);

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"

#if defined(BURLEY)
#	include "SubsurfaceScattering/Burley.hlsli"
#else
#	include "SubsurfaceScattering/SeparableSSS.hlsli"
#endif

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 texCoord = (DTid.xy + 0.5) * SharedData::BufferDim.zw;
	float2 mask = MaskTexture[DTid.xy].xy;
	uint profile = SSSDecodeProfile(mask.y);
	float sssAmount = SSSGetAmount(mask, profile);

#if defined(BURLEY)

	if (sssAmount > 0.0) {
		float4 color = BurleyNormalizedSS(DTid.xy, texCoord, sssAmount, profile, SSSRW[DTid.xy]);
		SSSRW[DTid.xy] = max(0, color);
	}

#elif defined(HORIZONTAL)

	float4 color = SSSSBlurCS(texCoord, float2(1.0, 0.0), sssAmount, profile);
	SSSRW[DTid.xy] = max(0, color);

#else

	if (sssAmount > 0.0) {
		float4 originalColor = SSSRW[DTid.xy];
		float4 color = SSSSBlurCS(texCoord, float2(0.0, 1.0), sssAmount, profile);
		float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
		color.rgb = SSSApplyAlbedo(color.rgb, ColorManagement::SceneToLinear(originalColor.rgb), albedo, ScatterMode);
		color.rgb = ColorManagement::LinearToScene(color.rgb);
		SSSRW[DTid.xy] = float4(color.rgb, originalColor.a);
	}

#endif
}
