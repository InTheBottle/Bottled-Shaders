#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "ProceduralLightning/Common.hlsli"

StructuredBuffer<ProceduralLightning::Segment> Segments : register(t0);

static const float NearW = 32.0;
static const float SupportSigmas = 3.0;

float2 ToPixels(float4 positionCS)
{
	return (positionCS.xy / positionCS.w * float2(0.5, -0.5) + 0.5) * ProceduralLightning::RenderSize;
}

float4 FromPixels(float2 pixel)
{
	return float4(pixel / ProceduralLightning::RenderSize * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}

ProceduralLightning::VertexOutput main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	ProceduralLightning::VertexOutput output = (ProceduralLightning::VertexOutput)0;
	output.Position = float4(2.0, 2.0, 2.0, 1.0);

	float2 corner = float2(vertexID & 1u, vertexID >> 1u) * 2.0 - 1.0;
	float3 right = FrameBuffer::ViewToWorld(float3(1.0, 0.0, 0.0), false);
	float pixelScale = abs(dot(FrameBuffer::CameraViewProj[0].xyz, right)) * 0.5 * ProceduralLightning::RenderSize.x;

	if (instanceID >= ProceduralLightning::SegmentCount) {
		ProceduralLightning::Strike strike = ProceduralLightning::Strikes[(instanceID - ProceduralLightning::SegmentCount) % ProceduralLightning::MaxStrikes];
		if (strike.CloudGlow <= 0.001)
			return output;

		float3 centerWS = strike.Glow.xyz - FrameBuffer::CameraPosAdjust.xyz;
		float4 centerCS = mul(FrameBuffer::CameraViewProj, float4(centerWS, 1.0));
		if (centerCS.w < NearW)
			return output;

		float radius = min(strike.Glow.w * pixelScale / centerCS.w, ProceduralLightning::RenderSize.y);
		float fog = ProceduralLightning::GetFogFactor(length(centerWS)) * ProceduralLightning::FogAbsorption;
		output.Position = FromPixels(ToPixels(centerCS) + corner * radius);
		output.Corner = corner;
		output.Ends = float4(1.0 / centerCS.w, 0.0, 0.0, 0.0);
		output.Shape = float4(strike.Glow.x * 0.0007, strike.Glow.y * 0.0007, 0.0, 0.0);
		output.Tint = float4(ColorManagement::SRGBToWorking(strike.Color.rgb), strike.CloudGlow * ProceduralLightning::Intensity * (1.0 - fog));
		output.Params = float2(0.0, ProceduralLightning::KindCloudGlow);
		return output;
	}

	ProceduralLightning::Segment segment = Segments[instanceID];
	uint branchDepth = segment.Packed >> 8u;
	ProceduralLightning::Strike strike = ProceduralLightning::Strikes[(segment.Packed & 0xFFu) % ProceduralLightning::MaxStrikes];

	float brightness = branchDepth == 0u ? strike.Main : strike.Branch * pow(0.65, float(branchDepth - 1u));
	if (brightness <= 0.001)
		return output;

	float3 startWS = segment.Start - FrameBuffer::CameraPosAdjust.xyz;
	float3 endWS = segment.End - FrameBuffer::CameraPosAdjust.xyz;
	float4 startCS = mul(FrameBuffer::CameraViewProj, float4(startWS, 1.0));
	float4 endCS = mul(FrameBuffer::CameraViewProj, float4(endWS, 1.0));
	if (startCS.w < NearW && endCS.w < NearW)
		return output;
	if (startCS.w < NearW)
		startCS = lerp(startCS, endCS, (NearW - startCS.w) / (endCS.w - startCS.w));
	else if (endCS.w < NearW)
		endCS = lerp(endCS, startCS, (NearW - endCS.w) / (startCS.w - endCS.w));

	float2 startPixel = ToPixels(startCS);
	float2 endPixel = ToPixels(endCS);
	float2 axis = endPixel - startPixel;
	float axisLength = length(axis);
	float2 direction = axisLength > 1e-3 ? axis / axisLength : float2(1.0, 0.0);
	float2 side = float2(-direction.y, direction.x);

	float sigma = segment.Width * pixelScale / (0.5 * (startCS.w + endCS.w));
	float coreSigma = clamp(sigma, ProceduralLightning::MinSigma, ProceduralLightning::MaxSigma);
	float glowSigma = coreSigma * ProceduralLightning::WidthToGlow;
	float support = (branchDepth > 1u ? coreSigma : glowSigma) * SupportSigmas;

	float fog = ProceduralLightning::GetFogFactor(length(0.5 * (startWS + endWS))) * ProceduralLightning::FogAbsorption;
	brightness *= sqrt(saturate(sigma / coreSigma)) * (1.0 - fog);

	float2 anchor = corner.x < 0.0 ? startPixel - direction * support : endPixel + direction * support;
	output.Position = FromPixels(anchor + side * corner.y * support);
	output.Line = float4(startPixel, endPixel);
	output.Ends = float4(1.0 / startCS.w, 1.0 / endCS.w, segment.StartEnergy, segment.EndEnergy);
	output.Shape = float4(segment.StartArrival, segment.EndArrival, coreSigma, branchDepth > 1u ? 0.0 : glowSigma);
	output.Tint = float4(ColorManagement::SRGBToWorking(strike.Color.rgb), brightness * ProceduralLightning::Intensity);
	output.Params = float2(strike.Front, ProceduralLightning::KindSegment);
	return output;
}
