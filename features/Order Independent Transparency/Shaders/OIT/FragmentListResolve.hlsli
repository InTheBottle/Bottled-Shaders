#ifndef __FRAGMENT_LIST_RESOLVE__
#define __FRAGMENT_LIST_RESOLVE__

#include "OIT/FragmentList.hlsli"

Texture2D<float> TexWaterDepth : register(t0);

#if defined(OIT_DEBUG)
bool OIT_Resolve(uint2 address, out float4 front, out float4 composite)
{
	static const float3 colors[8] = {
		float3(0.1, 0.1, 1.0),
		float3(0.1, 0.4, 0.8),
		float3(0.1, 0.8, 0.4),
		float3(0.1, 1.0, 0.1),
		float3(0.5, 0.5, 0.1),
		float3(0.8, 0.4, 0.1),
		float3(1.0, 0.1, 0.1),
		float3(0.8, 0.1, 0.8),
	};

	front = float4(0.0, 0.0, 0.0, 1.0);
	composite = float4(0.0, 0.0, 0.0, 1.0);

	uint nodeAddress = gFragmentListHeadSRV[address];
	uint count = 0;
	[loop] while (nodeAddress != 0 && count < 8)
	{
		nodeAddress = gFragmentListNodesSRV[nodeAddress].next;
		count++;
	}

	if (count > 0)
		front.xyz = colors[count - 1];
	return count > 0;
}
#else
bool OIT_Precedes(float nearness, uint2 color, float otherNearness, uint2 otherColor)
{
	if (nearness != otherNearness)
		return nearness > otherNearness;
	if (color.x != otherColor.x)
		return color.x > otherColor.x;
	return color.y > otherColor.y;
}

struct OITTail
{
	float3 premultiplied;
	float alphaSum;
	float transmittance;
	float3 additive;
};

void OIT_AddToTail(inout OITTail tail, float4 layer)
{
	if (layer.w > 0.0) {
		tail.premultiplied += layer.xyz;
		tail.alphaSum += layer.w;
		tail.transmittance *= saturate(1.0 - layer.w);
	} else {
		tail.additive += layer.xyz;
	}
}

float3 OIT_TailColor(OITTail tail)
{
	float3 blended = tail.alphaSum > 0.0 ? tail.premultiplied / tail.alphaSum * (1.0 - tail.transmittance) : 0.0;
	return blended + tail.additive;
}

bool OIT_Resolve(uint2 address, out float4 front, out float4 composite)
{
	front = float4(0.0, 0.0, 0.0, 1.0);
	composite = float4(0.0, 0.0, 0.0, 1.0);

	uint nodeAddress = gFragmentListHeadSRV[address];
	bool covered = nodeAddress != 0;
	[branch] if (covered)
	{
		float waterNearness = OIT_Nearness(TexWaterDepth[address]) - OIT_DEPTH_EPSILON;

		float layerNearness[OIT_NODE_COUNT];
		uint2 layerColor[OIT_NODE_COUNT];
		[unroll] for (uint i = 0; i < OIT_NODE_COUNT; i++)
		{
			layerNearness[i] = -1.0;
			layerColor[i] = 0;
		}

		OITTail tail = { 0.0.xxx, 0.0, 1.0, 0.0.xxx };
		OITTail frontTail = tail;

		[loop] while (nodeAddress != 0)
		{
			FragmentListNode node = gFragmentListNodesSRV[nodeAddress];
			nodeAddress = node.next;

			float nearness = node.nearness;
			uint2 packed = node.packedColor;
			[unroll] for (uint j = 0; j < OIT_NODE_COUNT; j++)
			{
				bool precedes = OIT_Precedes(nearness, packed, layerNearness[j], layerColor[j]);
				float displacedNearness = layerNearness[j];
				uint2 displacedColor = layerColor[j];
				layerNearness[j] = precedes ? nearness : displacedNearness;
				layerColor[j] = precedes ? packed : displacedColor;
				nearness = precedes ? displacedNearness : nearness;
				packed = precedes ? displacedColor : packed;
			}

			[branch] if (nearness >= 0.0)
			{
				float4 evicted = FL_UnpackColor(packed);
				OIT_AddToTail(tail, evicted);
				if (nearness >= waterNearness)
					OIT_AddToTail(frontTail, evicted);
			}
		}

		float3 accumulated = 0.0;
		float transmittance = 1.0;
		[unroll] for (uint k = 0; k < OIT_NODE_COUNT; k++)
		{
			[branch] if (layerNearness[k] >= 0.0)
			{
				float4 layer = FL_UnpackColor(layerColor[k]);
				accumulated += transmittance * layer.xyz;
				transmittance *= saturate(1.0 - layer.w);
				if (layerNearness[k] >= waterNearness)
					front = float4(accumulated, transmittance);
			}
		}

		front.xyz += front.w * OIT_TailColor(frontTail);
		front.w *= frontTail.transmittance;

		accumulated += transmittance * OIT_TailColor(tail);
		transmittance *= tail.transmittance;
		composite = float4(accumulated, transmittance);
	}
	return covered;
}
#endif

#endif
