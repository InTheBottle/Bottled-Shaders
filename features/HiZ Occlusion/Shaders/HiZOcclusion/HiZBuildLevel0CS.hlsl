// HiZBuildLevel0CS.hlsl
// Copies the post z-prepass depth buffer into Hi-Z pyramid mip 0.

#ifdef REVERSE_Z
Texture2D<float> DepthTex : register(t0);
#else
Texture2D<unorm float> DepthTex : register(t0);
#endif
RWTexture2D<float> OutMip0 : register(u0);

#if defined(BUILD_MIP1)
RWTexture2D<float> OutMip1 : register(u1);

float FarthestDepth(float a, float b)
{
#	ifdef REVERSE_Z
	return min(a, b);
#	else
	return max(a, b);
#	endif
}

[numthreads(16, 16, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint w, h;
	OutMip1.GetDimensions(w, h);

	uint2 dst = dispatchThreadID.xy;
	if (dst.x >= w || dst.y >= h)
		return;

	uint sw, sh;
	OutMip0.GetDimensions(sw, sh);

	uint2 srcBase = dst * 2;
	uint2 srcLast = srcBase + 1;
	if (dst.x == w - 1 && (sw & 1))
		srcLast.x += 1;
	if (dst.y == h - 1 && (sh & 1))
		srcLast.y += 1;
	srcLast = min(srcLast, uint2(sw - 1, sh - 1));

	float farthest = DepthTex.Load(int3(srcBase, 0));
	for (uint y = srcBase.y; y <= srcLast.y; ++y) {
		for (uint x = srcBase.x; x <= srcLast.x; ++x) {
			float d = DepthTex.Load(int3(x, y, 0));
			OutMip0[uint2(x, y)] = d;
			farthest = FarthestDepth(farthest, d);
		}
	}

	OutMip1[dst] = farthest;
}
#else
[numthreads(16, 16, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint w, h;
	OutMip0.GetDimensions(w, h);

	uint2 pix = dispatchThreadID.xy;
	if (pix.x >= w || pix.y >= h)
		return;

	float d = DepthTex.Load(int3(pix, 0));
	OutMip0[pix] = d;
}
#endif
