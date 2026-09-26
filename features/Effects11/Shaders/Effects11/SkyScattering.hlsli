#ifndef EFFECTS11_SKY_SCATTERING_HLSLI
#define EFFECTS11_SKY_SCATTERING_HLSLI

#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

#if defined(CLOUDS) && defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

namespace SkyScattering
{
	static const float CloudLayerHeight = 2e3 / GAME_UNIT_TO_M;
	static const float PlanetRadius = 6371e3 / GAME_UNIT_TO_M;

	struct Light
	{
		float3 direction;
		float3 color;
		float weight;
	};

	float3 SafeNormalize(float3 v)
	{
		return v * rsqrt(max(dot(v, v), 1e-8));
	}

	float HorizonFade(float z)
	{
		return smoothstep(-0.1, 0.02, z);
	}

	float3 GetChroma(float3 color)
	{
		return max(color, 0.0) / max(max(color.r, max(color.g, color.b)), 1e-4);
	}

	float PhaseHGPeak(float cosTheta, float g)
	{
		float x = saturate((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cosTheta, 1e-6));
		return x * sqrt(x);
	}

	float GetSunWeight()
	{
		return saturate(SharedData::SunColor.w) * HorizonFade(SharedData::SunDirection.z);
	}

	float GetMoonPresence()
	{
		float sunBelowHorizon = 1.0 - smoothstep(-0.2, -0.1, SharedData::SunDirection.z);
		float sunFadedOut = SharedData::SunColor.w > 0.0 ? 0.0 : smoothstep(0.0, 0.1, SharedData::SunDirection.z);
		return max(sunBelowHorizon, sunFadedOut);
	}

	bool UseMasser()
	{
		float masser = dot(max(SharedData::MasserColor.xyz, 0.0), 1.0 / 3.0) * HorizonFade(SharedData::MasserDirection.z);
		float secunda = dot(max(SharedData::SecundaColor.xyz, 0.0), 1.0 / 3.0) * HorizonFade(SharedData::SecundaDirection.z);
		return masser >= secunda;
	}

	Light GetLight()
	{
		Light light;
		float sunWeight = GetSunWeight();
		if (sunWeight > 0.0) {
			light.direction = SafeNormalize(SharedData::SunDirection.xyz);
			light.color = lerp(1.0.xxx, GetChroma(SharedData::SunColor.xyz), SharedData::enbSettings.SkyScatteringColorFromSun);
			light.weight = sunWeight;
		} else {
			bool useMasser = UseMasser();
			float3 moonDirection = useMasser ? SharedData::MasserDirection.xyz : SharedData::SecundaDirection.xyz;
			float3 moonColor = max(useMasser ? SharedData::MasserColor.xyz : SharedData::SecundaColor.xyz, 0.0);
			float moonBrightness = max(moonColor.r, max(moonColor.g, moonColor.b));
			float3 moonChroma = GetChroma(moonColor);
			light.direction = SafeNormalize(moonDirection);
			light.color = lerp(dot(moonChroma, 1.0 / 3.0).xxx, moonChroma, SharedData::enbSettings.SkyScatteringColorFromSun);
			light.weight = GetMoonPresence() * HorizonFade(light.direction.z) * SharedData::enbSettings.SkyScatteringMoonGlowAmount * moonBrightness;
		}
		light.color *= SharedData::enbSettings.SkyScatteringColor * SharedData::enbSettings.SkyScatteringIntensity;
		return light;
	}

	float GetCloudLayerDistance(float3 viewDirection)
	{
		float b = PlanetRadius * viewDirection.z;
		float c = CloudLayerHeight * (2.0 * PlanetRadius + CloudLayerHeight);
		float root = sqrt(b * b + c);
		return b >= 0.0 ? c / (b + root) : root - b;
	}

	float GetRayLength(float3 viewDirection, float depth, float3 positionMS)
	{
		float cloudDistance = GetCloudLayerDistance(viewDirection);
#ifdef REVERSE_Z
		return depth > 0.0 ? min(length(positionMS), cloudDistance) : cloudDistance;
#else
		return depth < 1.0 ? min(length(positionMS), cloudDistance) : cloudDistance;
#endif
	}

	float GetOpticalDepth(float distance, float viewZ)
	{
		float extinction = SharedData::enbSettings.SkyScatteringExtinction;
		float k = max(viewZ, 0.0) / SharedData::enbSettings.SkyScatteringScaleHeight;
		float x = k * distance;
		return x < 1e-3 ? extinction * distance * (1.0 - 0.5 * x) : extinction * (1.0 - exp(-x)) / k;
	}

	float GetDistanceAtOpticalDepth(float opticalDepth, float viewZ)
	{
		float extinction = max(SharedData::enbSettings.SkyScatteringExtinction, 1e-20);
		float k = max(viewZ, 0.0) / SharedData::enbSettings.SkyScatteringScaleHeight;
		float y = opticalDepth * k / extinction;
		return y < 1e-3 ? opticalDepth / extinction * (1.0 + 0.5 * y) : -log(max(1.0 - y, 1e-6)) / k;
	}

	float GetInscatterAmount(float rayLength, float viewZ)
	{
		return 1.0 - exp(-GetOpticalDepth(rayLength, viewZ));
	}

#if defined(CLOUD_SHADOWS)
	float GetCloudTransmittance(float3 samplePosition, float3 lightDirection, SamplerState textureSampler)
	{
		float3 cloudDirection = CloudShadows::GetCloudShadowSampleDir(samplePosition, lightDirection);
		float occlusion = CloudShadows::CloudShadowsTexture.SampleLevel(textureSampler, cloudDirection, 0);
		return 1.0 - sqrt(saturate(occlusion));
	}
#endif

#if defined(CLOUDS)
	static const float IsotropicPhase = 0.25 / Math::PI;
	static const float SunIlluminance = 10.0 / Math::PI;

	float PhaseHG(float cosTheta, float g)
	{
		float g2 = g * g;
		return IsotropicPhase * (1.0 - g2) / pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5);
	}

	float PhaseDraine(float cosTheta, float g, float alpha)
	{
		float g2 = g * g;
		float numerator = (1.0 - g2) * (1.0 + alpha * cosTheta * cosTheta);
		float denominator = pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5) * (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);
		return IsotropicPhase * numerator / denominator;
	}

	float PhaseThomasSchander(float cosTheta)
	{
		float p1 = cosTheta + 8.194068e-01;
		float4 expValues = exp(float4(-6.5e+01 * cosTheta - 5.5e+01, -8.370334e+01 * p1 * p1, 7.810083e+00 * cosTheta, -4.552125e-12 * cosTheta));
		float4 expWeights = float4(9.805233e-06, 1.388198e-01, 2.054747e-03, 2.600563e-02);
		return dot(expValues, expWeights) * 0.25;
	}

	float GetCloudLightOcclusion(float3 viewDirection, float3 lightDirection, SamplerState textureSampler)
	{
		static const float3 PoissonDisc[4] = {
			float3(0.460921, 0.615192, 0.887539),
			float3(0.757347, 0.911008, 0.189581),
			float3(0.548753, 0.145482, 0.0548723),
			float3(0.90051, 0.157048, 0.623493)
		};

		float occlusion = 0.0;
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float3 sampleDirection = SafeNormalize(lerp(viewDirection, lightDirection, (float(i) + 0.5) / 32.0)) + (PoissonDisc[i] * 2.0 - 1.0) * 0.01;
			if (sampleDirection.z < 0.0)
				occlusion += -sampleDirection.z;
#	if defined(CLOUD_SHADOWS)
			else
				occlusion += CloudShadows::CloudSelfShadowTexture.SampleLevel(textureSampler, sampleDirection, 0);
#	endif
		}
		return saturate(occlusion * 0.25);
	}

	float3 DesaturateCloudLight(float3 color)
	{
		return max(lerp(color, dot(color, 1.0 / 3.0), SharedData::enbSettings.CloudsLightingDesaturation), 0.0);
	}

	float3 RelightCloud(float3 cloudColor, float cloudAlpha, float3 viewDirection, SamplerState textureSampler)
	{
		if (cloudAlpha <= 0.0)
			return cloudColor;

		float sunWeight = GetSunWeight();
		float silverLiningMix = SharedData::enbSettings.CalculateCloudsEdgeFromScattering ? saturate(0.5 * SharedData::enbSettings.CloudsEdgeIntensity) : 0.0;
		float originalMix = 1.0;
		float relightMix;
		float3 lightDirection;
		float3 lightColor;
		if (sunWeight > 0.0) {
			relightMix = max(SharedData::enbSettings.CloudsLightingSunMultiplier, 0.0);
			originalMix = 1.0 - 0.5 * saturate(relightMix) * sunWeight;
			lightDirection = SafeNormalize(SharedData::SunDirection.xyz);
			lightColor = GetChroma(SharedData::DirLightColor.xyz) * (SunIlluminance * sunWeight);
		} else {
			relightMix = SharedData::enbSettings.EnableCloudsLightingFromMoon ? max(SharedData::enbSettings.CloudsLightingMoonIntensity, 0.0) : 0.0;
			silverLiningMix *= SharedData::enbSettings.CloudsEdgeMoonMultiplier;
			lightDirection = SafeNormalize(UseMasser() ? SharedData::MasserDirection.xyz : SharedData::SecundaDirection.xyz);
			lightColor = max(SharedData::DirLightColor.xyz, 0.0) * (GetMoonPresence() * HorizonFade(lightDirection.z));
		}

		[branch] if (relightMix <= 0.0) return cloudColor * originalMix;

		lightColor = DesaturateCloudLight(lightColor);
		float forwardScattering = SharedData::enbSettings.CloudsLightingForwardScattering;
		float cosTheta = dot(viewDirection, lightDirection);

		float lightVisibility = pow(max(1.0 - GetCloudLightOcclusion(viewDirection, lightDirection, textureSampler), 1e-6), SharedData::enbSettings.CloudsLightingDensity);
		float directVisibility = lerp(SharedData::enbSettings.CloudsLightingSunMinIntensity, 1.0, lightVisibility);

		float opticalDepth = -log(max(1.0 - cloudAlpha, 1e-3));
		float cloudPhase = max(0.0, lerp(IsotropicPhase, lerp(PhaseThomasSchander(cosTheta), IsotropicPhase, saturate(cloudAlpha)), forwardScattering)) * Math::TAU * relightMix;
		float forwardPhase = forwardScattering * (max(0.0, PhaseHG(cosTheta, 0.94) - IsotropicPhase) + 0.45 * max(0.0, PhaseDraine(cosTheta, 0.78, 2.0) - IsotropicPhase));
		float silverEdgeMask = smoothstep(0.08, 0.35, cloudAlpha) * (1.0 - smoothstep(0.45, 0.85, cloudAlpha));
		float silverScattering = 1.35 * silverEdgeMask * (1.0 - exp(-opticalDepth)) * exp(-0.5 * opticalDepth);
		float directScattering = 0.9 * opticalDepth * exp(-0.75 * opticalDepth);

		float3 relit = cloudColor * originalMix;
		relit += cloudColor * lightColor * directVisibility * (forwardPhase * silverScattering * silverLiningMix * relightMix + cloudPhase * directScattering);

#	if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			float3 cloudNormal = -viewDirection;
			float skyVisibility = saturate(cloudNormal.z * 0.5 + 0.5) * exp(-opticalDepth);
			float3 vanillaAmbient = Color::Ambient(max(0.0, SharedData::GetAmbient(cloudNormal)));
			float3 iblAmbient = DesaturateCloudLight(ImageBasedLighting::GetDiffuseIBLOccluded(vanillaAmbient, viewDirection, skyVisibility));
			float iblFill = cloudAlpha * exp(-0.35 * opticalDepth) * lerp(0.25, 1.0, 1.0 - lightVisibility);
			relit += cloudColor * iblAmbient * iblFill * relightMix;
		}
#	endif

		return relit;
	}
#endif
}

#endif
