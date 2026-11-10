// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#if defined(NIGHT_EYE_ACTOR_EFFECT)
	float nightEyeActorStrength = NightEye::GetActorStrength();
	[branch] if (nightEyeActorStrength > 0.0)
	{
		float3 nightEyeActorColor = NightEye::GetActorColor(ColorManagement::SceneToLinear(lightColor), baseColor.xyz);
		nightEyeActorColor *= saturate(Color::RGBToLuminance(baseColor.xyz));
		lightColor = lerp(lightColor, ColorManagement::LinearToScene(nightEyeActorColor), nightEyeActorStrength);
	}
#endif
