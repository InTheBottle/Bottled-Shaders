#include "CameraFX/Common.hlsli"

float2 Wrap(float2 cell, float2 period)
{
	return cell - period * floor(cell / period);
}

float PeriodicNoise(float2 p, float2 period)
{
	float2 cell = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	float2 c0 = Wrap(cell, period);
	float2 c1 = Wrap(cell + 1.0, period);
	float a = CameraFX::Hash12(c0);
	float b = CameraFX::Hash12(float2(c1.x, c0.y));
	float c = CameraFX::Hash12(float2(c0.x, c1.y));
	float d = CameraFX::Hash12(c1);
	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float PeriodicFbm(float2 p, float2 period, uint octaves)
{
	float sum = 0.0;
	float amplitude = 0.5;
	float norm = 0.0;
	[loop] for (uint i = 0; i < octaves; i++)
	{
		sum += amplitude * PeriodicNoise(p + i * 7.31, period);
		norm += amplitude;
		p *= 2.0;
		period *= 2.0;
		amplitude *= 0.5;
	}
	return sum / norm;
}

#if defined(BAKE_DIRT)

RWTexture2D<unorm float> DirtOutput : register(u0);

float Discs(float2 uv, float period, float density, float2 radii, float softness, float ring, float salt)
{
	float2 p = uv * period;
	float2 cell = floor(p);
	float2 f = frac(p);
	float value = 0.0;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			float2 id = Wrap(cell + float2(x, y), period.xx) + salt;
			if (CameraFX::Hash12(id) > density)
				continue;
			float2 centre = float2(x, y) + 0.5 + (CameraFX::Hash22(id * 1.37) - 0.5) * 0.3;
			float radius = lerp(radii.x, radii.y, CameraFX::Hash12(id + 19.7));
			float d = length(f - centre) / radius;
			float disc = 1.0 - smoothstep(1.0 - softness, 1.0, d);
			float edge = exp(-pow((d - 0.86) / 0.1, 2.0)) * ring;
			float strength = lerp(0.3, 1.0, CameraFX::Hash12(id + 41.3));
			value = max(value, (disc * (1.0 - ring * 0.45) + edge) * strength);
		}
	}
	return value;
}

float Fibres(float2 uv, float period, float salt)
{
	float2 p = uv * period;
	float2 cell = floor(p);
	float2 f = frac(p);
	float value = 0.0;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			float2 id = Wrap(cell + float2(x, y), period.xx) + salt;
			if (CameraFX::Hash12(id) > 0.16)
				continue;
			float2 centre = float2(x, y) + CameraFX::Hash22(id + 3.3);
			float bend = 0.35 + 0.6 * CameraFX::Hash12(id + 7.1);
			float start = CameraFX::Hash12(id + 11.9) * 6.2832;
			float span = 0.8 + 1.6 * CameraFX::Hash12(id + 13.7);
			float2 d = f - centre;
			float angle = atan2(d.y, d.x) - start;
			angle -= 6.2832 * floor(angle / 6.2832);
			float along = step(angle, span);
			float distance = abs(length(d) - bend);
			value = max(value, exp(-distance * distance / 0.00012) * along * (0.4 + 0.6 * CameraFX::Hash12(id + 17.3)));
		}
	}
	return value;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	DirtOutput.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	float2 uv = (id.xy + 0.5) / float2(width, height);

	float specks = Discs(uv, 60.0, 0.15, float2(0.1, 0.3), 0.45, 0.0, 3.0);
	float bokeh = Discs(uv, 22.0, 0.2, float2(0.22, 0.44), 0.25, 0.55, 17.0);
	float blobs = Discs(uv, 8.0, 0.32, float2(0.3, 0.47), 0.7, 0.25, 29.0);

	float2 warp = float2(PeriodicFbm(uv * 3.0, 3.0, 3), PeriodicFbm(uv * 3.0 + 5.2, 3.0, 3)) - 0.5;
	float smudge = smoothstep(0.52, 0.8, PeriodicFbm(uv * 4.0 + warp * 1.5, 4.0, 4)) * (0.6 + 0.4 * PeriodicFbm(uv * 32.0, 32.0, 2));

	float2 streakCoords = float2(uv.x * 6.0 + uv.y * 2.0, uv.y * 120.0 - uv.x * 40.0);
	float streaks = smoothstep(0.62, 0.9, PeriodicFbm(streakCoords, float2(2.0, 40.0), 3)) * smoothstep(0.55, 0.8, PeriodicFbm(uv * 2.0 + 9.1, 2.0, 2));

	float fibres = Fibres(uv, 5.0, 53.0);

	DirtOutput[id.xy] = saturate(specks * 0.7 + bokeh * 0.42 + blobs * 0.22 + smudge * 0.3 + streaks * 0.1 + fibres * 0.3);
}

#elif defined(BAKE_FROST)

RWTexture2D<unorm float4> FrostOutput : register(u0);

static const float FrostAspect = 16.0 / 9.0;

float BakedArrival(float2 uv)
{
	float2 p = float2(uv.x * FrostAspect, uv.y);
	float shaped = CameraFX::FrameArrival(uv, 4.5);
	float broad = CameraFX::Fbm(p * 2.6 + 3.7, 5);
	float2 centred = (uv - 0.5) * float2(FrostAspect, 1.0);
	float angle = atan2(centred.y, centred.x);
	float warp = CameraFX::Fbm(float2(angle * 3.0, shaped * 4.0) + 11.0, 3);
	float fingers = pow(CameraFX::Ridge(float2(angle * 15.0 + warp * 4.0, shaped * 2.5)), 4.0);
	return saturate(shaped + (broad - 0.5) * 0.24 - fingers * 0.05 * (1.0 - shaped));
}

float Fern(float2 d, float2 direction, float fernLength, float seed)
{
	float2 normal = float2(-direction.y, direction.x);
	float u = dot(d, direction);
	float v = dot(d, normal);
	float value = 0.0;
	if (u >= -0.01 && u <= fernLength + 0.01 && abs(v) <= fernLength * 0.5) {
		float t = saturate(u / fernLength);
		float bow = (CameraFX::ValueNoise(float2(u * 22.0, seed * 7.0)) - 0.5) * 0.012 + sin(u * 8.0 + seed * 5.0) * 0.005;
		v -= bow * t;
		float stemWidth = 0.0026 * (1.0 - 0.7 * t) + 0.0006;
		float tipFade = 1.0 - smoothstep(0.82, 1.0, t);
		value = exp(-v * v / (stemWidth * stemWidth)) * tipFade;

		float spacing = 0.0075 + 0.0045 * CameraFX::Hash12(float2(seed, 2.9));
		float angle = 0.85 + 0.25 * CameraFX::Hash12(float2(seed, 5.3));
		float ca = cos(angle);
		float sa = sin(angle);
		float last = floor(u / spacing);
		[loop] for (int j = 0; j < 7; j++)
		{
			float k = last - j;
			if (k < 1.0)
				break;
			float origin = k * spacing;
			float progress = origin / fernLength;
			[unroll] for (int side = 0; side < 2; side++)
			{
				float mirror = side == 0 ? 1.0 : -1.0;
				float reach = (fernLength - origin) * (0.28 + 0.2 * CameraFX::Hash12(float2(k + mirror * 0.5, seed))) * (1.0 - 0.35 * progress);
				float2 rel = float2(u - origin, v * mirror);
				float along = rel.x * ca + rel.y * sa;
				float across = rel.y * ca - rel.x * sa;
				if (along < 0.0 || along > reach)
					continue;
				float s = along / max(reach, 1e-4);
				float width = stemWidth * 0.6 * (1.0 - s) + 0.0004;
				float branch = exp(-across * across / (width * width)) * 0.85;
				float barbSpacing = 0.0022;
				float barbOrigin = floor(along / barbSpacing) * barbSpacing;
				float2 barbRel = float2(along - barbOrigin, abs(across));
				float barbAlong = barbRel.x * 0.6 + barbRel.y * 0.8;
				float barbAcross = barbRel.y * 0.6 - barbRel.x * 0.8;
				float barbReach = width * 3.5 * (1.0 - s);
				float barb = barbAlong > 0.0 && barbAlong < barbReach ? exp(-barbAcross * barbAcross / (width * width * 0.25)) * 0.5 : 0.0;
				value = max(value, max(branch, barb) * tipFade);
			}
		}
	}
	return value;
}

float FernLayer(float2 p, float cellSize, float2 lengths, float reach, float salt)
{
	float2 cell = floor(p / cellSize);
	float relief = 0.0;
	[loop] for (int y = -2; y <= 2; y++)
	{
		[loop] for (int x = -2; x <= 2; x++)
		{
			float2 id = cell + float2(x, y) + salt;
			float2 origin = (cell + float2(x, y) + 0.2 + 0.6 * CameraFX::Hash22(id + 9.1)) * cellSize;
			float2 originUV = float2(origin.x / FrostAspect, origin.y);
			float near = 1.0 - smoothstep(reach * 0.3, reach, CameraFX::FrameArrival(saturate(originUV), 4.5));
			if (CameraFX::Hash12(id * 1.31 + 5.7) > 0.95 * near)
				continue;
			float2 oc = originUV - 0.5;
			float2 inward = abs(oc.x) * FrostAspect > abs(oc.y) ? float2(-sign(oc.x), 0.0) : float2(0.0, -sign(oc.y));
			float2 toCentre = normalize(float2(0.5 * FrostAspect, 0.5) - origin);
			float2 direction = normalize(lerp(inward, toCentre, 0.4));
			direction = CameraFX::Rotate(direction, (CameraFX::Hash12(id + 3.3) - 0.5) * 1.6);
			float fernLength = lerp(lengths.x, lengths.y, CameraFX::Hash12(id + 7.7));
			relief = max(relief, Fern(p - origin, direction, fernLength, CameraFX::Hash12(id + 1.9) * 37.0) * (0.5 + 0.5 * CameraFX::Hash12(id + 13.1)));
		}
	}
	return relief;
}

float FernRelief(float2 uv)
{
	float2 p = float2(uv.x * FrostAspect, uv.y);
	float relief = FernLayer(p, 0.1, float2(0.08, 0.2), 0.36, 0.0);
	relief = max(relief, FernLayer(p, 0.045, float2(0.03, 0.08), 0.42, 71.0) * 0.8);
	float fuzz = pow(CameraFX::Ridge(p * 140.0), 6.0) * smoothstep(0.05, 0.4, relief);
	return saturate(relief + fuzz * 0.25);
}

float Bubbles(float2 uv, float arrival)
{
	float2 p = float2(uv.x * FrostAspect, uv.y);
	float zone = smoothstep(0.04, 0.12, arrival) * (1.0 - smoothstep(0.3, 0.45, arrival));
	float value = 0.5;
	[unroll] for (int layer = 0; layer < 2; layer++)
	{
		float scale = layer == 0 ? 34.0 : 78.0;
		float2 g = p * scale + layer * 13.7;
		float2 cell = floor(g);
		float2 f = frac(g);
		if (CameraFX::Hash12(cell + 31.0 * layer) > (layer == 0 ? 0.05 : 0.04) * zone)
			continue;
		float2 centre = 0.5 + (CameraFX::Hash22(cell + 7.0 * layer) - 0.5) * 0.3;
		float radius = 0.16 + 0.18 * CameraFX::Hash12(cell + 3.0);
		float2 b = (f - centre) / radius;
		float d = length(b);
		if (d >= 1.0)
			continue;
		float dark = smoothstep(0.62, 0.95, d) * (1.0 - smoothstep(0.95, 1.0, d));
		float bright = exp(-pow((d - 0.55) / 0.13, 2.0)) * 0.6 + exp(-dot(b + 0.38, b + 0.38) * 60.0);
		value = saturate(0.5 - dark * 0.42 + bright * 0.38);
	}
	return value;
}

float Snow(float2 uv)
{
	float2 p = float2(uv.x * FrostAspect, uv.y);
	float frame = 1.0 - smoothstep(0.0, 0.3, BakedArrival(uv));
	float grains = 0.0;
	[unroll] for (int layer = 0; layer < 2; layer++)
	{
		float scale = layer == 0 ? 230.0 : 90.0;
		float2 g = p * scale + layer * 5.3;
		float2 cell = floor(g);
		float density = (layer == 0 ? 0.2 : 0.05) * frame * frame;
		if (CameraFX::Hash12(cell + 17.0 * layer) > density)
			continue;
		float2 centre = 0.3 + 0.4 * CameraFX::Hash22(cell + 2.0 + layer);
		float radius = (layer == 0 ? 0.22 : 0.3) * (0.6 + 0.6 * CameraFX::Hash12(cell + 9.0));
		float d = length(frac(g) - centre) / radius;
		grains = max(grains, (1.0 - smoothstep(0.5, 1.0, d)) * (0.55 + 0.45 * CameraFX::Hash12(cell + 4.0)));
	}
	float top = 0.93 + (CameraFX::Fbm(float2(p.x * 7.0, 1.3), 4) - 0.5) * 0.07;
	float crust = smoothstep(top, top + 0.03, uv.y) * (0.55 + 0.45 * CameraFX::Fbm(p * 70.0, 4));
	return saturate(max(grains, crust));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	FrostOutput.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	float2 uv = (id.xy + 0.5) / float2(width, height);
	float arrival = BakedArrival(uv);
	FrostOutput[id.xy] = float4(FernRelief(uv), arrival, Bubbles(uv, arrival), Snow(uv));
}

#endif
