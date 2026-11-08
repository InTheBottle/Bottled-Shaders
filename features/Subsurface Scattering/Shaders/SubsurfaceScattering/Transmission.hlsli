#ifndef SSS_TRANSMISSION_HLSLI
#define SSS_TRANSMISSION_HLSLI

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

namespace SubsurfaceScattering
{
	Texture2DArray<float> SunShadowMap : register(t47);
	Texture2DArray<float> SunShadowMapESRAM : register(t48);

	bool HasSunTransmission()
	{
		return SharedData::subsurfaceScatteringSettings.Transmission.x > 0.0 && SharedData::subsurfaceScatteringSettings.TransmissionParams.x > 0.5 &&
		       SharedData::HasDirectionalShadows && !SharedData::InInterior && !SharedData::InMapMenu;
	}

	float GetOcclusionGate(float occluderDepth, float receiverDepth, float depthScale)
	{
		float occlusionDistance = SharedData::subsurfaceScatteringSettings.TransmissionParams.y;
		float thickness = max(receiverDepth - occluderDepth, 0.0) * depthScale;
		return 1.0 - smoothstep(occlusionDistance * 0.25, occlusionDistance, thickness);
	}

	float GetESRAMGate(float3 receiverLS, uint cascade, float depthScale)
	{
		uint width, height, slices;
		SunShadowMapESRAM.GetDimensions(width, height, slices);
		int2 texel = clamp((int2)(receiverLS.xy * float2(width, height)), 0, int2(width, height) - 1);
		return GetOcclusionGate(SunShadowMapESRAM.Load(int4(texel, cascade, 0)), receiverLS.z, depthScale);
	}

	float GetSunTransmissionVisibility(float3 positionWS, float3 normalWS)
	{
		DirectionalShadowLightData shadowData = DirectionalShadowLights[0];
		float viewDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(positionWS));
		if (viewDepth >= shadowData.EndSplitDistances.y)
			return -1.0;

		float splitRange = max(shadowData.EndSplitDistances.x - shadowData.StartSplitDistances.y, 1e-4);
		uint cascade = saturate((viewDepth - shadowData.StartSplitDistances.y) / splitRange) >= 1.0 ? 1 : 0;

		float3 receiverWS = positionWS - normalWS * SharedData::subsurfaceScatteringSettings.Transmission.w + FrameBuffer::CameraPosAdjust.xyz;
		float4 receiverLS = mul(shadowData.ShadowProj[cascade], float4(receiverWS, 1.0));
		receiverLS.xyz /= receiverLS.w;
		if (any(receiverLS.xy <= 0.0) || any(receiverLS.xy >= 1.0))
			return -1.0;

		float depthScale = length(mul(shadowData.InvShadowProj[cascade], float4(0.0, 0.0, 1.0, 0.0)).xyz);

		uint width, height, slices;
		SunShadowMap.GetDimensions(width, height, slices);
		float2 position = receiverLS.xy * float2(width, height);
		int2 origin = (int2)floor(position - 0.5);
		int2 maxCoord = int2(width, height) - 1;

		float visibility = 0.0;
		float weightSum = 0.0;
		[unroll] for (int y = -1; y <= 2; y++)
		{
			[unroll] for (int x = -1; x <= 2; x++)
			{
				int2 texel = origin + int2(x, y);
				float2 distanceToSample = abs(float2(texel) + 0.5 - position);
				float weight = max(1.5 - distanceToSample.x, 0.0) * max(1.5 - distanceToSample.y, 0.0);
				float occluderDepth = SunShadowMap.Load(int4(clamp(texel, 0, maxCoord), cascade, 0));
				visibility += weight * GetOcclusionGate(occluderDepth, receiverLS.z, depthScale);
				weightSum += weight;
			}
		}
		visibility /= max(weightSum, 1e-5);

		[branch] if (SharedData::subsurfaceScatteringSettings.TransmissionParams.z > 0.5)
			visibility *= GetESRAMGate(receiverLS.xyz, cascade, depthScale);

		return visibility;
	}

	float3 GetSkinTransmittanceProfile(float d)
	{
		float dd = -d * d;
		return float3(0.233, 0.455, 0.649) * exp(dd / 0.0064) +
		       float3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
		       float3(0.118, 0.198, 0.0) * exp(dd / 0.187) +
		       float3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
		       float3(0.358, 0.004, 0.0) * exp(dd / 1.99) +
		       float3(0.078, 0.0, 0.0) * exp(dd / 7.41);
	}

	float3 GetSkinTransmittance(float3 normalWS, float3 lightDirection)
	{
		float backLighting = saturate(SharedData::subsurfaceScatteringSettings.Transmission.y + dot(lightDirection, -normalWS));
		return GetSkinTransmittanceProfile(SharedData::subsurfaceScatteringSettings.Transmission.z) * backLighting;
	}
}

#endif
