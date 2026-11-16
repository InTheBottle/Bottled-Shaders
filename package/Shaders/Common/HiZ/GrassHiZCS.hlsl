// Reduce 4x4 blocks to farthest depth (0 near, 1 far).
#ifdef REVERSE_Z
Texture2D<float> SrcDepth : register(t0);
#else
Texture2D<unorm float> SrcDepth : register(t0);
#endif

RWTexture2D<float> HiZ : register(u0);

// Must match HiZPyramid::kDownsampleFactor.
#define HIZ_DOWNSAMPLE_FACTOR 4

cbuffer HiZParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (any(tid.xy >= DstSize))
		return;

	const int2 sourceOrigin = int2(tid.xy) * HIZ_DOWNSAMPLE_FACTOR;

	// Far-depth padding keeps partial tiles from occluding visible geometry.
#ifdef REVERSE_Z
	float maxDepth = 1.0;
#else
	float maxDepth = 0.0;
#endif
	[unroll] for (int y = 0; y < HIZ_DOWNSAMPLE_FACTOR; ++y)
	{
		[unroll] for (int x = 0; x < HIZ_DOWNSAMPLE_FACTOR; ++x)
		{
			const int2 sourcePixel = sourceOrigin + int2(x, y);
#ifdef REVERSE_Z
			maxDepth = min(maxDepth, all(sourcePixel < int2(SrcSize)) ? SrcDepth.Load(int3(sourcePixel, 0)) : 0.0);
#else
			maxDepth = max(maxDepth, all(sourcePixel < int2(SrcSize)) ? SrcDepth.Load(int3(sourcePixel, 0)) : 1.0);
#endif
		}
	}

	HiZ[tid.xy] = maxDepth;
}
