#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#include "Common/Game.hlsli"
#include "LightLimitFix/Attenuation.hlsli"
#include "Common/NamespacedCBuffer.hlsli"
#include "Common/DCLFObjects.hlsli"

namespace LightLimitFix
{

#include "LightLimitFix/Common.hlsli"

	cbuffer StrictLightData : register(b3)
	{
		uint NSCB(LightLimitFix, NumStrictLights);
		int NSCB(LightLimitFix, RoomIndex);
		uint NSCB(LightLimitFix, ShadowBitMask);
		uint NSCB(LightLimitFix, FirstPerson);
		float4 NSCB(LightLimitFix, WorldEyePosition);
		Light NSCB(LightLimitFix, StrictLights)[15];
	};
	NSCB_ALIAS(LightLimitFix, uint, NumStrictLights)
#if defined(DCLF_BINDLESS_DRAW)
	// Drawcall Limit Fix supplies these two per object from its own record, so that the StrictLightData
	// buffer stops being what makes a draw's binding record unique - DCLF writes nothing else into it, and
	// with NumStrictLights at 0 the StrictLights tail is never read. The cbuffer above keeps its
	// declaration and its layout: only the names change source, and NSCB_ALIAS is already exactly this
	// shape of static under DXC. Every other includer of this header compiles the aliases unchanged.
	// Read where used, not as statics: a plain draw's object word (DCLF_PULLED) is its sequence's, which its main loads after the
	// statics are initialised.
	int RoomIndexOf() { return DCLFObjects[DCLFObjectIndex].RoomIndex; }
	// The shadow lights are the draw's, selected by BuildDrawsCS this frame (the object word), not the record's; as the host shader
	// narrows them (Lighting.hlsl: none for a pass without DefShadow).
#	if defined(DCLF_SHADOW_LIGHT_MASK)
	uint ShadowBitMaskOf() { return DCLF_SHADOW_LIGHT_MASK; }
#	else
	uint ShadowBitMaskOf() { return DCLFLocalShadowMask; }
#	endif
#else
	NSCB_ALIAS(LightLimitFix, int, RoomIndex)
	NSCB_ALIAS(LightLimitFix, uint, ShadowBitMask)
	int RoomIndexOf() { return RoomIndex; }
	uint ShadowBitMaskOf() { return ShadowBitMask; }
#endif  // DCLF_BINDLESS_DRAW
	NSCB_ALIAS(LightLimitFix, uint, FirstPerson)
	NSCB_ALIAS(LightLimitFix, float4, WorldEyePosition)
	NSCB_ARRAY_ALIAS(LightLimitFix, Light, StrictLights, 15)

	StructuredBuffer<Light> lights : register(t35);
	StructuredBuffer<uint> lightList : register(t36);       //MAX_CLUSTER_LIGHTS * 16^3
	StructuredBuffer<LightGrid> lightGrid : register(t37);  //16^3

#include "LightLimitFix/LocalShadowProjection.hlsli"

	StructuredBuffer<LocalShadowData> LocalShadows : register(t102);
	Texture2DArray<float> LocalShadowMaps : register(t103);

	bool GetClusterIndex(in float2 uv, in float z, inout uint clusterIndex)
	{
		const uint3 clusterSize = SharedData::lightLimitFixSettings.ClusterSize.xyz;

		if (!FrameBuffer::FrameParams.y)  // Fix first person lights
			uv = 0.5;

		z = max(z, SharedData::CameraData.y);

		uint clusterZ = log(z / SharedData::CameraData.y) * clusterSize.z / log(SharedData::CameraData.x / SharedData::CameraData.y);
		uint3 cluster = uint3(uint2(uv * clusterSize.xy), clusterZ);

		// Bounds validation to prevent out-of-range cluster indices
		if (any(cluster >= clusterSize))
			return false;

		clusterIndex = cluster.x + (clusterSize.x * cluster.y) + (clusterSize.x * clusterSize.y * cluster.z);
		return true;
	}

	bool IsLightIgnored(Light light)
	{
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			return !(ShadowBitMaskOf() & (1 << light.shadowLightIndex));
		}

		bool lightIgnored = false;
		const int roomIndexOfObject = RoomIndexOf();
		if ((light.lightFlags & LightFlags::PortalStrict) && roomIndexOfObject >= 0) {
			lightIgnored = true;
			int roomIndex = roomIndexOfObject;
			[unroll] for (int flagsIndex = 0; flagsIndex < 4; ++flagsIndex)
			{
				if (roomIndex < 32) {
					if (((light.roomFlags[flagsIndex] >> roomIndex) & 1) == 1) {
						lightIgnored = false;
					}
					break;
				}
				roomIndex -= 32;
			}
		}
		return lightIgnored;
	}

	float2x2 GetShadowRotationMatrix(float noise)
	{
		float angle = noise * Math::TAU;
		float sinAngle, cosAngle;
		sincos(angle, sinAngle, cosAngle);
		return float2x2(cosAngle, sinAngle, -sinAngle, cosAngle);
	}

	/** @brief Bilinear PCF tap equivalent to SampleCmpLevelZero; the host shaders have no comparison sampler bound, so this uses Gather. */
	float SampleLocalShadowTap(Texture2DArray<float> shadowMaps, SamplerState samp, uint slice, float2 uv, float receiverDepth, float texel)
	{
		float2 texelPosition = uv / texel - 0.5;
		float2 weight = frac(texelPosition);
		float4 lit = float4(shadowMaps.GatherRed(samp, float3((floor(texelPosition) + 1.0) * texel, slice)) >= receiverDepth);
		return lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
	}

	float SampleLocalShadowPCF(Texture2DArray<float> shadowMaps, SamplerState samp, uint slice, float2 uv, float receiverDepth, float noise, float2 clampMin, float2 clampMax, float texel, uint sampleCount, float radiusUV)
	{
		float shadow = 0.0;
		[branch] if (sampleCount <= 1)
		{
			shadow = SampleLocalShadowTap(shadowMaps, samp, slice, clamp(uv, clampMin, clampMax), receiverDepth, texel);
		}
		else
		{
			const float2x2 rotationMatrix = GetShadowRotationMatrix(noise);
			const uint stride = sampleCount >= 8 ? 1 : 2;
			float sum = 0.0;
			[loop] for (uint i = 0; i < 8; i += stride)
			{
				float2 offset = mul(Random::SpiralSampleOffset8(i), rotationMatrix) * radiusUV;
				sum += SampleLocalShadowTap(shadowMaps, samp, slice, clamp(uv + offset, clampMin, clampMax), receiverDepth, texel);
			}
			shadow = sum * (stride == 1 ? 0.125 : 0.25);
		}
		return shadow;
	}

	// position is relative to eyePosition; each cached projection is relative to its own Origin.
	float GetLocalShadow(Texture2DArray<float> shadowMaps, SamplerState samp, LocalShadowData data, uint slice, float3 position, float3 eyePosition, float3 towardLight, bool skinnedReceiver, float noise, float texel, uint sampleCount, float radiusUV)
	{
		float rawShadow = 1.0;
		float fade = 0.0;

		[branch] if (data.Params.y > 0.0)
		{
			fade = data.Params.w;
			float3 shadowPosition = position + (eyePosition - data.Origin.xyz) + towardLight * (skinnedReceiver ? data.Params2.y : 0.0);
			const LocalShadowLookup lookup = GetLocalShadowLookup(data, shadowPosition, texel);
			rawShadow = lookup.sampled ? SampleLocalShadowPCF(shadowMaps, samp, slice, lookup.uv, lookup.receiverDepth, noise, lookup.clampMin, lookup.clampMax, texel,
											 sampleCount, radiusUV) * lookup.scale :
			                             lookup.unsampled;
		}

		return lerp(1.0, rawShadow, fade);
	}

	float GetLocalShadow(SamplerState samp, uint slice, float3 position, float3 eyePosition, float3 towardLight, bool skinnedReceiver, float noise)
	{
		return GetLocalShadow(LocalShadowMaps, samp, LocalShadows[slice], slice, position, eyePosition, towardLight, skinnedReceiver, noise,
			SharedData::lightLimitFixSettings.LocalShadowTexelSize, SharedData::lightLimitFixSettings.LocalShadowSamples, SharedData::lightLimitFixSettings.LocalShadowFilterRadius);
	}

	// The first-person viewmodel renders in a compressed depth range below this linearized
	// value; occluders there are not part of the world.
	static const float CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH = 16.5;

	// Reference view-space depth for perspective-correct rays. Beyond it, ray length and
	// thickness scale linearly with depth so each ray covers ~constant screen distance.
	static const float CONTACT_SHADOW_REFERENCE_DEPTH = 100.0;

	static const uint CONTACT_SHADOW_COARSE_STEPS = 4;

	static const float CONTACT_SHADOW_MIN_PIXELS_PER_STEP = 1.0;

	static const float CONTACT_SHADOW_MIN_RAY_DEPTH = 1.0;

	static const float CONTACT_SHADOW_MARCH_EXPONENT = 2.0;

	static const float CONTACT_SHADOW_RAY_END_FADE = 4.0;

	static const float CONTACT_SHADOW_DEPTH_BIAS = 2.0 / 65536.0;

	// Rotated-grid sub-pixel offsets in pixels; 2x uses entries 0 and 2.
	static const float2 CONTACT_SHADOW_SUBSAMPLE_OFFSETS[4] = {
		float2(-0.375, -0.125),
		float2(0.125, -0.375),
		float2(0.375, 0.125),
		float2(-0.125, 0.375)
	};

	struct ContactShadowRay
	{
		float2 uvOrigin;
		float2 uvDelta;
		float2 inverseDepth;
		float tMax;
	};

	float2 ClipToUV(float4 clipPosition)
	{
		return (clipPosition.xy / clipPosition.w) * float2(0.5, -0.5) + 0.5;
	}

	ContactShadowRay GetContactShadowRay(float3 viewPosition, float3 endPosition)
	{
		ContactShadowRay ray;
		ray.uvOrigin = ClipToUV(mul(FrameBuffer::CameraProj, float4(viewPosition, 1.0)));
		ray.uvDelta = ClipToUV(mul(FrameBuffer::CameraProj, float4(endPosition, 1.0))) - ray.uvOrigin;
		ray.inverseDepth.x = rcp(viewPosition.z);
		ray.inverseDepth.y = rcp(endPosition.z) - ray.inverseDepth.x;

		const float2 room = saturate(ray.uvDelta >= 0.0 ? 1.0 - ray.uvOrigin : ray.uvOrigin);
		const float2 axisT = room / max(abs(ray.uvDelta), 1e-7);
		ray.tMax = min(1.0, min(axisT.x, axisT.y));
		return ray;
	}

	float GetContactShadowRayT(ContactShadowRay ray, float u)
	{
		return ray.tMax * pow(u, CONTACT_SHADOW_MARCH_EXPONENT);
	}

	float GetContactShadowRayDepth(ContactShadowRay ray, float t)
	{
		return rcp(ray.inverseDepth.x + ray.inverseDepth.y * t);
	}

	uint GetPixelLimitedSteps(ContactShadowRay ray, uint requestedSteps)
	{
		const float2 pixelScale = FrameBuffer::DynamicResolutionParams1.xy * SharedData::BufferDim.xy;
		const float rayPixels = length(ray.uvDelta * ray.tMax * pixelScale);

		return min(requestedSteps, (uint)max(2.0, floor(rayPixels / CONTACT_SHADOW_MIN_PIXELS_PER_STEP)));
	}

	float2 GetContactShadowSceneDepths(SamplerState samp, float2 uv)
	{
		const float2 texel = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv) * SharedData::BufferDim.xy - 0.5;
		const float2 maxBase = floor(FrameBuffer::DynamicResolutionParams1.xy * SharedData::BufferDim.xy) - 2.0;
		const float2 base = clamp(floor(texel), 0.0, maxBase);
		const float2 weight = saturate(texel - base);

		const float4 depths = SharedData::DepthTexture.GatherRed(samp, (base + 1.0) * SharedData::BufferDim.zw);
		const float filtered = lerp(lerp(depths.w, depths.z, weight.x), lerp(depths.x, depths.y, weight.x), weight.y);
		const float nearest = weight.x < 0.5 ? (weight.y < 0.5 ? depths.w : depths.x) : (weight.y < 0.5 ? depths.z : depths.y);

		const float2 linearDepths = SharedData::GetScreenDepths(float4(filtered, nearest, 0, 0)).xy;
		return float2(max(linearDepths.x, linearDepths.y), min(linearDepths.x, linearDepths.y));
	}

	// Coarse rejection prepass: samples the ray at four points and reports whether any interval
	// can intersect the occlusion band. Rays that never come near an occluder skip the fine
	// march entirely, so cost scales with penumbra coverage instead of step count.
	bool MayBeOccluded(ContactShadowRay ray, float thickness)
	{
		float previousRayDepth = rcp(ray.inverseDepth.x);
		float previousDelta = 0.0;
		bool havePrevious = false;

		[unroll] for (uint i = 0; i < CONTACT_SHADOW_COARSE_STEPS; i++)
		{
			const float t = GetContactShadowRayT(ray, float(i + 1) / float(CONTACT_SHADOW_COARSE_STEPS));
			const float rayDepth = GetContactShadowRayDepth(ray, t);
			const float slack = abs(rayDepth - previousRayDepth);
			previousRayDepth = rayDepth;

			const float sceneDepth = SharedData::GetScreenDepth(ray.uvOrigin + ray.uvDelta * t);
			if (sceneDepth <= CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH)
				return true;

			const float delta = rayDepth - sceneDepth;
			if (delta > -slack && delta < thickness + slack)
				return true;

			if (havePrevious && min(previousDelta, delta) < thickness && max(previousDelta, delta) > 0.0)
				return true;
			previousDelta = delta;
			havePrevious = true;
		}
		return false;
	}

	// Screen-space contact shadow for a local light: marches the depth buffer from the shaded
	// point toward the light and returns a visibility multiplier in [0, 1].
	float ContactShadows(SamplerState samp, float3 viewPosition, float noise, float3 lightVectorVS, float lightDistance, uint contactShadowSteps, float strengthScale)
	{
		float occlusion = 0.0;

		[branch] if (contactShadowSteps > 0)
		{
			const float perspectiveScale = max(viewPosition.z, CONTACT_SHADOW_REFERENCE_DEPTH) / CONTACT_SHADOW_REFERENCE_DEPTH;
			const float thickness = SharedData::lightLimitFixSettings.ContactShadowDepthThickness * perspectiveScale;
			const float depthBias = CONTACT_SHADOW_DEPTH_BIAS * SharedData::CameraData.z / SharedData::CameraData.w;

			const float3 lightDirectionVS = lightVectorVS * rcp(max(lightDistance, 1e-3));
			float rayLength = min(SharedData::lightLimitFixSettings.ContactShadowLength * perspectiveScale, lightDistance);
			rayLength = min(rayLength, (viewPosition.z - CONTACT_SHADOW_MIN_RAY_DEPTH) / max(-lightDirectionVS.z, 1e-6));

			const ContactShadowRay ray = GetContactShadowRay(viewPosition, viewPosition + lightDirectionVS * max(rayLength, 0.0));
			const uint marchSteps = GetPixelLimitedSteps(ray, contactShadowSteps);
			const uint subSamples = clamp(SharedData::lightLimitFixSettings.ContactShadowSubSamples, 1u, 4u);

			// The coarse test is skipped at the default step count so that path stays unchanged; with extra
			// sub-pixel rays it is cheaper than the march it can reject.
			bool march = true;
			[branch] if (marchSteps > CONTACT_SHADOW_COARSE_STEPS || subSamples > 1)
				march = MayBeOccluded(ray, thickness);

			[branch] if (march)
			{
				const float2 pixelUV = rcp(FrameBuffer::DynamicResolutionParams1.xy * SharedData::BufferDim.xy);
				const float2x2 subSampleRotation = GetShadowRotationMatrix(noise);
				const float stepScale = rcp(float(marchSteps));
				float occlusionSum = 0.0;
				[loop] for (uint s = 0; s < subSamples; s++)
				{
					float2 uvOrigin = ray.uvOrigin;
					[branch] if (subSamples > 1)
						uvOrigin += mul(CONTACT_SHADOW_SUBSAMPLE_OFFSETS[(subSamples == 2 ? s * 2 : s) & 3], subSampleRotation) * pixelUV;
					const float jitter = 1.0 - frac(noise + float(s) * 0.3819660113);
					[loop] for (uint i = 0; i < marchSteps; i++)
					{
						const float t = GetContactShadowRayT(ray, (float(i) + jitter) * stepScale);
						const float rayDepth = GetContactShadowRayDepth(ray, t);
						const float2 sceneDepths = GetContactShadowSceneDepths(samp, uvOrigin + ray.uvDelta * t);

						const bool behindSurface = rayDepth > sceneDepths.x * (1.0 + sceneDepths.x * depthBias);
						const bool withinThickness = rayDepth - sceneDepths.y < thickness;
						if (sceneDepths.y > CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH && behindSurface && withinThickness) {
							occlusionSum += saturate((1.0 - t) * CONTACT_SHADOW_RAY_END_FADE);
							break;
						}
					}
				}
				occlusion = occlusionSum / float(subSamples);
			}
		}

		return 1.0 - occlusion * SharedData::lightLimitFixSettings.ContactShadowStrength * strengthScale;
	}

	float3 GetVisibleSurfacePositionFromDepth(float3 viewPosition, float visibleDepth)
	{
		[flatten] if (visibleDepth > CONTACT_SHADOW_FIRST_PERSON_MAX_DEPTH && visibleDepth < viewPosition.z)
			viewPosition *= visibleDepth / viewPosition.z;
		return viewPosition;
	}

	float3 GetVisibleSurfacePosition(float3 viewPosition, float2 pixelPosition)
	{
		return GetVisibleSurfacePositionFromDepth(viewPosition, SharedData::GetScreenDepth(SharedData::DepthTexture.Load(int3(pixelPosition, 0)).x));
	}

	uint GetContactShadowSteps(float viewDepth, out float strengthScale)
	{
		uint steps = 0;
		strengthScale = 0.0;
		[branch] if (SharedData::lightLimitFixSettings.EnableContactShadows) {
			const float requestedSteps = SharedData::lightLimitFixSettings.ContactShadowMaxSteps * (1.0 - saturate(viewDepth / SharedData::lightLimitFixSettings.ContactShadowMaxDistance));
			steps = (uint)round(requestedSteps);
			strengthScale = saturate(2.0 * requestedSteps - 1.0);
		}
		return steps;
	}
}
