#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "ProceduralNightSky/ProceduralNightSky.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif

float4 main(float4 position : SV_POSITION) : SV_Target0
{
	float depth = SceneDepth.Load(int3(position.xy, 0));
#if defined(REVERSE_Z)
	bool isSky = FrameBuffer::IsReverseProjection() ? depth <= 0.0 : depth >= 1.0;
#else
	bool isSky = depth >= 1.0;
#endif
	if (!isSky)
		discard;

	float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(position.xy * SharedData::BufferDim.zw);
	float4 viewPoint = mul(FrameBuffer::CameraViewProjInverse, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
	float3 view = normalize(viewPoint.xyz / viewPoint.w);
	if (view.z <= 0.0)
		discard;

	float dither = Random::InterleavedGradientNoise(position.xy, SharedData::FrameCount);
	return float4(ProceduralNightSky::EvaluateAurora(view, dither), 0.0);
}
