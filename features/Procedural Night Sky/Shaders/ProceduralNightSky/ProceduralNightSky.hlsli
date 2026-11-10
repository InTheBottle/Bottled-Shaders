// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Noise and star code adapted from WGSL-Generator, Copyright (c) 2026 NeaByteLab (MIT)

#ifndef PROCEDURAL_NIGHT_SKY_HLSLI
#define PROCEDURAL_NIGHT_SKY_HLSLI

namespace ProceduralNightSky
{
	static const float MaxStarRadius = 0.2;
	static const uint AuroraSteps = 8;
	static const float AuroraHeight = 1.1;
	static const float AuroraHorizonBend = 0.26;
	static const float AuroraArcSpacing = 1.7;
	static const float AuroraSheetThickness = 0.075;
	static const float AuroraHazeWidth = 0.5;
	static const float AuroraPeriod = 256.0;

	float Hash12(float2 p)
	{
		float2 q = frac(p * float2(123.34, 456.21));
		q += dot(q, q + 45.32);
		return frac(q.x * q.y);
	}

	float2 Hash22(float2 p)
	{
		float3 q = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
		q += dot(q, q.yzx + 33.33);
		return frac((q.xx + q.yz) * q.zy);
	}

	float Noise(float2 p)
	{
		float2 cell = floor(p);
		float2 blend = p - cell;
		blend = blend * blend * (3.0 - 2.0 * blend);

		float x0 = cell.x - AuroraPeriod * floor(cell.x / AuroraPeriod);
		float x1 = x0 + 1.0 >= AuroraPeriod ? 0.0 : x0 + 1.0;
		float bottom = lerp(Hash12(float2(x0, cell.y)), Hash12(float2(x1, cell.y)), blend.x);
		float top = lerp(Hash12(float2(x0, cell.y + 1.0)), Hash12(float2(x1, cell.y + 1.0)), blend.x);
		return lerp(bottom, top, blend.y);
	}

	float RibbonNoise(float x, float row)
	{
		float cell = floor(x);
		float blend = x - cell;
		blend = blend * blend * (3.0 - 2.0 * blend);

		cell -= AuroraPeriod * floor(cell / AuroraPeriod);
		float next = cell + 1.0 >= AuroraPeriod ? 0.0 : cell + 1.0;
		return lerp(Hash12(float2(cell, row)), Hash12(float2(next, row)), blend);
	}

	float2 ToOctahedron(float3 direction)
	{
		direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
		float2 folded = (1.0 - abs(direction.yx)) * float2(direction.x >= 0.0 ? 1.0 : -1.0, direction.y >= 0.0 ? 1.0 : -1.0);
		return direction.z >= 0.0 ? direction.xy : folded;
	}

	float3 StarLayer(float2 uv, float cells, float density, float baseRadius, float pixelAngle, float twinkle, float timer)
	{
		float2 grid = uv * cells;
		float2 cell = floor(grid);
		float pick = Hash12(cell);
		float3 star = 0.0;
		if (pick <= density) {
			float2 seed = Hash22(cell);
			float radius = min(max(baseRadius, pixelAngle * cells * 0.9), MaxStarRadius);
			float2 offset = frac(grid) - 0.5 - (seed - 0.5) * (0.9 - 2.0 * MaxStarRadius);
			float falloff = saturate(1.0 - length(offset) / radius);
			if (falloff > 0.0) {
				float rank = 1.0 - pick / density;
				float magnitude = rank * rank * rank * rank;
				float flicker = 1.0 + twinkle * sin(timer * (1.5 + 4.0 * seed.y) + seed.x * 6.2831853);
				float area = baseRadius * baseRadius / (radius * radius);

				float colorPick = Hash12(cell + float2(123.4, 567.8));
				float3 color = colorPick > 0.8 ? float3(0.6, 0.8, 1.0) : (colorPick < 0.2 ? float3(1.0, 0.9, 0.7) : float3(1.0, 1.0, 1.0));
				star = color * (pow(falloff, 6.0) * 2.5 + falloff * falloff * 0.5) * magnitude * flicker * area;
			}
		}
		return star;
	}

	float3 Stars(float3 direction, float pixelAngle, float twinkle, float timer)
	{
		float2 uv = ToOctahedron(direction);
		float3 stars = StarLayer(uv, 200.0, 0.5, 0.09, pixelAngle, twinkle, timer) * 0.6;
		stars += StarLayer(uv + float2(5.4, 2.7), 110.0, 0.4, 0.06, pixelAngle, twinkle, timer);
		stars += StarLayer(uv + float2(1.9, 8.4), 55.0, 0.2, 0.035, pixelAngle, twinkle, timer) * 2.0;
		return stars;
	}

	float3 ShootingStar(float3 view, float pixelAngle, float3 head, float3 tail, float width)
	{
		float3 axis = head - tail;
		float along = saturate(dot(view - tail, axis) / max(dot(axis, axis), 1e-8));
		float3 offset = view - lerp(tail, head, along);
		float taperedWidth = width * (0.2 + 0.8 * along);
		float radius = max(taperedWidth, pixelAngle);
		float core = exp(-dot(offset, offset) / (radius * radius)) * taperedWidth / radius;
		return lerp(float3(1.0, 0.62, 0.35), float3(0.85, 0.95, 1.0), along) * core * along * along;
	}

	float AuroraFold(float2 p, float timer)
	{
		float broad = Noise(p * 0.31 + float2(timer * 0.021, -timer * 0.009)) - 0.5;
		float curl = Noise(p * 0.83 + float2(-timer * 0.033, timer * 0.017 + 9.0)) - 0.5;
		float ripple = Noise(p * 2.1 + float2(timer * 0.05, 21.0)) - 0.5;
		return broad * 3.4 + curl * 1.1 + ripple * 0.3;
	}

	float3 Aurora(float3 view, float timer, float dither, float3 lowColor, float3 highColor)
	{
		float2 plane = view.xy / (view.z + AuroraHorizonBend);
		float foldLow = AuroraFold(plane, timer);
		float foldHigh = AuroraFold(plane * (1.0 + AuroraHeight), timer);
		float stepLength = abs(plane.y) * AuroraHeight / float(AuroraSteps);
		float spread = sqrt(AuroraSheetThickness * AuroraSheetThickness + stepLength * stepLength);

		float3 glow = 0.0;
		for (uint i = 0; i < AuroraSteps; i++) {
			float altitude = (float(i) + dither) / float(AuroraSteps);
			float2 p = plane * (1.0 + altitude * AuroraHeight);
			float fold = lerp(foldLow, foldHigh, altitude);

			float phase = (p.y + fold + timer * 0.012) / AuroraArcSpacing;
			float arc = floor(phase + 0.5);
			float offset = (phase - arc) * AuroraArcSpacing;
			float sheet = exp(-offset * offset / (spread * spread)) * 2.2 * AuroraSheetThickness / spread;
			float haze = exp(-offset * offset / (AuroraHazeWidth * AuroraHazeWidth)) * 0.2;

			float along = p.x + fold * 0.6 + arc * 17.3;
			float presence = 0.42 + 0.58 * smoothstep(0.25, 0.6, Noise(float2(along * 0.17 + timer * 0.013, arc * 3.7 + 40.0)));
			float base = 0.03 + 0.16 * RibbonNoise(along * 0.9 + timer * 0.04, arc + 60.0);
			float rays = RibbonNoise(along * 6.5 + timer * 0.21, arc + 70.0);
			float fine = RibbonNoise(along * 21.0 - timer * 0.47, arc + 80.0);
			float streak = 0.12 + 1.5 * rays * rays * rays + 1.1 * fine * fine * fine * (0.3 + rays);

			float lift = max(altitude - base, 0.0);
			float profile = smoothstep(0.0, 0.035, altitude - base) * exp(-lift * 3.6);
			glow += lerp(lowColor, highColor * 0.75, saturate(lift * 1.9)) * (sheet + haze) * presence * streak * profile;
		}

		float slant = sqrt(1.0 + dot(plane, plane));
		return glow * slant * smoothstep(0.0, 0.2, view.z) / float(AuroraSteps);
	}

	float4 ToAdditiveBlend(float3 emitted, float radianceLimit)
	{
		emitted = max(emitted, 0.0);
		float peak = max(emitted.x, max(emitted.y, emitted.z));
		float4 result = 0.0;
		if (peak > 0.0) {
			float alpha = clamp(peak / max(radianceLimit, 1.0), 1.0 / 256.0, 1.0);
			result = float4(emitted / alpha, alpha);
		}
		return result;
	}

	float3 EvaluateAurora(float3 view, float dither)
	{
		const SharedData::ProceduralNightSkySettings settings = SharedData::proceduralNightSkySettings;

		float3 lowColor = ColorManagement::SRGBToWorking(settings.auroraLowColor);
		float3 highColor = ColorManagement::SRGBToWorking(settings.auroraHighColor);
		return Aurora(view, SharedData::Timer * settings.auroraSpeed, dither, lowColor, highColor) * settings.auroraIntensity;
	}

	float4 Shade(float3 starDirection, float3 view, float pixelAngle, float3 starTint, float horizon)
	{
		const SharedData::ProceduralNightSkySettings settings = SharedData::proceduralNightSkySettings;

		float3 starLight = 0.0;
		[branch] if (settings.starIntensity > 0.0)
			starLight = Stars(starDirection, pixelAngle, settings.starTwinkle, SharedData::Timer) * settings.starIntensity;
		[branch] if (settings.shootingStarIntensity > 0.0)
			starLight += ShootingStar(view, pixelAngle, settings.shootingStarHead, settings.shootingStarTail, settings.shootingStarWidth) * settings.shootingStarIntensity;

		float3 emitted = ColorManagement::SRGBToWorking(starLight) * starTint;
		return ToAdditiveBlend(emitted * horizon, settings.radianceLimit);
	}
}

#endif
