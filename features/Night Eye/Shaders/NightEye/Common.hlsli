// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#ifndef NIGHT_EYE_COMMON
#define NIGHT_EYE_COMMON
namespace Math { static const float PI = 3.14159265358979323846; }
namespace Color {
float RGBToLuminance(float3 color) { return dot(color, float3(0.2125, 0.7154, 0.0721)); }
}
namespace Random {
float InterleavedGradientNoise(float2 position, uint frame) {
    float frameStep = float(frame % 16) * 0.0625;
    position += frameStep * float2(4.7526, 3.1914);
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(position, magic.xy)));
}
}
#endif
