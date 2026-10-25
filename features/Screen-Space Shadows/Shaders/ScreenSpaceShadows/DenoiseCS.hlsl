#if defined(TERRAIN_BLENDING) || defined(REVERSE_Z)
Texture2D<float> DepthTexture : register(t0);
#else
Texture2D<unorm float> DepthTexture : register(t0);
#endif
Texture2D<unorm float2> InputTexture : register(t1);
RWTexture2D<unorm float2> OutputTexture : register(u0);

cbuffer PerFrame : register(b1)
{
	float4 LightCoordinate;
	int2 WaveOffset;
	float FarDepthValue;
	float NearDepthValue;
	float2 InvDepthTextureSize;
	float2 DynamicRes;
	float SurfaceThickness;
	float BilinearThreshold;
	float ShadowContrast;
};

static const float RelativeDepthTolerance = 0.02;

int2 GetRenderSize()
{
	return int2(round(rcp(InvDepthTextureSize)));
}

bool IsSky(float depth)
{
	return depth == FarDepthValue;
}

float GetDepthScale(float depth)
{
	return rcp(max(abs(depth - FarDepthValue), 1e-7) * RelativeDepthTolerance);
}

float GetWeight(float distanceSq, float spatialFalloff, float residual)
{
	return exp(-distanceSq * spatialFalloff - residual * residual);
}

#if defined(CROSS_RAY)

static const int Radius = 6;
static const float Sigma = 3.0;

float LoadDepth(int2 texel, int2 renderSize)
{
	return DepthTexture[clamp(texel, 0, renderSize - 1)];
}

float GetDepthGradient(int2 texel, int2 axis, float centerDepth, int2 renderSize)
{
	float backward = centerDepth - LoadDepth(texel - axis, renderSize);
	float forward = LoadDepth(texel + axis, renderSize) - centerDepth;
	return abs(backward) < abs(forward) ? backward : forward;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	int2 renderSize = GetRenderSize();
	if (any(dispatchID.xy >= uint2(renderSize)))
		return;

	int2 texel = int2(dispatchID.xy);
	float2 center = InputTexture[texel];
	float centerDepth = DepthTexture[texel];

	if (IsSky(centerDepth)) {
		OutputTexture[texel] = center;
		return;
	}

	float2 depthGradient = float2(GetDepthGradient(texel, int2(1, 0), centerDepth, renderSize), GetDepthGradient(texel, int2(0, 1), centerDepth, renderSize));
	float depthScale = GetDepthScale(centerDepth);
	float spatialFalloff = rcp(2.0 * Sigma * Sigma);

	float2 rayDirection = float2(texel) + 0.5 - LightCoordinate.xy;
	float rayLength = length(rayDirection);
	float2 across = rayLength > 1e-3 ? float2(-rayDirection.y, rayDirection.x) / rayLength : float2(1, 0);

	float2 filtered = center;
	float weightSum = 1.0;

	[unroll] for (int i = -Radius; i <= Radius; i++)
	{
		if (i == 0)
			continue;

		int2 sampleTexel = clamp(texel + int2(round(across * i)), 0, renderSize - 1);
		float sampleDepth = DepthTexture[sampleTexel];
		float residual = (sampleDepth - (centerDepth + dot(float2(sampleTexel - texel), depthGradient))) * depthScale;
		float weight = IsSky(sampleDepth) ? 0.0 : GetWeight(float(i * i), spatialFalloff, residual);

		filtered += InputTexture[sampleTexel] * weight;
		weightSum += weight;
	}

	OutputTexture[texel] = filtered / weightSum;
}

#else

static const int GroupSize = 8;
static const int Radius = 2;
static const int TileSize = GroupSize + 2 * Radius;
static const float Sigma = 1.5;

groupshared float TileDepth[TileSize * TileSize];
groupshared float2 TileValue[TileSize * TileSize];

uint GetTileIndex(int2 tileTexel)
{
	return tileTexel.y * TileSize + tileTexel.x;
}

float GetDepthGradient(float centerDepth, int2 tileTexel, int2 axis)
{
	float backward = centerDepth - TileDepth[GetTileIndex(tileTexel - axis)];
	float forward = TileDepth[GetTileIndex(tileTexel + axis)] - centerDepth;
	return abs(backward) < abs(forward) ? backward : forward;
}

[numthreads(GroupSize, GroupSize, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	int2 renderSize = GetRenderSize();
	int2 tileOrigin = int2(groupID.xy) * GroupSize - Radius;
	for (uint i = groupIndex; i < uint(TileSize * TileSize); i += uint(GroupSize * GroupSize)) {
		int2 tileTexel = clamp(tileOrigin + int2(i % uint(TileSize), i / uint(TileSize)), 0, renderSize - 1);
		TileDepth[i] = DepthTexture[tileTexel];
		TileValue[i] = InputTexture[tileTexel];
	}
	GroupMemoryBarrierWithGroupSync();

	if (any(dispatchID.xy >= uint2(renderSize)))
		return;

	int2 texel = int2(dispatchID.xy);
	int2 tileCenter = int2(groupThreadID.xy) + Radius;
	uint centerIndex = GetTileIndex(tileCenter);
	float2 center = TileValue[centerIndex];
	float centerDepth = TileDepth[centerIndex];

	if (IsSky(centerDepth)) {
		OutputTexture[texel] = center;
		return;
	}

	float2 depthGradient = float2(GetDepthGradient(centerDepth, tileCenter, int2(1, 0)), GetDepthGradient(centerDepth, tileCenter, int2(0, 1)));
	float depthScale = GetDepthScale(centerDepth);
	float spatialFalloff = rcp(2.0 * Sigma * Sigma);

	float2 filtered = center;
	float weightSum = 1.0;

	[unroll] for (int y = -Radius; y <= Radius; y++)
	{
		[unroll] for (int x = -Radius; x <= Radius; x++)
		{
			if (x == 0 && y == 0)
				continue;

			uint sampleIndex = GetTileIndex(tileCenter + int2(x, y));
			float sampleDepth = TileDepth[sampleIndex];
			float2 sampleOffset = float2(clamp(texel + int2(x, y), 0, renderSize - 1) - texel);
			float residual = (sampleDepth - (centerDepth + dot(sampleOffset, depthGradient))) * depthScale;
			float weight = IsSky(sampleDepth) ? 0.0 : GetWeight(float(x * x + y * y), spatialFalloff, residual);

			filtered += TileValue[sampleIndex] * weight;
			weightSum += weight;
		}
	}

	OutputTexture[texel] = filtered / weightSum;
}

#endif
