// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#ifndef NIGHT_EYE_CUSTOM_VFX
#define NIGHT_EYE_CUSTOM_VFX

// Return normalized RGB; the host applies Custom VFX Amount and the Night Eye fade.
float3 ApplyNightEyeCustomVFX(float3 color, float2 uv, float2 pixelPosition, float elapsedSeconds)
{
	const float3 phosphorColor = float3(0.35, 1.0, 0.45);
	const float scanlineDepth = 0.08;
	const float scanlineSpacingPixels = 2.0;
	const float scanlineSpeedPixels = 12.0;
	float scanline = 0.5 + 0.5 * sin((pixelPosition.y + elapsedSeconds * scanlineSpeedPixels) * (2.0 * Math::PI) / scanlineSpacingPixels);
	return Color::RGBToLuminance(color) * phosphorColor * (1.0 - scanlineDepth * scanline);
}

#endif
