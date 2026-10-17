#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"
#include "IBL/IBL.hlsli"

RWTexture2D<float4> FogIBLColor : register(u0);

[numthreads(1, 1, 1)] void main() {
	float3 fogColor = Color::Fog(SharedData::exponentialHeightFogSettings.vanillaFogNearColor.rgb);
	FogIBLColor[uint2(0, 0)] = float4(ImageBasedLighting::GetFogIBLColor(fogColor), 1.0f);
}
