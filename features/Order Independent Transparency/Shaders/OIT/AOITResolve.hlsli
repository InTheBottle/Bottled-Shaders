#ifndef __AOIT_RESOLVE__
#define __AOIT_RESOLVE__

#include "OIT/AOIT.hlsli"

Texture2D<float> TexWaterDepth : register(t0);

bool OIT_Resolve(uint2 address, out float4 front, out float4 composite)
{
	front = float4(0.0, 0.0, 0.0, 1.0);
	composite = float4(0.0, 0.0, 0.0, 1.0);

	AOITCtrlSurface ctrlSurface;
	AOITLoadControlSurfaceSRV(address, ctrlSurface);
	bool covered = !ctrlSurface.clear;
	[branch] if (covered)
	{
		float waterKey = OIT_SortKey(TexWaterDepth[address]) + OIT_DEPTH_EPSILON;

		ATSPNode nodeArray[OIT_NODE_COUNT];
		AOITSPLoadDataSRV(address, nodeArray);

		float3 accumulated = 0.0;
		float transmittance = 1.0;
		[unroll] for (uint i = 0; i < OIT_NODE_COUNT; i++)
		{
			accumulated += transmittance * nodeArray[i].color;
			transmittance = nodeArray[i].trans;
			[flatten] if (nodeArray[i].depth <= waterKey)
				front = float4(accumulated, transmittance);
		}

		composite = float4(accumulated, transmittance);
	}
	return covered;
}

#endif
