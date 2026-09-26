// Builds the depth pyramid used by light occlusion: half-resolution linear view depth with
// LIGHT_OCCLUSION_MIP_COUNT levels, each texel holding the FARTHEST depth of its footprint.
// A ray counts as blocked only when it is behind everything in a texel, so coarse samples never
// invent shadows at object edges; they only let long rays skip over gaps between point samples.
// One 16x16 group reduces a 32x32 block of full-resolution pixels to all five levels.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT, OFF -> R24_UNORM_X8_TYPELESS (float under reverse Z)
#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepthTexture : register(t0);
#endif

RWTexture2D<float> PyramidMip0 : register(u0);
RWTexture2D<float> PyramidMip1 : register(u1);
RWTexture2D<float> PyramidMip2 : register(u2);
RWTexture2D<float> PyramidMip3 : register(u3);
RWTexture2D<float> PyramidMip4 : register(u4);

cbuffer LightOcclusionPyramidCB : register(b1)
{
	uint2 RenderSize;  // Dynamic-resolution render size in full-resolution pixels
	uint2 pad0;
};

#define GROUP_SIZE 16

groupshared float SharedDepths[GROUP_SIZE * GROUP_SIZE];

uint SharedIndex(uint2 coord)
{
	return coord.y * GROUP_SIZE + coord.x;
}

void WritePyramid(uint level, uint2 coord, float depth)
{
	// level is a compile-time constant at every call site (unrolled loop), so this folds to one store
	if (level == 1)
		PyramidMip1[coord] = depth;
	else if (level == 2)
		PyramidMip2[coord] = depth;
	else if (level == 3)
		PyramidMip3[coord] = depth;
	else
		PyramidMip4[coord] = depth;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint3 dispatchID : SV_DispatchThreadID) {
	const uint2 maxPixel = max(RenderSize, 1) - 1;
	const uint2 basePixel = dispatchID.xy * 2;

	float farthest = 0.0;
	[unroll] for (uint y = 0; y < 2; y++)
	{
		[unroll] for (uint x = 0; x < 2; x++)
		{
			const uint2 pixel = min(basePixel + uint2(x, y), maxPixel);
			farthest = max(farthest, SharedData::GetScreenDepth(SceneDepthTexture[pixel]));
		}
	}

	if (all(basePixel <= maxPixel))
		PyramidMip0[dispatchID.xy] = farthest;

	SharedDepths[SharedIndex(groupThreadID.xy)] = farthest;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint level = 1; level < 5; level++)
	{
		const uint size = GROUP_SIZE >> level;
		const bool active = all(groupThreadID.xy < size);

		float depth = 0.0;
		if (active) {
			const uint2 source = groupThreadID.xy * 2;
			depth = max(max(SharedDepths[SharedIndex(source)], SharedDepths[SharedIndex(source + uint2(1, 0))]),
				max(SharedDepths[SharedIndex(source + uint2(0, 1))], SharedDepths[SharedIndex(source + uint2(1, 1))]));
		}
		GroupMemoryBarrierWithGroupSync();

		if (active) {
			SharedDepths[SharedIndex(groupThreadID.xy)] = depth;
			const uint2 coord = groupID.xy * size + groupThreadID.xy;
			// Texels past the render area are never sampled; skip them like mip 0 does
			if (all(coord * (2u << level) <= maxPixel))
				WritePyramid(level, coord, depth);
		}
		GroupMemoryBarrierWithGroupSync();
	}
}
