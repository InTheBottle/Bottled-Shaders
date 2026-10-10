#ifndef __MOTION_BLUR_DEPENDENCY_HLSL__
#define __MOTION_BLUR_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"

namespace MotionBlur
{
	float2 GetSSMotionVector(float4 a_wsPosition, float4 a_previousWSPosition)
	{
		float4 screenPosition = mul(FrameBuffer::CameraViewProjUnjittered, a_wsPosition);
		float4 previousScreenPosition = mul(FrameBuffer::CameraPreviousViewProjUnjittered, a_previousWSPosition);
		screenPosition.xy = screenPosition.xy / screenPosition.ww;
		previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
		return float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
	}

	float2 GetSSCameraMotionVector(float4 a_wsPosition)
	{
		float4 cameraMovement = float4(FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz, 0.0);
		return GetSSMotionVector(a_wsPosition, a_wsPosition + cameraMovement);
	}
}

#endif  // __MOTION_BLUR_DEPENDENCY_HLSL__
