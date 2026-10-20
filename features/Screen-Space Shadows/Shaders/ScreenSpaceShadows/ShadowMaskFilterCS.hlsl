Texture2D<float4> ShadowMask : register(t0);
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t1);
#else
Texture2D<unorm float> DepthTexture : register(t1);
#endif

#if defined(SHADOW_MASK_UNORM)
RWTexture2D<unorm float4> FilteredShadowMask : register(u0);
#else
RWTexture2D<float4> FilteredShadowMask : register(u0);
#endif

cbuffer ShadowMaskFilterCB : register(b0)
{
	float4 CameraData;
	float2 DepthScale;
	float2 RenderSize;
	float FarDepthValue;
	uint ReverseZ;
	float2 pad0;
};

static const int GroupSize = 8;
static const int FilterRadius = 2;
static const int TileSize = GroupSize + 2 * FilterRadius;
static const float SpatialSigma = 1.5;
static const float RelativeDepthTolerance = 0.015;

groupshared float TileDepth[TileSize * TileSize];
groupshared float4 TileMask[TileSize * TileSize];

int2 ClampToRender(int2 texel)
{
	return clamp(texel, 0, int2(RenderSize) - 1);
}

float LoadLinearDepth(int2 texel, out bool isSky)
{
	float depth = DepthTexture.Load(int3(int2((texel + 0.5) * DepthScale), 0));
	isSky = depth == FarDepthValue;
	return ReverseZ ? CameraData.w / (depth * CameraData.z + CameraData.y) : CameraData.w / (-depth * CameraData.z + CameraData.x);
}

uint GetTileIndex(int2 tileTexel)
{
	return tileTexel.y * TileSize + tileTexel.x;
}

float GetDepthGradient(float centerDepth, int2 tileTexel, int2 axis)
{
	float backward = centerDepth - abs(TileDepth[GetTileIndex(tileTexel - axis)]);
	float forward = abs(TileDepth[GetTileIndex(tileTexel + axis)]) - centerDepth;
	return abs(backward) < abs(forward) ? backward : forward;
}

[numthreads(GroupSize, GroupSize, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	int2 tileOrigin = int2(groupID.xy) * GroupSize - FilterRadius;
	for (uint i = groupIndex; i < uint(TileSize * TileSize); i += uint(GroupSize * GroupSize)) {
		int2 tileTexel = ClampToRender(tileOrigin + int2(i % uint(TileSize), i / uint(TileSize)));
		bool isSky;
		float depth = LoadLinearDepth(tileTexel, isSky);
		TileDepth[i] = isSky ? -depth : depth;
		TileMask[i] = ShadowMask[tileTexel];
	}
	GroupMemoryBarrierWithGroupSync();

	if (any(dispatchID.xy >= (uint2)RenderSize))
		return;

	int2 texel = int2(dispatchID.xy);
	int2 tileCenter = int2(groupThreadID.xy) + FilterRadius;
	uint centerIndex = GetTileIndex(tileCenter);
	float4 center = TileMask[centerIndex];
	float centerDepth = TileDepth[centerIndex];

	if (centerDepth < 0.0) {
		FilteredShadowMask[texel] = center;
		return;
	}

	float2 depthGradient = float2(GetDepthGradient(centerDepth, tileCenter, int2(1, 0)), GetDepthGradient(centerDepth, tileCenter, int2(0, 1)));
	float rangeFalloff = rcp(max(RelativeDepthTolerance * RelativeDepthTolerance * centerDepth * centerDepth, 1e-8));
	float spatialFalloff = rcp(2.0 * SpatialSigma * SpatialSigma);

	float4 filtered = center;
	float weightSum = 1.0;

	[unroll] for (int y = -FilterRadius; y <= FilterRadius; y++)
	{
		[unroll] for (int x = -FilterRadius; x <= FilterRadius; x++)
		{
			if (x == 0 && y == 0)
				continue;

			uint sampleIndex = GetTileIndex(tileCenter + int2(x, y));
			float sampleDepth = TileDepth[sampleIndex];
			float2 sampleOffset = float2(ClampToRender(texel + int2(x, y)) - texel);
			float residual = sampleDepth - (centerDepth + dot(sampleOffset, depthGradient));
			float weight = sampleDepth < 0.0 ? 0.0 : exp(-float(x * x + y * y) * spatialFalloff - residual * residual * rangeFalloff);

			filtered += TileMask[sampleIndex] * weight;
			weightSum += weight;
		}
	}

	FilteredShadowMask[texel] = filtered / weightSum;
}
