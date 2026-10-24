// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#if !defined(DEPTH_WRITE_DECALS)
	float nightEyeActorStrength = NightEye::GetActorStrength();
	[branch] if (nightEyeActorStrength > 0.0)
	{
		float3 nightEyeActorColor = NightEye::GetActorColor(ColorManagement::SceneToLinear(color.xyz) + specularColor, material.BaseColor);
		[branch] if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::AdditiveLighting) != 0)
			nightEyeActorColor *= saturate(Color::RGBToLuminance(material.BaseColor * max(vertexColor, 0.0)));
		color.xyz = lerp(color.xyz, ColorManagement::LinearToScene(nightEyeActorColor), nightEyeActorStrength);
		specularColor *= 1.0 - nightEyeActorStrength;
		indirectLobeWeights.diffuse *= 1.0 - nightEyeActorStrength;
		indirectLobeWeights.specular *= 1.0 - nightEyeActorStrength;
#	if defined(WETNESS_EFFECTS)
		wetnessReflectance *= 1.0 - nightEyeActorStrength;
#	endif
	}
#endif
	[branch] if (NightEye::HasEyeshine())
	{
#if defined(EYE)
		float3 nightEyeNormal = input.EyeNormal;
#else
		float3 nightEyeNormal = worldNormal.xyz;
#endif
		float nightEyeFacing = saturate(dot(normalize(nightEyeNormal), viewDirection));
		float nightEyeFacingSquared = nightEyeFacing * nightEyeFacing;
		float nightEyeFacingFourth = nightEyeFacingSquared * nightEyeFacingSquared;
		float nightEyeFacingEighth = nightEyeFacingFourth * nightEyeFacingFourth;
		float nightEyeMask = 1.0;
		[branch] if (NightEye::HasFlag(Permutation::ExtraFlags::NightEyeMask))
			nightEyeMask = NightEye::EyeshineMask.SampleLevel(SampColorSampler, uvOriginal, 0.0);
		specularColor += NightEye::GetEyeshine() * (nightEyeMask * nightEyeFacingEighth);
	}
