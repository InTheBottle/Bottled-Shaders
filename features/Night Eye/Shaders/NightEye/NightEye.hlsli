// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#ifndef NIGHT_EYE_HLSLI
#define NIGHT_EYE_HLSLI

namespace NightEye
{
	Texture2D<float> EyeshineMask : register(t127);

	static const float kInfraredMode = 1.0;
	static const float kPredatorMode = 2.0;
	static const float kMinimumBrightness = 0.35;
	static const float3 kInfraredCool = float3(0.8, 0.01, 0.0);
	static const float3 kInfraredWarm = float3(1.0, 0.35, 0.01);
	static const float3 kInfraredHot = float3(1.0, 0.95, 0.65);

	bool HasFlag(uint a_flag)
	{
		return (Permutation::ExtraShaderDescriptor & a_flag) != 0;
	}

	bool IsWorld()
	{
		return HasFlag(Permutation::ExtraFlags::InWorld) && !HasFlag(Permutation::ExtraFlags::InReflection);
	}

	bool HasEyeshine()
	{
		return SharedData::nightEyeSettings.Eye.w > 0.0 && HasFlag(Permutation::ExtraFlags::NightEyeEyeshine) && IsWorld();
	}

	float3 GetEyeshine()
	{
		return SharedData::nightEyeSettings.Eye.xyz * SharedData::nightEyeSettings.Eye.w;
	}

	float GetActorStrength()
	{
		const float mode = SharedData::nightEyeSettings.Actor.x;
		bool active = HasFlag(Permutation::ExtraFlags::NightEyeActor) && IsWorld() && (mode == kInfraredMode || mode == kPredatorMode);
		return active ? saturate(SharedData::nightEyeSettings.Actor.y) : 0.0;
	}

	float3 GetActorColor(float3 linearLighting, float3 surfaceColor)
	{
		float detail = 0.5 * (saturate(Color::RGBToLuminance(max(linearLighting, 0.0))) +
			Color::RGBToLuminance(saturate(surfaceColor)));
		float3 palette;
		[branch] if (SharedData::nightEyeSettings.Actor.x == kInfraredMode)
		{
			palette = lerp(kInfraredCool, kInfraredWarm, saturate(detail * 2.0));
			palette = lerp(palette, kInfraredHot, saturate(detail * 2.0 - 1.0));
		}
		else
		{
			palette = SharedData::nightEyeSettings.Neutral.rgb;
			if (HasFlag(Permutation::ExtraFlags::NightEyeEnemy))
				palette = SharedData::nightEyeSettings.Enemy.rgb;
			else if (HasFlag(Permutation::ExtraFlags::NightEyeAlly))
				palette = SharedData::nightEyeSettings.Ally.rgb;
		}
		return saturate(palette) * max(SharedData::nightEyeSettings.Actor.z, 0.0) * lerp(kMinimumBrightness, 1.0, detail);
	}
}

#if !defined(MULTBLEND) && !defined(MULTBLEND_DECAL) && !defined(PROJECTED_UV) && !defined(BLOOD) && !defined(IS_VOLUMETRIC_FOG)
#	define NIGHT_EYE_ACTOR_EFFECT
#endif

#endif
