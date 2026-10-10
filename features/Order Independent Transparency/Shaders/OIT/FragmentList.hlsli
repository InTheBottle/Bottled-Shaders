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

#ifndef H_FRAGMENT_LIST
#define H_FRAGMENT_LIST

#include "OIT/OITCommon.hlsli"

struct FragmentListNode
{
	uint next;
	float nearness;
	uint2 packedColor;
};

static const float FL_HALF_MAX = 65504.0;

uint FL_PackHalf2(float2 value)
{
	uint2 packed = f32tof16(clamp(value, 0.0.xx, FL_HALF_MAX.xx));
	return packed.x | (packed.y << 16);
}

float2 FL_UnpackHalf2(uint packed)
{
	return f16tof32(uint2(packed & 0xFFFF, packed >> 16));
}

uint2 FL_PackColor(float4 color)
{
	return uint2(FL_PackHalf2(color.xy), FL_PackHalf2(color.zw));
}

float4 FL_UnpackColor(uint2 packed)
{
	return float4(FL_UnpackHalf2(packed.x), FL_UnpackHalf2(packed.y));
}

#if defined(OIT_RESOLVE)
Texture2D<uint> gFragmentListHeadSRV : register(t1);
StructuredBuffer<FragmentListNode> gFragmentListNodesSRV : register(t2);
#else
RWTexture2D<uint> gFragmentListHeadUAV : register(u4);
RWStructuredBuffer<FragmentListNode> gFragmentListNodesUAV : register(u5);

bool OIT_CaptureImpl(int2 screenAddress, float4 color, float depth)
{
	uint nodeAddress = gFragmentListNodesUAV.IncrementCounter();
	uint maxNodes, stride;
	gFragmentListNodesUAV.GetDimensions(maxNodes, stride);
	if (nodeAddress >= maxNodes)
		return false;

	uint previousHead;
	InterlockedExchange(gFragmentListHeadUAV[screenAddress], nodeAddress, previousHead);

	FragmentListNode node;
	node.next = previousHead;
	node.nearness = OIT_Nearness(depth);
	node.packedColor = FL_PackColor(color);
	gFragmentListNodesUAV[nodeAddress] = node;
	return true;
}
#endif

#endif
