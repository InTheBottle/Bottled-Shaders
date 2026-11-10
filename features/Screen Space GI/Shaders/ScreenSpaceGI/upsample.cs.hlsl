// depth-aware upsampling: https://gist.github.com/pixelmager/a4364ea18305ed5ca707d89ddc5f8743

#include "Common/FastMath.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<half> srcDepth : register(t0);
Texture2D<half> srcAo : register(t1);           // half-res
Texture2D<half4> srcIlY : register(t2);         // half-res
Texture2D<half2> srcIlCoCg : register(t3);      // half-res
Texture2D<half4> srcGiSpecular : register(t4);  // half-res

RWTexture2D<half> outAo : register(u0);
RWTexture2D<half4> outIlY : register(u1);
RWTexture2D<half2> outIlCoCg : register(u2);
RWTexture2D<half4> outGiSpecular : register(u3);

#define BLEND_WEIGHT(a, b, c, d, w) (a * w.x + b * w.y + c * w.z + d * w.w)

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside frame bounds
	if (any(dtid >= uint2(FrameDim)))
		return;
#ifdef HALF_RES
	const float2 lowPos = (dtid + .5) * 0.5 - .5;
#else  // QUARTER_RES
	const float2 lowPos = (dtid + .5) * 0.25 - .5;
#endif
	const int2 lowBase = int2(floor(lowPos));
	const float2 f = lowPos - lowBase;
	const int2 lowMax = max(int2(OUT_FRAME_DIM) - 1, 0);

	int2 px00 = clamp(lowBase, 0, lowMax);
	int2 px01 = clamp(lowBase + int2(0, 1), 0, lowMax);
	int2 px10 = clamp(lowBase + int2(1, 0), 0, lowMax);
	int2 px11 = clamp(lowBase + int2(1, 1), 0, lowMax);

	float4 d = float4(
		srcDepth.Load(int3(px00, RES_MIP)),
		srcDepth.Load(int3(px01, RES_MIP)),
		srcDepth.Load(int3(px10, RES_MIP)),
		srcDepth.Load(int3(px11, RES_MIP)));

	float fullDepth = srcDepth[dtid];
	float4 relDiff = abs(d - fullDepth) / max(fullDepth, 1e-3);
	float4 bilinear = float4((1 - f.x) * (1 - f.y), (1 - f.x) * f.y, f.x * (1 - f.y), f.x * f.y);
	float4 w = bilinear / (relDiff + 0.01);
	w *= rcp(max(w.x + w.y + w.z + w.w, 1e-20));

	float ao = BLEND_WEIGHT(srcAo[px00], srcAo[px01], srcAo[px10], srcAo[px11], w);
	float4 y = BLEND_WEIGHT(srcIlY[px00], srcIlY[px01], srcIlY[px10], srcIlY[px11], w);
	float2 coCg = BLEND_WEIGHT(srcIlCoCg[px00], srcIlCoCg[px01], srcIlCoCg[px10], srcIlCoCg[px11], w);
#ifdef GI_SPECULAR
	float4 giSpecular = BLEND_WEIGHT(srcGiSpecular[px00], srcGiSpecular[px01], srcGiSpecular[px10], srcGiSpecular[px11], w);
#endif

	outAo[dtid] = ao;
	outIlY[dtid] = y;
	outIlCoCg[dtid] = coCg;
#ifdef GI_SPECULAR
	outGiSpecular[dtid] = giSpecular;
#endif
}