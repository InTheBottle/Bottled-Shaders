// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#if !defined(MULTBLEND) && !defined(MULTBLEND_DECAL)
	[branch] if (NightEye::HasEyeshine())
	{
		float nightEyeShape = saturate(Color::RGBToLuminance(baseTexColor.xyz)) * saturate(alpha);
#	if !defined(MOTIONVECTORS_NORMALS)
#		if defined(EXP_HEIGHT_FOG)
		nightEyeShape *= (1.0 - saturate(vanillaFogFactor)) * (1.0 - saturate(expFogFactor));
#		else
		nightEyeShape *= 1.0 - saturate(fogFactor);
#		endif
#	endif
		blendedColor += NightEye::GetEyeshine() * nightEyeShape;
	}
#endif
