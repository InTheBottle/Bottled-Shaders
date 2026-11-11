#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

struct Capsule
{
	float3 A;
	float Radius;
	float3 B;
	float pad;
};

struct ActorData
{
	float3 Center;
	float BoundRadius;
	float3 LightPosition;
	float LightStrength;
	uint FirstCapsule;
	uint CapsuleCount;
	float2 pad;
};

#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> DepthTexture : register(t0);
#endif
Texture2D<unorm float3> NormalRoughnessTexture : register(t1);
StructuredBuffer<Capsule> Capsules : register(t2);
StructuredBuffer<ActorData> Actors : register(t3);

RWTexture2D<unorm float2> OcclusionRW : register(u0);

cbuffer CapsuleShadowsFrame : register(b0)
{
	uint ActorCount;
	float AmbientStrength;
	float ShadowStrength;
	float pad0;
};

static const float PenumbraSlope = 0.12;
static const float MaxShadowLength = 160.0;
static const float SelfMaskStart = 0.5;
static const float SelfMaskEnd = 1.0;

float3 ClosestPointOnSegment(float3 p, float3 a, float3 b)
{
	float3 axis = b - a;
	return a + axis * saturate(dot(p - a, axis) / max(dot(axis, axis), 1e-4));
}

float GetAmbientVisibility(float3 position, float3 normal, Capsule capsule)
{
	float3 toCapsule = ClosestPointOnSegment(position, capsule.A, capsule.B) - position;
	float distanceSq = max(dot(toCapsule, toCapsule), 1.0);
	float occlusion = saturate(dot(toCapsule, normal)) * capsule.Radius * capsule.Radius / (distanceSq * sqrt(distanceSq));
	return 1.0 - saturate(occlusion);
}

float GetLightVisibility(float3 position, float3 toLight, float lightDistance, Capsule capsule)
{
	float3 nearest = ClosestPointOnSegment(position, capsule.A, capsule.B);
	float selfFade = smoothstep(capsule.Radius * 1.1, capsule.Radius * 2.0, length(nearest - position));
	float result = 1.0;
	if (!(selfFade <= 0.0)) {
		float3 axis = capsule.B - capsule.A;
		float3 fromStart = position - capsule.A;
		float axisDotRay = dot(toLight, axis);
		float axisLengthSq = dot(axis, axis);
		float denominator = max(axisLengthSq - axisDotRay * axisDotRay, 1e-3);
		float along = saturate((dot(fromStart, axis) - dot(fromStart, toLight) * axisDotRay) / denominator);
		float3 onCapsule = capsule.A + axis * along;

		float travel = dot(onCapsule - position, toLight);
		if (!(travel <= 0.0 || travel >= lightDistance)) {
			float3 offset = onCapsule - (position + toLight * travel);
			float penumbra = max(travel * PenumbraSlope, 1.0);
			float visibility = saturate((length(offset) - capsule.Radius + penumbra) / (2.0 * penumbra));
			visibility = visibility * visibility * (3.0 - 2.0 * visibility);

			float lengthFade = 1.0 - saturate(travel / MaxShadowLength);
			result = lerp(1.0, visibility, selfFade * lengthFade);
		}
	}
	return result;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint2 pixel = dispatchID.xy * 2;
	float2 occlusion = 0.0;

	float depth = DepthTexture[pixel];
#if defined(REVERSE_Z)
	bool isSky = depth <= 1e-6;
#else
	bool isSky = depth >= 0.999999;
#endif

	if (!isSky && all(pixel < uint2(SharedData::BufferDim.xy))) {
		float2 uv = (float2(pixel) + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
		float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
		float3 position = positionCS.xyz / positionCS.w + FrameBuffer::CameraPosAdjust.xyz;

		float3 normalVS = GBuffer::DecodeNormal(NormalRoughnessTexture[pixel].xy);
		float3 normal = normalize(mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0.0)).xyz);

		float ambient = 1.0;
		float shadow = 1.0;

		[loop] for (uint actorIndex = 0; actorIndex < ActorCount; actorIndex++)
		{
			ActorData actor = Actors[actorIndex];
			float3 fromCenter = position - actor.Center;
			if (dot(fromCenter, fromCenter) > actor.BoundRadius * actor.BoundRadius)
				continue;

			float3 toLight = actor.LightPosition - position;
			float lightDistance = max(length(toLight), 1.0);
			toLight /= lightDistance;

			uint lastCapsule = actor.FirstCapsule + actor.CapsuleCount;
			float bodyGap = 1e6;
			[loop] for (uint maskIndex = actor.FirstCapsule; maskIndex < lastCapsule; maskIndex++)
			{
				Capsule capsule = Capsules[maskIndex];
				float gap = length(position - ClosestPointOnSegment(position, capsule.A, capsule.B)) - capsule.Radius;
				bodyGap = min(bodyGap, gap / max(capsule.Radius, 1.0));
			}
			float actorWeight = smoothstep(SelfMaskStart, SelfMaskEnd, bodyGap);
			if (actorWeight <= 0.0)
				continue;

			float actorAmbient = 1.0;
			float actorShadow = 1.0;
			[loop] for (uint capsuleIndex = actor.FirstCapsule; capsuleIndex < lastCapsule; capsuleIndex++)
			{
				Capsule capsule = Capsules[capsuleIndex];
				actorAmbient *= GetAmbientVisibility(position, normal, capsule);
				[branch] if (actor.LightStrength > 0.0)
					actorShadow = min(actorShadow, GetLightVisibility(position, toLight, lightDistance, capsule));
			}
			ambient *= lerp(1.0, actorAmbient, actorWeight);
			float facing = saturate(dot(normal, toLight) * 4.0);
			shadow *= lerp(1.0, actorShadow, saturate(actor.LightStrength) * actorWeight * facing);
		}

		occlusion = float2((1.0 - ambient) * AmbientStrength, (1.0 - shadow) * ShadowStrength);
	}

	OcclusionRW[dispatchID.xy] = occlusion;
}
