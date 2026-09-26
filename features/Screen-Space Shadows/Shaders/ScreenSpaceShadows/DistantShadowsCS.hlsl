// Distant LOD shadows: a long-range screen-space sun raymarch for pixels beyond the shadow cascades,
// where terrain LOD, object LOD and tree LOD otherwise receive no shadows at all.
// Rays march in world space with geometrically growing steps, so a handful of depth fetches cover
// thousands of units. The result multiplies the contact shadow texture every shader already samples.

#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT, OFF -> R24_UNORM_X8_TYPELESS (float under reverse Z)
#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepthTexture : register(t0);
#endif
Texture2D<unorm float2> ContactShadowsTexture : register(t1);  // Copy of the Bend contact shadows
RWTexture2D<unorm float2> OutputTexture : register(u0);        // Front- and back-facing shadow visibility (R8G8_UNORM)

cbuffer DistantShadowsCB : register(b1)
{
	float2 RenderSize;       // Dynamic-resolution render size in pixels
	float2 InvRenderSize;    // 1 / RenderSize
	float StartDistance;     // View depth where distant shadows start fading in (cascade end minus FadeLength)
	float FadeLength;        // View depth range of the fade in
	float MaxRayLength;      // World-space length of each ray
	float Intensity;         // Shadow strength
	float ThicknessScale;    // Assumed occluder thickness as a fraction of the distance travelled
	uint SampleCount;        // Depth fetches per ray
	uint UseContactShadows;  // ContactShadowsTexture holds this frame's contact shadows
	float pad0;
};

// Shortest first step, in world units; also the smallest self-shadowing bias
static const float MinStepLength = 32.0;
// Fraction of the view depth used as the first step and as the depth comparison bias
static const float FirstStepDepthScale = 0.004;
static const float DepthBiasScale = 0.002;

float GetViewDepth(uint2 pixel)
{
	return SharedData::GetScreenDepth(SceneDepthTexture[pixel]);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(RenderSize)))
		return;

	float depth = SceneDepthTexture[dispatchID.xy];
	if (depth == FrameBuffer::FarPlaneDepth())
		return;

	float viewDepth = SharedData::GetScreenDepth(depth);
	float fade = saturate((viewDepth - StartDistance) / FadeLength);
	if (fade <= 0.0)
		return;  // Cascaded shadow maps cover this pixel; keep the contact shadows as they are

	float firstStep = max(MinStepLength, viewDepth * FirstStepDepthScale);
	if (firstStep >= MaxRayLength)
		return;

	float2 uv = (dispatchID.xy + 0.5) * InvRenderSize;
	float4 positionWS = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
	positionWS.xyz /= positionWS.w;

	float3 lightDirection = SharedData::DirLightDirection.xyz;
	float noise = Random::InterleavedGradientNoise(dispatchID.xy, SharedData::FrameCount);

	// Geometric steps: fine near the receiver, coarse far away where the screen-space footprint is small anyway
	float growth = pow(MaxRayLength / firstStep, 1.0 / SampleCount);
	float distanceTravelled = firstStep * pow(growth, noise);

	float occlusion = 0.0;
	[loop] for (uint i = 0; i < SampleCount; i++, distanceTravelled *= growth)
	{
		float4 sampleCS = mul(FrameBuffer::CameraViewProj, float4(positionWS.xyz + lightDirection * distanceTravelled, 1.0));
		if (sampleCS.w <= 0.0)
			break;

		float2 sampleNDC = sampleCS.xy / sampleCS.w;
		if (any(abs(sampleNDC) >= 1.0))
			break;  // Left the screen; nothing is known about occluders there

		uint2 samplePixel = uint2((sampleNDC * float2(0.5, -0.5) + 0.5) * RenderSize);
		float depthDelta = sampleCS.w - GetViewDepth(samplePixel);  // > 0: the ray is behind the visible surface

		float bias = max(MinStepLength, sampleCS.w * DepthBiasScale);
		float thickness = max(distanceTravelled * ThicknessScale, 2.0 * bias);

		// Soft onset past the bias, soft falloff near the thickness limit so thin foreground objects don't cast
		float sampleOcclusion = saturate((depthDelta - bias) / bias) * saturate((thickness - depthDelta) / (0.25 * thickness));
		occlusion = max(occlusion, sampleOcclusion);

		if (occlusion >= 1.0)
			break;
	}

	float shadow = 1.0 - occlusion * fade * Intensity;
	float2 contactShadows = UseContactShadows ? ContactShadowsTexture[dispatchID.xy] : float2(1.0, 1.0);
	OutputTexture[dispatchID.xy] = contactShadows * shadow;
}
