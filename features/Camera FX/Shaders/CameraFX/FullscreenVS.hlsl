#include "CameraFX/Common.hlsli"

CameraFX::Interpolants main(uint vertexID : SV_VertexID)
{
	CameraFX::Interpolants output;
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.TexCoord = uv;
	output.Position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}
