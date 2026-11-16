#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/PreExposure.hlsli"
#include "Common/SharedData.hlsli"
#include "ProceduralNightSky/ProceduralNightSky.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif

SamplerState LinearSampler : register(s0);

#if defined(EXP_HEIGHT_FOG)
#	define SampColorSampler LinearSampler
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#if defined(PHYSICAL_SKY)
#	include "PhysicalSky/Common.hlsli"
#endif

#if defined(PHYSICAL_SKY) || defined(EXP_HEIGHT_FOG)
#	include "Common/ViewMedium.hlsli"
#endif

float4 main(float4 position : SV_POSITION) : SV_Target0
{
	float depth = SceneDepth.Load(int3(position.xy, 0));
#if defined(REVERSE_Z)
	bool isSky = FrameBuffer::IsReverseProjection() ? depth <= 0.000003 : depth >= 0.999997;
#else
	bool isSky = depth >= 0.999997;
#endif
	if (!isSky)
		discard;

	float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(position.xy * SharedData::BufferDim.zw);
	float4 viewPoint = mul(FrameBuffer::CameraViewProjInverse, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
	float3 view = normalize(viewPoint.xyz / viewPoint.w);
	if (view.z <= 0.0)
		discard;

	float3 aurora = ProceduralNightSky::EvaluateAurora(view);

	// The aurora sits above the atmosphere, so it takes the same losses as the stars and moons
	float3 transmittance = 1.0;
#if defined(PHYSICAL_SKY) || defined(EXP_HEIGHT_FOG)
	if (SharedData::PostWaterComposite)
		transmittance = ViewMedium::SampleViewMedium(view, uv, LinearSampler, true).transmittance;
#endif
#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		transmittance *= PhysSky::SampleViewCelestialTr(view, LinearSampler);
#endif
	aurora = ColorManagement::SceneColor::ScaleByLinear(aurora, transmittance);

	return float4(PreExposure::Encode(aurora), 0.0);
}
