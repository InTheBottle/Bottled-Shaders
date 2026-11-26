#ifndef POSTPROCESSING_SURFACE_METERING_HLSLI
#define POSTPROCESSING_SURFACE_METERING_HLSLI

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"

namespace SurfaceMetering
{
	static const float NeutralAlbedo = 0.18;

	float BrightSurfaceStops(Texture2D<unorm float4> texAlbedo, Texture2D<float> texDepth, float2 uv, float2 gbufferScale, out bool isSky)
	{
		uint2 size;
		texAlbedo.GetDimensions(size.x, size.y);
		uint2 coord = min(uint2(saturate(uv) * gbufferScale * size), size - 1);
		isSky = FrameBuffer::ToStandardDepth(texDepth[coord]) >= 1.0;
		if (isSky)
			return 0.0;
		float albedo = Color::RGBToLuminance(ColorManagement::SceneToLinear(texAlbedo[coord].rgb));
		return log2(max(albedo, NeutralAlbedo) / NeutralAlbedo);
	}
}

#endif
