#include "Common/GBuffer.hlsli"

Texture2D<float4> NormalRoughnessTexture : register(t0);
Texture2D<float4> MasksTexture : register(t1);

cbuffer GBufferData : register(b0)
{
	float2 ResolutionScale;
};

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	int3 pixel = int3(input.pos.xy * ResolutionScale, 0);
	float3 normal = GBuffer::DecodeNormal(NormalRoughnessTexture.Load(pixel).xy);
	float2 masks = MasksTexture.Load(pixel).xy;
	float skin = masks.y < 1.5 ? saturate(masks.x) : 0.0;
	return float4(normal * 0.5 + 0.5, 1.0 - skin);
}
