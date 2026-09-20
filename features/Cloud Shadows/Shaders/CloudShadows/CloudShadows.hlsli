#ifndef CLOUD_SHADOWS_HLSLI
#define CLOUD_SHADOWS_HLSLI

#include "Common/Game.hlsli"

namespace CloudShadows
{
	TextureCube<float> CloudShadowsTexture : register(t25);

#if defined(CLOUD_SELF_SHADOW)
	// Only bound while a cloud deck is being drawn. Holds the decks drawn above this one.
	TextureCube<float> CloudsAboveTexture : register(t26);
#endif

	const static float CloudHeight = (2e3f / GAME_UNIT_TO_M);
	const static float PlanetRadius = (6371e3f / GAME_UNIT_TO_M);
	const static float RcpHPlusR = (1.0 / (CloudHeight + PlanetRadius));

	float3 GetCloudShadowSampleDir(float3 rel_pos, float3 eye_to_sun)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, eye_to_sun);
		float lengthsqr = dot(p, p);
		float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
		float3 v = (p + eye_to_sun * t) * (r + CloudHeight) - float3(0, 0, r);
		return v;
	}

	float GetCloudShadowMult(float3 worldPosition, SamplerState textureSampler)
	{
		float3 cloudSampleDir = GetCloudShadowSampleDir(worldPosition, SharedData::DirLightDirection.xyz).xyz;
		float cloudCubeSample = CloudShadowsTexture.SampleLevel(textureSampler, cloudSampleDir, 0).x;
		return saturate(1.0 - cloudCubeSample * SharedData::cloudShadowsSettings.Opacity);
	}

#if defined(CLOUD_SELF_SHADOW)
	// The occlusion cubemap accumulates with source-alpha blending, so a deck writes
	// its coverage squared. Undo that to get back to something linear in density.
	float GetCloudDensity(TextureCube<float> occlusionTexture, float3 sampleDirection, SamplerState textureSampler)
	{
		return sqrt(saturate(occlusionTexture.SampleLevel(textureSampler, sampleDirection, 0)));
	}

	// Angular span of the march towards the light, in radians. The occlusion cubemap
	// places every deck on one 2km shell, so there is no parallax between decks to
	// march through; this stands in for it.
	const static float SelfShadowArc = 0.2;

	/**
	 * @brief Estimates how much cloud sits between a cloud texel and the directional light.
	 * @param viewDirection Normalised direction from the eye to the cloud texel.
	 * @param lightDirection Normalised direction towards the sun or moon.
	 * @param strength How far the result is allowed to darken the texel, 0 to 1.
	 * @return Multiplier for the cloud colour, 1 when nothing shadows the texel.
	 */
	float GetCloudSelfShadow(float3 viewDirection, float3 lightDirection, float strength, SamplerState textureSampler)
	{
		const static uint sampleCount = 4;

		float selfDensity = 0.0;
		float sunwardDensity = 0.0;

		[unroll] for (uint i = 0; i < sampleCount; ++i)
		{
			float t = (float(i) + 0.5) / float(sampleCount);
			float3 sampleDirection = normalize(lerp(viewDirection, lightDirection, t * t * SelfShadowArc));
			float density = GetCloudDensity(CloudShadowsTexture, sampleDirection, textureSampler);
			if (i == 0)
				selfDensity = density;
			sunwardDensity += density;
		}
		sunwardDensity /= float(sampleCount);

		// Only density in excess of the texel's own shadows it, so an even overcast stays
		// flat and structure within a deck is what picks up shading.
		float occlusion = saturate(sunwardDensity - selfDensity);

		// Decks drawn above this one shade it outright, including when the light is high
		// enough that the march above barely leaves the texel.
		float3 aboveDirection = normalize(lerp(viewDirection, lightDirection, 0.5 * SelfShadowArc));
		occlusion = saturate(occlusion + GetCloudDensity(CloudsAboveTexture, aboveDirection, textureSampler));

		// The march crosses the horizon once the light sits on it, where the cubemap holds
		// no cloud at all, so fade out rather than sample through it.
		occlusion *= smoothstep(-0.05, 0.1, lightDirection.z);

		return 1.0 - strength * occlusion;
	}
#endif
}

#endif
