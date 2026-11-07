#include "CameraFX/Common.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float> LightVisibility : register(t2);
SamplerState LinearSampler : register(s0);

static const float3 FrostTint = float3(0.84, 0.92, 1.0);
static const float3 MoonTint = float3(0.75, 0.85, 1.0);
static const float3 EmberColor = float3(1.0, 0.45, 0.1);
static const float3 FireColor = float3(1.0, 0.45, 0.12);
static const float3 ShockColor = float3(0.6, 0.75, 1.0);

static const uint SplatBlood = 0;
static const uint SplatDarkBlood = 1;
static const uint SplatOil = 2;
static const uint SplatVenom = 3;
static const uint SplatIce = 4;
static const uint SplatFire = 5;
static const uint SplatGrit = 6;

float GetAspect()
{
	return CameraFX::Resolution.x * CameraFX::Resolution.w;
}

float GetTime()
{
	return CameraFX::ThemeParams.w;
}

float3 SampleScene(float2 uv, float mip)
{
	return CameraFX::ToWorking(SceneTexture.SampleLevel(LinearSampler, saturate(uv), min(mip, CameraFX::Blur.w)).rgb);
}

float4 Over(float4 top, float4 bottom)
{
	return float4(top.rgb + bottom.rgb * (1.0 - top.a), top.a + bottom.a * (1.0 - top.a));
}

float4 Layer(float3 color, float alpha)
{
	alpha = saturate(alpha);
	return float4(color * alpha, alpha);
}

float2 GetDirection(float2 v)
{
	return v / max(sqrt(dot(v, v)), 1e-5);
}

float GetFrost(float2 uv, float frost, out float crystals)
{
	crystals = 0.0;
	float2 q = abs(uv - 0.5) * 2.0;
	float shape = lerp(max(q.x, q.y), length(q * float2(0.82, 0.9)), 0.35);
	float threshold = 1.3 - frost * 1.15;
	float mask = 0.0;
	[branch] if (shape + 0.175 >= threshold - 0.06)
	{
		float2 p = uv * float2(GetAspect(), 1.0);
		float coarse = CameraFX::Fbm(p * 3.0, 3);
		mask = smoothstep(threshold - 0.06, threshold + 0.05, shape + (coarse - 0.5) * 0.35) * saturate(frost * 5.0);
		[branch] if (mask > 0.0)
		{
			float fine = CameraFX::Fbm(p * 22.0 + coarse * 3.0, 3);
			crystals = pow(saturate(1.0 - abs(fine * 2.0 - 1.0)), 6.0);
			float2 radial = uv - 0.5;
			float streak = CameraFX::ValueNoise(GetDirection(radial) * 18.0 + length(radial) * 4.0);
			crystals = max(crystals, pow(streak, 8.0) * 0.6) * mask;
		}
	}
	return mask;
}

float GetSpecks(float2 p, float density, float sizeScale)
{
	float2 cell = floor(p);
	float2 local = frac(p);
	float specks = 0.0;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			float2 neighbour = cell + float2(x, y);
			float present = step(1.0 - density, CameraFX::Hash12(neighbour + 3.1));
			float radius = (0.15 + 0.35 * CameraFX::Hash12(neighbour + 7.7)) * sizeScale;
			float distance = length(local - float2(x, y) - CameraFX::Hash22(neighbour));
			specks += present * (1.0 - smoothstep(radius * 0.6, radius, distance)) * (0.4 + 0.6 * CameraFX::Hash12(neighbour + 1.3));
		}
	}
	return specks;
}

float GetLensDirt(float2 uv)
{
	float2 p = uv * float2(GetAspect(), 1.0);
	float smudge = smoothstep(0.45, 0.8, CameraFX::Fbm(p * 2.2, 3));
	return GetSpecks(p * 14.0, 0.45, 1.0) * 0.6 + smudge * 0.5;
}

float4 GetHeatRipple(float2 uv, float heat)
{
	float band = smoothstep(0.5, 0.95, uv.y);
	float4 layer = 0.0;
	[branch] if (band > 0.0)
	{
		float time = GetTime();
		float noise = CameraFX::Fbm(float2(uv.x * 8.0, uv.y * 4.0 - time * 1.5), 2);
		float2 offset = float2(sin(uv.y * 60.0 + time * 8.0 + noise * 6.0), cos(uv.x * 40.0 + time * 6.0)) * 0.003 * heat;
		float3 color = SampleScene(uv + offset, 0.0) * lerp(float3(1.0, 1.0, 1.0), float3(1.04, 1.0, 0.95), heat);
		layer = Layer(color, band * heat * 0.9);
	}
	return layer;
}

float4 GetWaterSheet(float2 uv, float sheet)
{
	float time = GetTime();
	float2 p = uv * float2(GetAspect(), 1.0);
	float broad = CameraFX::Fbm(float2(p.x * 6.0, p.y * 2.0 - time * 1.2), 3);
	float drained = 1.0 - sheet;
	float coverage = smoothstep(drained - 0.1, drained + 0.1, uv.y + (broad - 0.5) * 0.3);
	float4 layer = 0.0;
	[branch] if (coverage > 0.0)
	{
		float fine = CameraFX::Fbm(float2(p.x * 14.0, p.y * 3.0 - time * 1.8), 2);
		float2 offset = float2(broad - 0.5, fine - 0.5) * 0.02 * sheet;
		float streaks = smoothstep(0.55, 0.75, CameraFX::Fbm(float2(p.x * 30.0, p.y * 1.5 - time * 0.6), 2));
		layer = Layer(SampleScene(uv + offset, 1.0) * 1.02, coverage * (0.5 + 0.3 * streaks) * saturate(sheet * 3.0));
	}
	return layer;
}

float4 GetRainDrops(float2 uv, float wet)
{
	float4 result = 0.0;
	float aspect = GetAspect();
	float time = GetTime();
	float2 p = uv * float2(aspect, 1.0);
	float density = wet * lerp(0.6, 1.4, saturate(CameraFX::Weather.y * 0.5 + 0.5));

	[unroll] for (int layer = 0; layer < 2; layer++)
	{
		float scale = layer == 0 ? 7.0 : 13.0;
		float2 gridPosition = p * scale + layer * 17.3;
		float2 cell = floor(gridPosition);
		float2 local = frac(gridPosition) - 0.5;
		float seed = CameraFX::Hash12(cell + 11.7 * layer);
		float present = step(seed, density * (layer == 0 ? 0.35 : 0.5));
		float cycle = frac(time * (0.05 + 0.08 * seed) + seed);
		float radius = (0.12 + 0.18 * CameraFX::Hash12(cell + 5.2)) * (1.0 - cycle * 0.6);
		float2 delta = local - (CameraFX::Hash22(cell + 3.1 * layer) - 0.5) * 0.4;
		float distance = length(delta) / radius;
		[branch] if (present > 0.0 && distance < 1.0)
		{
			float2 normal = delta / radius;
			float2 offset = -normal * (radius / scale) * 0.8 / float2(aspect, 1.0);
			float rim = smoothstep(0.75, 1.0, distance) * 0.35;
			float highlight = pow(saturate(1.0 - length(normal - float2(-0.35, -0.35)) * 2.5), 4.0) * 0.4;
			float3 color = SampleScene(uv + offset, 0.0) * (1.0 - rim) + highlight;
			float alpha = smoothstep(1.0, 0.85, distance) * (1.0 - pow(cycle, 4.0));
			result = Over(Layer(color, alpha), result);
		}
	}

	float column = floor(p.x * 9.0);
	float columnSeed = CameraFX::Hash12(float2(column, 91.7));
	[branch] if (columnSeed < density * 0.4)
	{
		float speed = 0.15 + 0.25 * CameraFX::Hash12(float2(column, 3.3));
		float head = frac(time * speed + columnSeed);
		float x = (frac(p.x * 9.0) - 0.5 + sin(uv.y * 20.0 + columnSeed * 6.28) * 0.08) / 9.0;
		float dy = uv.y - head;
		float drop = 1.0 - smoothstep(0.009, 0.013, length(float2(x, dy * 0.8)));
		float trail = (1.0 - smoothstep(0.0015, 0.003, abs(x))) * step(dy, 0.0) * (1.0 - smoothstep(0.0, 0.15, -dy)) * 0.6;
		float alpha = max(drop, trail);
		[branch] if (alpha > 0.0)
		{
			float2 offset = float2(-x * 0.6, -0.004 * drop);
			result = Over(Layer(SampleScene(uv + offset, 0.0) + drop * 0.05, alpha), result);
		}
	}
	return result;
}

float4 GetCondensation(float2 uv, float condensation)
{
	float mask = smoothstep(0.55, 1.0, CameraFX::EdgeDistance(uv)) * condensation;
	float4 layer = 0.0;
	[branch] if (mask > 0.0)
	{
		float2 p = uv * float2(GetAspect(), 1.0);
		float droplets = GetSpecks(p * 60.0, 0.6, 0.6);
		float3 color = SampleScene(uv, 2.0) * 0.95 + 0.05 + droplets * 0.08;
		layer = Layer(color, mask * (0.45 + 0.2 * droplets));
	}
	return layer;
}

float4 GetEntryFog(float2 uv, float fog)
{
	float2 p = uv * float2(GetAspect(), 1.0);
	float clear = (1.0 - fog) * 1.2;
	float mask = smoothstep(clear - 0.15, clear + 0.25, CameraFX::EdgeDistance(uv) + (CameraFX::Fbm(p * 4.0, 3) - 0.5) * 0.2);
	return Layer(SampleScene(uv, 4.0) * 0.8 + 0.15, mask * 0.85 * saturate(fog * 1.5));
}

float4 GetSplat(float2 delta, float radius, float opacity, float type, float seed, float age)
{
	uint kind = (uint)(type + 0.5);
	float distance = length(delta) / radius;
	float edge = 1.0 + (CameraFX::ValueNoise(GetDirection(delta) * 3.0 + seed * float2(20.0, 7.0)) - 0.5) * 0.5;
	float core = smoothstep(edge, edge * 0.8, distance);
	float spatter = step(0.82, CameraFX::ValueNoise(delta / radius * 6.0 + seed * 13.0)) * smoothstep(2.0, 1.0, distance);
	float body = max(core, spatter);

	bool liquid = kind == SplatBlood || kind == SplatDarkBlood || kind == SplatOil || kind == SplatVenom;
	[branch] if (liquid)
	{
		[unroll] for (int i = 0; i < 3; i++)
		{
			float x = (CameraFX::Hash12(float2(seed * 31.0, i)) - 0.5) * radius * 1.2;
			float dripLength = min(age * 0.05, 0.12) * CameraFX::Hash12(float2(seed * 17.0, i + 4.0));
			float t = saturate(delta.y / max(dripLength, 1e-4));
			float width = radius * 0.12 * (1.0 - t * 0.5);
			float drip = step(0.0, delta.y) * step(delta.y, dripLength) * (1.0 - smoothstep(width * 0.6, width, abs(delta.x - x)));
			body = max(body, drip);
		}
	}

	float4 layer = 0.0;
	if (kind == SplatBlood)
		layer = Layer(float3(0.35, 0.02, 0.02) + core * 0.04, body * 0.85);
	else if (kind == SplatDarkBlood)
		layer = Layer(float3(0.15, 0.02, 0.03), body * 0.85);
	else if (kind == SplatOil)
		layer = Layer(float3(0.03, 0.03, 0.035) + pow(saturate(1.0 - distance), 6.0) * 0.08, body * 0.9);
	else if (kind == SplatVenom)
		layer = Layer(float3(0.25, 0.4, 0.05), body * 0.7);
	else if (kind == SplatIce) {
		float crystals = pow(saturate(1.0 - abs(CameraFX::Fbm(delta / radius * 4.0 + seed * 9.0, 3) * 2.0 - 1.0)), 4.0);
		layer = Layer(FrostTint * (0.65 + 0.35 * crystals), core * (0.45 + 0.3 * crystals));
	} else if (kind == SplatFire) {
		float cooled = saturate(age / 1.5);
		float alpha = core * lerp(0.6, 0.8, cooled);
		layer = float4(lerp(FireColor * 2.0, float3(0.05, 0.035, 0.03), cooled) * alpha, saturate(alpha));
	} else if (kind == SplatGrit)
		layer = Layer(float3(0.3, 0.25, 0.2), GetSpecks(delta / radius * 5.0 + seed * 11.0, 0.7, 0.8) * smoothstep(1.4, 0.6, distance));
	return layer * opacity;
}

float4 GetSplats(float2 uv)
{
	float4 result = 0.0;
	float aspect = GetAspect();
	uint count = min((uint)(CameraFX::Combat2.w + 0.5), CameraFX::MaxSplats);
	[loop] for (uint i = 0; i < count; i++)
	{
		float4 placement = CameraFX::Splats[i * 2];
		float4 data = CameraFX::Splats[i * 2 + 1];
		float2 delta = (uv - placement.xy) * float2(aspect, 1.0);
		float reach = placement.z * 2.2;
		[branch] if (placement.w > 0.001 && abs(delta.x) < reach && delta.y > -reach && delta.y < reach + 0.15)
			result = Over(GetSplat(delta, placement.z, placement.w, data.x, data.y, data.z), result);
	}
	return result;
}

float4 GetSlash(float2 uv, float slash, float angle)
{
	float2 direction = float2(cos(angle), sin(angle));
	float2 normal = float2(-direction.y, direction.x);
	float2 q = (uv - 0.5) * float2(GetAspect(), 1.0);
	float along = dot(q, direction);
	float across = abs(dot(q, normal) - (1.0 - slash) * 0.1) + (CameraFX::ValueNoise(float2(along * 30.0, 1.0)) - 0.5) * 0.02;
	float band = smoothstep(0.035, 0.0, across) * smoothstep(0.75, 0.3, abs(along));
	return Layer(float3(0.25, 0.02, 0.02), band * slash * 0.7);
}

float4 GetAtmosphere(float2 uv, float ash, float dust)
{
	float2 p = uv * float2(GetAspect(), 1.0);
	float edge = smoothstep(0.5, 1.0, CameraFX::EdgeDistance(uv));
	float4 result = 0.0;
	[branch] if (ash > 0.001 && edge > 0.0)
	{
		float specks = GetSpecks(p * 40.0, 0.3, 0.7);
		result = Over(Layer(float3(0.25, 0.25, 0.25), (specks * 0.8 + 0.15) * edge * ash), result);
	}
	[branch] if (dust > 0.001)
	{
		float2 drift = float2(-CameraFX::Wind.y * 0.2, 0.0);
		float specks = GetSpecks((p + drift) * 30.0, 0.25, 0.6);
		float haze = smoothstep(0.7, 1.0, uv.y) * 0.25 + edge * 0.15;
		result = Over(Layer(float3(0.45, 0.38, 0.28) * 0.6, (haze + specks * 0.5) * dust), result);
	}
	return result;
}

float4 GetBreath(float2 uv, float visible, float age, float strength)
{
	float rise = age * 0.12;
	float2 center = float2(0.5, 1.02 - rise);
	float radius = 0.12 + age * 0.12;
	float distance = length((uv - center) * float2(GetAspect(), 1.6));
	float4 layer = 0.0;
	[branch] if (distance < radius)
	{
		float noise = CameraFX::Fbm(uv * 6.0 + float2(0.0, -age * 0.5), 3);
		float density = smoothstep(radius, radius * 0.2, distance) * noise * 1.2 * (1.0 - smoothstep(0.8, 2.5, age)) * smoothstep(0.0, 0.25, age);
		layer = Layer(SampleScene(uv, 3.0) * 0.6 + 0.35, density * visible * strength * 0.55);
	}
	return layer;
}

float3 GetSunGlare(float2 uv, float amount, float dirt)
{
	float2 sun = CameraFX::SunScreen.xy;
	float2 aspect = float2(CameraFX::SunScreen.w, 1.0);
	float distance = length((uv - sun) * aspect);
	float veil = exp(-distance * 3.0) * 0.18 + exp(-distance * 14.0) * 0.35;
	float lit = dirt * CameraFX::Lens.w * exp(-distance * 1.8) * 1.4;

	float2 axis = 0.5 - sun;
	float ghosts = 0.0;
	[unroll] for (int i = 1; i <= 3; i++)
	{
		float2 position = sun + axis * (0.6 + 0.55 * i);
		float radius = 0.03 + 0.025 * i;
		ghosts += smoothstep(radius, radius * 0.6, length((uv - position) * aspect)) * (0.05 / i);
	}
	return CameraFX::SunColor.rgb * amount * (veil + lit + ghosts);
}

float3 GetMoonGlow(float2 uv, float4 moon, float amount, float dirt)
{
	float distance = length((uv - moon.xy) * float2(GetAspect(), 1.0));
	float halo = exp(-distance / max(moon.w, 1e-3) * 0.35) * 0.12;
	float lit = dirt * CameraFX::Lens.w * exp(-distance * 3.0) * 0.5;
	return MoonTint * amount * (halo + lit);
}

float3 GetEmbers(float2 uv, float embers)
{
	float time = GetTime();
	float2 p = uv * float2(GetAspect(), 1.0) * 12.0 + float2(-CameraFX::Wind.y * 2.0, time * 1.5);
	float2 cell = floor(p);
	float2 local = frac(p) - (CameraFX::Hash22(cell) * 0.8 + 0.1);
	float present = step(0.8, CameraFX::Hash12(cell + 2.7));
	float flicker = 0.6 + 0.4 * sin(time * (6.0 + 6.0 * CameraFX::Hash12(cell)) + CameraFX::Hash12(cell + 9.1) * 6.28);
	return EmberColor * present * exp(-dot(local, local) * 900.0) * flicker * embers * 2.0;
}

float3 GetShock(float2 uv, float shock)
{
	float mask = smoothstep(0.55, 1.0, CameraFX::EdgeDistance(uv));
	float3 glow = 0.0;
	[branch] if (mask > 0.0)
	{
		float time = GetTime();
		float2 p = uv * float2(GetAspect(), 1.0);
		float noise = CameraFX::Fbm(p * 5.0 + float2(time * 2.3, -time * 1.7), 3);
		float arc = 1.0 - smoothstep(0.0, 0.035, abs(noise - 0.5));
		float flicker = 0.6 + 0.4 * sin(time * 9.0 + noise * 10.0);
		glow = ShockColor * arc * mask * shock * flicker * 1.5;
	}
	return glow;
}

float4 GetOverlay(float2 uv)
{
	float4 result = 0.0;

	[branch] if (CameraFX::Weather3.x > 0.001)
		result = Over(GetHeatRipple(uv, saturate(CameraFX::Weather3.x)), result);

	[branch] if (CameraFX::Weather.z > 0.001)
		result = Over(GetWaterSheet(uv, CameraFX::Weather.z), result);

	[branch] if (CameraFX::Weather.x > 0.001)
		result = Over(GetRainDrops(uv, saturate(CameraFX::Weather.x)), result);

	[branch] if (CameraFX::Weather2.x > 0.001)
		result = Over(GetCondensation(uv, saturate(CameraFX::Weather2.x)), result);

	float frost = saturate(CameraFX::Lens.x);
	[branch] if (frost > 0.001)
	{
		float crystals;
		float mask = GetFrost(uv, frost, crystals);
		[branch] if (mask > 0.0)
		{
			float3 glass = SampleScene(uv, 3.0) * FrostTint * 0.85 + FrostTint * crystals * 0.6;
			result = Over(Layer(glass, mask * 0.9), result);
		}
	}

	[branch] if (CameraFX::Weather.w > 0.001)
		result = Over(GetEntryFog(uv, CameraFX::Weather.w), result);

	[branch] if (CameraFX::Combat2.w > 0.5)
		result = Over(GetSplats(uv), result);

	[branch] if (CameraFX::Combat2.x > 0.001)
		result = Over(GetSlash(uv, CameraFX::Combat2.x, CameraFX::Combat2.y), result);

	[branch] if (CameraFX::Weather2.y > 0.001 || CameraFX::Weather2.z > 0.001)
		result = Over(GetAtmosphere(uv, saturate(CameraFX::Weather2.y), saturate(CameraFX::Weather2.z)), result);

	[branch] if (CameraFX::Weather3.y > 0.001 && CameraFX::Weather3.z < 2.5)
		result = Over(GetBreath(uv, CameraFX::Weather3.y, CameraFX::Weather3.z, CameraFX::Weather3.w), result);

	[branch] if (CameraFX::Combat.w > 0.001)
	{
		float pulse = exp(-CameraFX::Weather3.z * 3.0);
		result = Over(Layer(0.0, CameraFX::Combat.w * pulse * smoothstep(0.4, 1.0, CameraFX::EdgeDistance(uv)) * 0.35), result);
	}
	return result;
}

float3 GetAdditive(float2 uv)
{
	float3 additive = 0.0;
	float sunAmount = CameraFX::Lens.y > 0.001 ? CameraFX::Lens.y * CameraFX::SunScreen.z * LightVisibility.Load(int3(0, 0, 0)) : 0.0;
	float masserAmount = CameraFX::MoonScreen[0].z > 0.001 ? CameraFX::MoonScreen[0].z * LightVisibility.Load(int3(1, 0, 0)) : 0.0;
	float secundaAmount = CameraFX::MoonScreen[1].z > 0.001 ? CameraFX::MoonScreen[1].z * LightVisibility.Load(int3(2, 0, 0)) : 0.0;
	[branch] if (sunAmount > 0.001 || masserAmount > 0.001 || secundaAmount > 0.001)
	{
		float dirt = GetLensDirt(uv);
		if (sunAmount > 0.001)
			additive += GetSunGlare(uv, sunAmount, dirt);
		if (masserAmount > 0.001)
			additive += GetMoonGlow(uv, CameraFX::MoonScreen[0], masserAmount, dirt);
		if (secundaAmount > 0.001)
			additive += GetMoonGlow(uv, CameraFX::MoonScreen[1], secundaAmount, dirt);
	}
	[branch] if (CameraFX::Weather2.w > 0.001)
		additive += GetEmbers(uv, saturate(CameraFX::Weather2.w));
	[branch] if (CameraFX::Combat.y > 0.001)
		additive += GetShock(uv, saturate(CameraFX::Combat.y));
	[branch] if (CameraFX::Combat.z > 0.001)
		additive += FireColor * smoothstep(0.5, 1.05, CameraFX::EdgeDistance(uv)) * saturate(CameraFX::Combat.z) * (0.6 + 0.2 * sin(GetTime() * 5.0));
	[branch] if (CameraFX::Combat.x > 0.001)
		additive += lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.6, 0.5), saturate(CameraFX::Combat2.z)) * CameraFX::Combat.x * (0.5 + 0.5 * CameraFX::EdgeDistance(uv));
	return additive;
}

float4 main(CameraFX::Interpolants input) : SV_Target
{
	float2 uv = input.TexCoord;
	float mode = CameraFX::Output.x;
	bool uiLayer = mode > 0.5 && mode < 2.5;

	float4 result = 0.0;
	[branch] if (mode < 2.5)
		result = GetOverlay(uv);

	float3 additive = 0.0;
	[branch] if (mode < 1.5 || mode > 2.5)
		additive = GetAdditive(uv);

	float visible = 1.0 - saturate(CameraFX::FadeColor.a);
	result *= visible;
	additive *= visible;
	result.a = saturate(result.a);

	float4 output;
	[branch] if (uiLayer)
	{
		float gain = max(CameraFX::Output.w, 1e-3);
		output = float4((result.rgb + additive) / gain, result.a);
	}
	else if (CameraFX::Output.z > 0.5)
	{
		float3 sceneLinear = SceneTexture.SampleLevel(LinearSampler, uv, 0.0).rgb;
		float3 composite = result.rgb + CameraFX::ToWorking(sceneLinear) * (1.0 - result.a) + additive;
		output = float4(CameraFX::FromWorking(composite) - sceneLinear * (1.0 - result.a), result.a);
	}
	else
	{
		output = float4(result.rgb + additive, result.a);
	}
	return output;
}
