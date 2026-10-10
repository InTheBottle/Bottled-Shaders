/////////////////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/////////////////////////////////////////////////////////////////////////////////////////////

#ifndef H_AOIT
#define H_AOIT

#include "OIT/OITCommon.hlsli"

uint2 AOITPackColor(float3 color, float transmittance)
{
	color = clamp(color, 0.0.xxx, 65504.0.xxx);
	uint2 packedRG = f32tof16(color.xy);
	uint2 packedBA = f32tof16(float2(color.z, saturate(transmittance)));
	return uint2(packedRG.x | (packedRG.y << 16), packedBA.x | (packedBA.y << 16));
}

float4 AOITUnpackColorAndTransmittance(uint2 packedColor)
{
	float2 rg = f16tof32(uint2(packedColor.x & 0xFFFF, packedColor.x >> 16));
	float2 ba = f16tof32(uint2(packedColor.y & 0xFFFF, packedColor.y >> 16));
	return float4(rg, ba);
}

struct AOITCtrlSurface
{
	bool clear;
};

struct AOITSPData
{
	float4 depth[OIT_RT_COUNT];
	uint2 color[OIT_NODE_COUNT];
};

struct AOITSPDepthData
{
	float4 depth[OIT_RT_COUNT];
};

struct AOITSPColorData
{
	uint2 color[OIT_NODE_COUNT];
};

struct ATSPNode
{
	float depth;
	float trans;
	float3 color;
};

uint AOITAddrGen(uint2 addr2D, uint surfaceWidth)
{
#ifdef OIT_TILED_ADDRESSING
	surfaceWidth = (surfaceWidth + 1U) >> 1U;
	uint2 tileAddr2D = addr2D >> 1U;
	uint tileAddr1D = (tileAddr2D[0] + surfaceWidth * tileAddr2D[1]) << 2U;
	uint2 pixelAddr2D = addr2D & 0x1U;
	uint pixelAddr1D = (pixelAddr2D[1] << 1U) + pixelAddr2D[0];
	return tileAddr1D | pixelAddr1D;
#else
	return addr2D[0] + surfaceWidth * addr2D[1];
#endif
}

void AOITUnpackNodes(AOITSPData data, out ATSPNode nodeArray[OIT_NODE_COUNT])
{
	[unroll] for (uint i = 0; i < OIT_RT_COUNT; i++)
	{
		[unroll] for (uint j = 0; j < 4; j++)
		{
			float4 colorAndTransmittance = AOITUnpackColorAndTransmittance(data.color[4 * i + j]);
			ATSPNode node = { data.depth[i][j], colorAndTransmittance.w, colorAndTransmittance.xyz };
			nodeArray[4 * i + j] = node;
		}
	}
}

#if defined(OIT_RESOLVE)
Texture2D<uint> gAOITSPClearMaskSRV : register(t1);
StructuredBuffer<AOITSPColorData> gAOITSPColorDataSRV : register(t2);
StructuredBuffer<AOITSPDepthData> gAOITSPDepthDataSRV : register(t3);

void AOITLoadControlSurfaceSRV(uint2 pixelAddr, inout AOITCtrlSurface surface)
{
	surface.clear = (gAOITSPClearMaskSRV[pixelAddr] & 0x1) == 0;
}

void AOITSPLoadDataSRV(uint2 pixelAddr, out ATSPNode nodeArray[OIT_NODE_COUNT])
{
	uint2 dim;
	gAOITSPClearMaskSRV.GetDimensions(dim[0], dim[1]);
	uint addr = AOITAddrGen(pixelAddr, dim[0]);

	AOITSPData data;
	data.color = gAOITSPColorDataSRV[addr].color;
	data.depth = gAOITSPDepthDataSRV[addr].depth;
	AOITUnpackNodes(data, nodeArray);
}
#else
RasterizerOrderedTexture2D<uint> gAOITSPClearMaskUAV : register(u4);
RasterizerOrderedStructuredBuffer<AOITSPColorData> gAOITSPColorDataUAV : register(u5);
RasterizerOrderedStructuredBuffer<AOITSPDepthData> gAOITSPDepthDataUAV : register(u6);

uint AOITAddrGenUAV(uint2 addr2D)
{
	uint2 dim;
	gAOITSPClearMaskUAV.GetDimensions(dim[0], dim[1]);
	return AOITAddrGen(addr2D, dim[0]);
}

void AOITSPInsertFragment(float fragmentDepth, float fragmentTrans, float3 fragmentColor, inout ATSPNode nodeArray[OIT_NODE_COUNT])
{
	int i;

	float depth[OIT_NODE_COUNT + 1];
	float trans[OIT_NODE_COUNT + 1];
	float3 color[OIT_NODE_COUNT + 1];

	[unroll] for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		depth[i] = nodeArray[i].depth;
		trans[i] = nodeArray[i].trans;
		color[i] = nodeArray[i].color;
	}

	int index = 0;
	float prevTrans = 1;
	[unroll] for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		if (depth[i] < fragmentDepth) {
			index++;
			prevTrans = trans[i];
		}
	}

	[unroll] for (i = OIT_NODE_COUNT - 1; i >= 0; --i)
	{
		[flatten] if (i >= index)
		{
			depth[i + 1] = depth[i];
			trans[i + 1] = trans[i] * fragmentTrans;
			color[i + 1] = color[i];
		}
	}

	depth[index] = fragmentDepth;
	trans[index] = fragmentTrans * prevTrans;
	color[index] = fragmentColor;

	[flatten] if (depth[OIT_NODE_COUNT] != OIT_EMPTY_NODE_DEPTH)
	{
		color[OIT_NODE_COUNT - 1] += color[OIT_NODE_COUNT] * trans[OIT_NODE_COUNT - 1] * rcp(max(trans[OIT_NODE_COUNT - 2], 1e-6));
		trans[OIT_NODE_COUNT - 1] = trans[OIT_NODE_COUNT];
	}

	[unroll] for (i = 0; i < OIT_NODE_COUNT; ++i)
	{
		nodeArray[i].depth = depth[i];
		nodeArray[i].trans = trans[i];
		nodeArray[i].color = color[i];
	}
}

void WriteNewPixelToAOIT(uint2 pixelAddr, float surfaceDepth, float4 surfaceColor)
{
	uint addr = AOITAddrGenUAV(pixelAddr);

	[branch] if ((gAOITSPClearMaskUAV[pixelAddr] & 0x1) == 0)
	{
		uint2 emptyColor = AOITPackColor(0.0.xxx, 1.0 - surfaceColor.w);
		AOITSPDepthData depthData;
		AOITSPColorData colorData;
		[unroll] for (uint i = 0; i < OIT_RT_COUNT; i++)
		{
			depthData.depth[i] = OIT_EMPTY_NODE_DEPTH;
			colorData.color[4 * i] = emptyColor;
			colorData.color[4 * i + 1] = emptyColor;
			colorData.color[4 * i + 2] = emptyColor;
			colorData.color[4 * i + 3] = emptyColor;
		}
		depthData.depth[0][0] = surfaceDepth;
		colorData.color[0] = AOITPackColor(surfaceColor.xyz, 1.0 - surfaceColor.w);

		gAOITSPDepthDataUAV[addr] = depthData;
		gAOITSPColorDataUAV[addr] = colorData;
		gAOITSPClearMaskUAV[pixelAddr] = 1;
	}
	else
	{
		AOITSPData data;
		data.color = gAOITSPColorDataUAV[addr].color;
		data.depth = gAOITSPDepthDataUAV[addr].depth;

		ATSPNode nodeArray[OIT_NODE_COUNT];
		AOITUnpackNodes(data, nodeArray);
		AOITSPInsertFragment(surfaceDepth, 1.0 - surfaceColor.w, surfaceColor.xyz, nodeArray);

		AOITSPDepthData depthData;
		AOITSPColorData colorData;
		[unroll] for (uint i = 0; i < OIT_RT_COUNT; i++)
		{
			[unroll] for (uint j = 0; j < 4; j++)
			{
				depthData.depth[i][j] = nodeArray[4 * i + j].depth;
				colorData.color[4 * i + j] = AOITPackColor(nodeArray[4 * i + j].color, nodeArray[4 * i + j].trans);
			}
		}
		gAOITSPDepthDataUAV[addr] = depthData;
		gAOITSPColorDataUAV[addr] = colorData;
	}
}

bool OIT_CaptureImpl(int2 screenAddress, float4 color, float depth)
{
	WriteNewPixelToAOIT(uint2(screenAddress), OIT_SortKey(depth), color);
	return true;
}
#endif

#endif
