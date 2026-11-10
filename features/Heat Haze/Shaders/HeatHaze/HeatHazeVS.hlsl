#include "Common/FrameBuffer.hlsli"
#include "HeatHaze/Common.hlsli"

HeatHaze::Interpolants main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	float4 plume = HeatHaze::Plumes[instanceID];
	float2 corner = float2(vertexID & 1u, vertexID >> 1u);

	float3 base = plume.xyz - FrameBuffer::CameraPosAdjust.xyz;
	float3 side = float3(base.y, -base.x, 0.0);
	side /= max(length(side), 1e-3);

	float3 positionWS = base + side * (corner.x * 2.0 - 1.0) * plume.w * HeatHaze::PlumeWidthScale;
	positionWS.z += corner.y * plume.w * HeatHaze::PlumeHeightScale;

	HeatHaze::Interpolants output;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
	output.PlumeCoord = float2(corner.x * 2.0 - 1.0, corner.y);
	output.Fade = HeatHaze::PlumeParams[instanceID].x;
	output.Seed = HeatHaze::PlumeParams[instanceID].y;
	output.ViewDepth = output.Position.w;
	return output;
}
