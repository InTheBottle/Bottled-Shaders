#include "CameraFX/Common.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float> LightVisibility : register(t2);
Texture2D<float> LensDirtTexture : register(t3);
Texture2D<float4> FrostTexture : register(t4);
SamplerState LinearSampler : register(s0);
SamplerState WrapSampler : register(s1);

static const float2 LensLight = float2(-0.53, -0.848);
static const float3 IceTint = float3(0.88, 0.94, 1.0);
static const float3 MoonTint = float3(0.78, 0.85, 1.0);
static const float3 EmberHot = float3(1.0, 0.72, 0.32);
static const float3 EmberCool = float3(1.0, 0.36, 0.08);
static const float3 ArcColor = float3(0.3, 0.58, 1.0);
static const float ReferenceHeight = 1080.0;

static const uint SplatBlood = 0;
static const uint SplatDarkBlood = 1;
static const uint SplatOil = 2;
static const uint SplatVenom = 3;
static const uint SplatIce = 4;
static const uint SplatFire = 5;
static const uint SplatGrit = 6;

struct Composite
{
	float3 color;
	float alpha;
	float3 light;
};

struct Water
{
	float mask;
	float2 offset;
	float2 normal;
	float shine;
	float rim;
};

float Aspect()
{
	return CameraFX::Resolution.x * CameraFX::Resolution.w;
}

float Time()
{
	return CameraFX::ThemeParams.w;
}

float2 ToUV(float2 offset)
{
	return float2(offset.x / Aspect(), offset.y);
}

float3 Scene(float2 uv, float mip)
{
	return CameraFX::ToWorking(SceneTexture.SampleLevel(LinearSampler, saturate(uv), clamp(mip, 0.0, CameraFX::Blur.w)).rgb);
}

float MipForRadius(float referencePixels)
{
	return log2(1.0 + max(referencePixels, 0.0) * CameraFX::Resolution.y / ReferenceHeight * 0.7);
}

float LightLevel(float3 color)
{
	return saturate(CameraFX::Luminance(color));
}

void Blend(inout Composite c, float3 target, float amount)
{
	amount = saturate(amount);
	c.color = lerp(c.color, target, amount);
	c.alpha = 1.0 - (1.0 - c.alpha) * (1.0 - amount);
}

float Dirt(float2 q, float scale, float2 shift)
{
	float dirt;
	// LensLook.z is the baked texture's size: one texel per screen pixel is mip 0, and each halving steps a level.
	[branch] if (CameraFX::LensLook.z > 0.5)
		dirt = LensDirtTexture.SampleLevel(WrapSampler, q * scale * 0.85 + shift, max(0.0, log2(CameraFX::LensLook.z * scale * 0.85 * CameraFX::Resolution.w)));
	else
		dirt = smoothstep(0.55, 0.9, CameraFX::Fbm(q * scale * 9.0 + shift * 17.0, 3)) * 0.6;
	return dirt;
}

float Sparkle(float2 q, float cellPixels, float chance, float salt)
{
	float2 p = q * ReferenceHeight / cellPixels;
	float2 cell = floor(p);
	float pick = CameraFX::Hash12(cell + salt);
	float value = 0.0;
	[branch] if (pick < chance)
	{
		float2 centre = 0.3 + 0.4 * CameraFX::Hash22(cell + salt * 1.3);
		float2 d = (frac(p) - centre) * cellPixels;
		float d2 = dot(d, d);
		float twinkle = 0.6 + 0.4 * sin(Time() * (0.7 + 2.1 * CameraFX::Hash12(cell + 2.1)) + pick * 157.0);
		value = (exp(-d2 / 0.7) + 0.1 * exp(-d2 / 3.0)) * (0.8 + 2.0 * CameraFX::Hash12(cell + 4.7)) * twinkle;
	}
	return value;
}

void AddDrop(inout Water water, float2 q, float cellSize, float salt, float spawn, float slide, float lifetime)
{
	float2 g = q / cellSize + salt * float2(3.71, 1.93);
	float2 cell = floor(g);
	float2 f = frac(g);
	[branch] if (CameraFX::Hash12(cell + salt * 1.7) >= spawn)
		return;

	float life = lifetime * (0.65 + 0.7 * CameraFX::Hash12(cell + 3.1));
	float phase = Time() / life + CameraFX::Hash12(cell + 8.3);
	float t = frac(phase);
	float2 seed = cell + floor(phase) * float2(1.37, 2.91);
	float2 jitter = CameraFX::Hash22(seed + salt);
	float size = CameraFX::Hash12(seed + 5.9);
	bool heavy = size > 0.62;
	float2 centre = float2(0.3 + 0.4 * jitter.x, heavy ? 0.3 + 0.15 * jitter.y : 0.3 + 0.4 * jitter.y);
	centre.y += heavy ? slide * t * t : 0.0;
	float radius = (0.1 + 0.14 * size) * (1.0 - 0.3 * t);

	float2 d = f - centre;
	float sag = d.y > 0.0 ? 0.9 : 1.08;
	float angle = atan2(d.y, d.x);
	float wobble = 1.0 + 0.07 * sin(2.0 * angle + jitter.x * 6.283) + 0.04 * sin(3.0 * angle + jitter.y * 6.283);
	float2 shaped = float2(d.x, d.y * sag) / (radius * wobble);
	float r = length(shaped);
	float presence = smoothstep(0.0, 0.05, t) * (1.0 - smoothstep(0.72, 1.0, t));
	float mask = (1.0 - smoothstep(0.8, 1.0, r)) * presence;
	[branch] if (mask <= water.mask)
		return;

	water.mask = mask;
	water.normal = shaped;
	water.offset = -shaped * radius * cellSize * 1.4;
	water.rim = smoothstep(0.55, 1.0, r);
	float facing = saturate(dot(shaped, LensLight));
	float2 spot = shaped - LensLight * 0.42;
	water.shine = pow(facing, 3.0) * smoothstep(0.35, 0.95, r) * 0.9 + exp(-dot(spot, spot) * 45.0) * 0.8;
}

void AddRivulets(inout Water water, float2 q, float amount)
{
	const float columns = 14.0;
	float column = floor(q.x * columns);
	float h0 = CameraFX::Hash12(float2(column, 4.7));
	[branch] if (h0 >= amount * 0.55)
		return;
	float h1 = CameraFX::Hash12(float2(column, 9.1));
	float h2 = CameraFX::Hash12(float2(column, 13.3));
	float h3 = CameraFX::Hash12(float2(column, 17.9));

	const float span = 1.7;
	float speed = 0.06 + 0.1 * h1;
	float t = frac(Time() * speed / span + h2);
	float hops = t * 9.0;
	float head = (floor(hops) + smoothstep(0.25, 1.0, frac(hops))) / 9.0 * span - 0.3;

	float meander = sin(q.y * (4.0 + 5.0 * h3) + h1 * 6.283) * 0.17 + (CameraFX::ValueNoise(float2(column * 2.3, q.y * 8.0)) - 0.5) * 0.22;
	float dx = (frac(q.x * columns) - 0.5 - meander) / columns;
	float headRadius = 0.0042 + 0.004 * h3;
	float behind = head - q.y;
	float trailLength = 0.2 + 0.35 * h2;
	float trail = behind > 0.0 ? saturate(1.0 - behind / trailLength) : 0.0;
	float width = headRadius * (0.25 + 0.45 * pow(trail, 1.3));
	float beads = behind > trailLength * 0.35 ? smoothstep(0.3, 0.62, CameraFX::ValueNoise(float2(column * 5.3, behind * 85.0))) : 1.0;
	float trailMask = (1.0 - smoothstep(0.5, 1.0, abs(dx) / width)) * saturate(trail * 5.0) * beads * 0.85;
	float2 headShape = float2(dx, (q.y - head) * 0.75 + headRadius * 0.25) / headRadius;
	float headMask = 1.0 - smoothstep(0.65, 1.0, length(headShape));
	float mask = max(trailMask, headMask);
	[branch] if (mask <= water.mask)
		return;

	bool onHead = headMask >= trailMask;
	float2 n = onHead ? headShape : float2(dx / width, 0.0);
	water.mask = mask;
	water.normal = n;
	water.offset = -n * (onHead ? headRadius : width) * 3.5;
	water.rim = saturate(length(n) * 1.2 - 0.3);
	water.shine = saturate(pow(saturate(dot(n, LensLight) * 1.3), 2.5) * 1.6);
}

float2 ApplyWater(inout Composite c, float2 uv, float2 q, out Water water)
{
	water = (Water)0;
	float time = Time();
	float rain = saturate(CameraFX::Weather.x);
	float sheet = saturate(CameraFX::Weather.z);
	float extra = saturate(CameraFX::Weather3.x);
	float thaw = saturate(CameraFX::LensLook.x);

	float drops = max(max(rain, extra), thaw * 0.5);
	[branch] if (drops > 0.001)
	{
		float density = drops * lerp(0.75, 1.25, saturate(CameraFX::Weather.y * 0.5 + 0.5));
		AddDrop(water, q, 0.09, 1.0, saturate(density * 0.27), 0.25, 9.0);
		AddDrop(water, q + 0.37, 0.052, 2.0, saturate(density * 0.37), 0.15, 7.0);
		AddDrop(water, q + 0.71, 0.028, 3.0, saturate(density * 0.46), 0.0, 5.0);
		AddRivulets(water, q, saturate(max(max(rain, extra * 1.6), thaw * 0.8)));
		[branch] if (rain <= 0.001 && extra <= 0.001)
		{
			float edge = smoothstep(0.25, 0.55, length(uv - 0.5));
			water.mask *= edge;
			water.shine *= edge;
		}
	}

	float sheetMask = 0.0;
	float sheetBody = 0.0;
	float front = 0.0;
	float streak = 0.0;
	float2 sheetOffset = 0.0;
	[branch] if (sheet > 0.001)
	{
		float flow = CameraFX::ValueNoise(uv * float2(7.0, 3.5) + float2(0.0, time * 0.8));
		float top = (1.0 - sheet) * 1.2 - 0.12;
		float level = uv.y + (flow - 0.5) * 0.28;
		sheetMask = smoothstep(top - 0.06, top + 0.14, level);
		front = exp(-pow((level - top - 0.02) / 0.025, 2.0)) * saturate(sheet * 4.0);
		float2 film = float2(CameraFX::ValueNoise(q * float2(5.0, 2.4) + float2(0.0, time * 0.9)), CameraFX::ValueNoise(q * float2(5.0, 2.4) + float2(13.7, time * 0.9))) - 0.5;
		streak = smoothstep(0.55, 0.9, CameraFX::Fbm(float2(q.x * 90.0, uv.y * 1.6 - time * 0.9), 3));
		sheetOffset = (film * 0.016 + float2(streak * 0.0012, streak * 0.003)) * sheetMask * sheet;
		sheetBody = sheetMask * saturate(sheet * 2.5);
	}

	float2 viewUV = uv + ToUV(water.offset) * water.mask + sheetOffset;
	float coverage = max(water.mask, max(sheetBody, front * 0.6));
	[branch] if (coverage > 0.001)
	{
		float3 seen = Scene(viewUV, sheetMask * sheet * 1.8);
		seen *= 1.0 - water.rim * water.mask * 0.35 - front * 0.1;
		float level = 0.25 + LightLevel(seen);
		seen += (water.shine * water.mask * 0.28 + front * 0.14 + streak * sheetBody * 0.04) * level;
		Blend(c, seen, coverage);
	}
	return viewUV;
}

void ApplyDust(inout Composite c, float2 q, float edgeR, float dust)
{
	float time = Time();
	float3 dustColor = float3(0.62, 0.53, 0.42);
	float lum = LightLevel(c.color);
	float gust = smoothstep(0.42, 0.85, CameraFX::Fbm(float2(q.x * 1.4 - time * 0.9 - CameraFX::Wind.y * 0.3, q.y * 12.0 + sin(time * 0.3)), 4));
	float clearCentre = 0.25 + 0.75 * smoothstep(0.2, 0.62, edgeR + (CameraFX::Fbm(q * 1.5 + time * 0.05, 3) - 0.5) * 0.15);
	Blend(c, dustColor * (0.5 + 0.5 * lum), dust * (0.1 + 0.2 * gust) * clearCentre);
	float grit = saturate(Dirt(q, 2.3, float2(0.47, 0.83)) * 1.5);
	float edge = smoothstep(0.55, 1.05, edgeR + (CameraFX::Fbm(q * 2.0, 3) - 0.5) * 0.3);
	Blend(c, dustColor * 0.55, grit * edge * dust * 0.45);
}

void ApplyAsh(inout Composite c, float2 uv, float2 q, float edgeR, float ash)
{
	float time = Time();
	float amount = saturate(ash);
	float storm = saturate((ash - 0.45) * 1.8);
	float3 ashColor = float3(0.41, 0.4, 0.38);
	float lum = LightLevel(c.color);

	float slide = time * (0.005 + 0.012 * storm);
	float2 settled = q + float2(sin(time * 0.13 + uv.y * 3.0) * 0.012, -slide);
	float reach = edgeR + (CameraFX::Fbm(q * 3.0 - float2(0.0, slide * 3.0), 3) - 0.5) * 0.3;
	float edge = smoothstep(0.45, 1.05, reach);
	float broken = smoothstep(0.3, 0.7, CameraFX::ValueNoise(settled * 140.0));
	float flakes = saturate(Dirt(settled, 3.4, float2(0.31, 0.57)) * 1.6 + Dirt(settled, 1.8, float2(0.62, 0.14)) * 0.8) * (0.35 + 0.65 * broken);
	float film = smoothstep(0.45, 0.85, CameraFX::Fbm(q * 9.0 - float2(0.0, slide * 9.0), 3));
	Blend(c, ashColor * (0.55 + 0.45 * lum), saturate(edge * (flakes * 0.85 + film * 0.5)) * amount * 0.5);

	float2 nearDrift = float2(sin(time * 0.31) * 0.05 + sin(time * 0.07 + 1.3) * 0.08 - CameraFX::Wind.y * 0.2, -time * (0.035 + 0.11 * storm));
	float2 farDrift = float2(sin(time * 0.23 + 2.1) * 0.03 - CameraFX::Wind.y * 0.1, -time * (0.018 + 0.06 * storm));
	float nearFlakes = Dirt(q, 2.0, nearDrift * 2.0) * smoothstep(0.25, 0.6, CameraFX::ValueNoise(q * 90.0 + nearDrift * 40.0));
	float farFlakes = Dirt(q, 4.2, farDrift * 4.2);
	float airborne = saturate(pow(max(nearFlakes, 0.0), 1.4) * 1.1 + pow(max(farFlakes, 0.0), 1.4) * 0.7) * amount * (0.35 + 0.4 * storm);
	Blend(c, ashColor * (0.7 + 0.3 * lum), airborne * 0.3);
	Blend(c, ashColor, amount * (0.1 + 0.06 * storm) * 0.25);
}

float3 Embers(float2 q, float amount)
{
	float time = Time();
	float3 light = 0.0;
	[unroll] for (int layer = 0; layer < 3; layer++)
	{
		float scale = layer == 0 ? 7.0 : (layer == 1 ? 12.0 : 20.0);
		float rise = 0.2 + 0.12 * layer;
		float2 drift = float2(sin(time * 0.35 + layer * 1.7) * 0.35 + time * 0.06 - CameraFX::Wind.y * (0.4 + 0.2 * layer), -time * rise);
		float2 g = q * scale + drift * scale * 0.5;
		float2 cell = floor(g);
		float pick = CameraFX::Hash12(cell + 19.0 * layer);
		[branch] if (pick < 0.045)
		{
			float2 local = frac(g) - 0.5 - (CameraFX::Hash22(cell + 7.0 + layer) - 0.5) * 0.4;
			float2 velocity = normalize(float2(0.35 * sin(time * 0.35 + layer * 1.7) + 0.12, -1.0));
			float along = dot(local, velocity);
			float across = dot(local, float2(-velocity.y, velocity.x));
			float size = 0.05 - 0.012 * layer;
			float spark = exp(-(across * across) / (size * size * 0.25) - (along * along) / (size * size * 1.6));
			float life = sin(3.14159 * frac(time * (0.16 + 0.1 * pick) + pick * 7.0));
			float cooling = frac(time * 0.1 + pick * 3.0);
			float2 inCell = abs(frac(g) - 0.5);
			float edgeFade = 1.0 - smoothstep(0.3, 0.46, max(inCell.x, inCell.y));
			float glow = 0.85 + 0.15 * sin(time * (1.3 + 1.4 * pick) + pick * 41.0);
			light += lerp(EmberHot, EmberCool, cooling) * spark * glow * life * life * edgeFade * (1.0 - 0.25 * layer);
		}
	}
	return light * amount * 1.2;
}

float3 MistHalo(float2 viewUV, float fog)
{
	float3 halo = Scene(viewUV, 4.5);
	return halo * smoothstep(0.55, 1.1, LightLevel(halo)) * fog * 0.16;
}

void ApplyMist(inout Composite c, float2 q, float2 viewUV, float edgeR, float fog)
{
	c.light += MistHalo(viewUV, fog);
	float3 veil = Scene(viewUV, 2.5);
	Blend(c, veil * 0.85 + (0.03 + LightLevel(veil) * 0.25) * float3(0.9, 0.94, 1.0), fog * 0.28);
	float edge = smoothstep(0.5, 1.05, edgeR + (CameraFX::Fbm(q * 2.5, 3) - 0.5) * 0.35);
	float beads = saturate(Dirt(q, 3.1, float2(0.19, 0.71)) * 1.8);
	Blend(c, Scene(viewUV, 2.0) * 1.03 + 0.035, saturate(edge * (0.45 + 0.55 * beads) * fog * 1.3) * 0.45);
}

void ApplyEntryFog(inout Composite c, float2 q, float2 viewUV, float edgeR, float fog)
{
	float clearRadius = (1.0 - fog) * 0.8;
	float mask = smoothstep(clearRadius, clearRadius + 0.22, edgeR * 0.7071 + (CameraFX::Fbm(q * 3.0, 3) - 0.5) * 0.25) * saturate(fog * 1.5);
	[branch] if (mask > 0.001)
	{
		float3 soft = Scene(viewUV, MipForRadius(18.0 * mask));
		float beads = saturate(Dirt(q, 3.1, float2(0.37, 0.11)) * 1.6);
		float lift = 0.14 + LightLevel(soft) * 0.1;
		Blend(c, soft * 0.8 + lift * float3(0.96, 0.98, 1.0), mask * 0.7);
		Blend(c, soft * 0.85 + lift * 1.3, beads * mask * 0.28);
	}
}

void ApplyBreath(inout Composite c, float2 uv, float2 q, float2 viewUV, float visible, float age, float strength, float seed)
{
	float envelope = smoothstep(0.0, 0.14, age) * (1.0 - smoothstep(1.1, 2.8, age));
	[branch] if (envelope * visible <= 0.001)
		return;
	float time = Time();
	float aspect = Aspect();
	float grow = 1.0 - exp(-age * 1.45);
	float side = CameraFX::Hash12(float2(seed, 2.0)) < 0.5 ? -1.0 : 1.0;
	float drift = (CameraFX::Hash12(float2(seed, 3.0)) - 0.3) * 0.14 * side;
	float lift = (0.13 + 0.06 * strength) * (0.6 + 0.6 * CameraFX::Hash12(float2(seed, 4.0)));
	float2 mouth = float2(0.5 * aspect, 0.86);

	float body = 0.0;
	float2 shape = 0.0;
	[unroll] for (int k = 0; k < 3; k++)
	{
		float2 jitter = CameraFX::Hash22(float2(seed * 1.7, k)) - 0.5;
		float delay = k * 0.18 * CameraFX::Hash12(float2(seed, k + 7.0));
		float lobeGrow = 1.0 - exp(-max(age - delay, 0.0) * 1.45);
		float2 centre = mouth + float2(drift * lobeGrow * lobeGrow + jitter.x * 0.09 * lobeGrow, -lift * lobeGrow - jitter.y * 0.05 * lobeGrow);
		centre.x += sin(time * 0.6 + age * 1.3 + k) * 0.008;
		float radius = (0.045 + 0.15 * lobeGrow) * (0.7 + 0.45 * CameraFX::Hash12(float2(seed, k + 11.0)));
		float2 d = (q - centre) / (radius * float2(1.3, 1.0));
		float lobe = exp(-dot(d, d) * 1.7) * smoothstep(0.0, 0.1, age - delay);
		if (lobe > body)
			shape = d;
		body = max(body, lobe);
	}
	[branch] if (body <= 0.01)
		return;

	float2 p = (q - mouth) / (0.06 + 0.16 * grow);
	float2 warp = float2(CameraFX::Fbm(p * 1.3 + float2(age * 0.4 + seed, 3.1), 3), CameraFX::Fbm(p * 1.3 + float2(5.7, seed - age * 0.4), 3)) - 0.5;
	float billow = CameraFX::Fbm(p * 2.1 + warp * 1.5 + float2(seed, age * 0.5), 4);
	float wisps = CameraFX::Fbm(p * 5.2 + warp * 0.8 + float2(seed, age * 0.9), 3);
	float density = saturate(body * (0.5 + 1.4 * (billow - 0.4)) + body * (wisps - 0.5) * 0.4);
	density = smoothstep(0.05, 0.45, density);
	density *= 1.0 - 0.7 * smoothstep(0.4, 2.6, age);
	density *= 1.0 - 0.85 * smoothstep(0.35, 0.62, 1.0 - uv.y);
	float alpha = saturate(density * envelope * visible * (0.65 + 0.35 * strength) * 0.6);
	[branch] if (alpha > 0.002)
	{
		float3 behind = Scene(viewUV + warp * 0.004 * alpha, 2.0 + 2.5 * alpha);
		float lit = saturate(0.55 - shape.y * 0.35 + (billow - 0.5) * 1.2);
		float tone = lerp(LightLevel(behind), 0.78, 0.55) * (0.8 + 0.4 * lit);
		Blend(c, lerp(behind, tone * float3(0.95, 0.97, 1.0), 0.85), alpha);
	}
}

float IceBend(float2 uv, float n, float growth, float cold, bool baked)
{
	float baseArrival = baked ? FrostTexture.SampleLevel(LinearSampler, uv, 0).g : CameraFX::FrameArrival(uv, 4.5);
	float arrival = lerp(baseArrival, CameraFX::FrameArrival(uv, n), 0.5);
	float lead = growth - arrival;
	float glaze = smoothstep(-0.06, 0.0, lead);
	float age = saturate(lead / 0.5);
	float profile = pow(saturate(1.0 - arrival / 0.72), 1.6);
	return profile * glaze * ((0.12 + 0.88 * pow(age, 1.2)) * (0.25 + 0.75 * cold) + 0.12);
}

void ApplyFrost(inout Composite c, float2 uv, float2 q, float2 viewUV, float frostAmount, float3 ambient, inout float frostCover)
{
	float amount = saturate(frostAmount);
	float growth = pow(amount, 1.6);
	float n = lerp(8.0, 2.6, smoothstep(0.25, 0.9, amount));
	bool baked = CameraFX::LensLook.y > 0.5;
	float4 tex = baked ? FrostTexture.SampleLevel(LinearSampler, uv, 0) : float4(0.0, saturate(CameraFX::FrameArrival(uv, 4.5) + (CameraFX::Fbm(q * 2.6, 4) - 0.5) * 0.24), 0.5, 0.0);
	float arrival = lerp(tex.g, CameraFX::FrameArrival(uv, n), 0.5);
	float lead = growth - arrival;
	float cover = smoothstep(-0.17, 0.0, lead + (CameraFX::Fbm(q * 6.0, 3) - 0.5) * 0.08);
	float fernStart = 0.54 + 0.46 * saturate(tex.g * 1.6 + (CameraFX::ValueNoise(q * 9.6) - 0.5) * 0.1);
	float fernIn = smoothstep(fernStart - 0.06, fernStart, amount) * (0.6 + 0.4 * saturate((amount - 0.5) * 2.0));
	float relief = tex.r * fernIn * 0.75;
	[branch] if (cover <= 0.001 && relief <= 0.001)
		return;

	float cold = saturate((amount - 0.35) / 0.65);
	float glaze = smoothstep(-0.06, 0.0, lead);
	float age = saturate(lead / 0.5);
	float profile = pow(saturate(1.0 - arrival / 0.72), 1.6);
	float grain = CameraFX::ValueNoise(q * 12.0);
	float thickness = profile * glaze * (0.12 + 0.88 * pow(age, 1.2)) * (0.25 + 0.75 * cold) * (0.9 + 0.2 * grain);
	float frosting = profile * glaze * (0.35 + 0.65 * age) * (0.45 + 0.55 * cold) * (0.75 + 0.5 * grain);
	float3 frostLight = (ambient * 0.75 + 0.18) * IceTint;
	float bubblesIn = glaze * saturate(age * 1.6) * saturate(0.35 + cold * 1.6) * saturate(profile * 2.5);

	float aspect = Aspect();
	const float stride = 0.022;
	float2 ex = float2(stride / aspect, 0.0);
	float2 ey = float2(0.0, stride);
	float bendR = IceBend(uv + ex, n, growth, cold, baked);
	float bendL = IceBend(uv - ex, n, growth, cold, baked);
	float bendD = IceBend(uv + ey, n, growth, cold, baked);
	float bendU = IceBend(uv - ey, n, growth, cold, baked);
	float2 offset = float2(bendR - bendL, bendD - bendU) / (2.0 * stride) * 0.0015;
	offset += (float2(CameraFX::Fbm(q * 4.2, 3), CameraFX::Fbm(q * 4.2 + 7.3, 3)) - 0.5) * 0.02 * thickness;
	float ripples = smoothstep(0.88, 0.99, amount);
	[branch] if (ripples > 0.001)
	{
		float2 warp = float2(CameraFX::Fbm(q * 2.1, 3), CameraFX::Fbm(q * 2.1 + 11.7, 3)) - 0.5;
		float2 rp = q * 7.4 + warp * 2.5;
		offset += (float2(CameraFX::Fbm(rp, 3), CameraFX::Fbm(rp + 5.3, 3)) - 0.5) * 0.006 * thickness * ripples;
	}

	float2 reliefGradient = 0.0;
	float2 snowGradient = 0.0;
	[branch] if (baked)
	{
		// A fixed step across the baked mask, the 1.5 screen pixels it was tuned with at 1080p. Stepping in screen
		// texels made the fern relief, bubbles and crust shading half as strong at 4K and 1.5x as strong at 720p.
		float2 texel = float2(1.5 / ReferenceHeight / Aspect(), 1.5 / ReferenceHeight);
		float4 sampleR = FrostTexture.SampleLevel(LinearSampler, uv + float2(texel.x, 0.0), 0);
		float4 sampleL = FrostTexture.SampleLevel(LinearSampler, uv - float2(texel.x, 0.0), 0);
		float4 sampleD = FrostTexture.SampleLevel(LinearSampler, uv + float2(0.0, texel.y), 0);
		float4 sampleU = FrostTexture.SampleLevel(LinearSampler, uv - float2(0.0, texel.y), 0);
		reliefGradient = float2(sampleR.r - sampleL.r, sampleD.r - sampleU.r) * fernIn * 0.6;
		snowGradient = float2(sampleR.a - sampleL.a, sampleD.a - sampleU.a);
		offset += reliefGradient * 0.003;
		offset += float2(sampleR.b - sampleL.b, sampleD.b - sampleU.b) * bubblesIn * 0.003;
	}
	float offsetLength = length(offset);
	offset *= offsetLength > 0.008 ? 0.008 / offsetLength : 1.0;
	offset = all(isfinite(offset)) ? offset : 0.0;

	float dEdge = max(0.0, (1.0 + 0.1 * saturate((9.0 - n) / 7.0) - CameraFX::SuperellipseRadius(uv, n)) * 0.5);
	float rimNoise = saturate((CameraFX::Fbm(q * 5.5, 4) - 0.25) * 1.6);
	float rimWidth = (0.03 + 0.07 * cold) * (0.55 + 0.9 * rimNoise) * saturate((amount - 0.2) / 0.12);
	float rim = pow(saturate(1.0 - dEdge / max(rimWidth, 1e-4)), 1.5) * smoothstep(0.2, 0.32, amount);
	rim = saturate(rim * (0.7 + 0.5 * smoothstep(0.5, 0.85, CameraFX::ValueNoise(q * 680.0))));

	float cloud = saturate(CameraFX::Fbm(q * 7.5, 4) * 2.6 - 1.5) * profile;
	float blur = frosting * 7.0 + cloud * bubblesIn * 4.0 + rim * 8.0 + (1.0 - exp(-relief * 1.8)) * 6.0;
	float3 ice = Scene(viewUV + ToUV(offset), MipForRadius(blur * 1.6));

	float settle = smoothstep(0.0, 0.3, lead);
	float blue = profile * settle * (0.45 + 0.55 * cold) * (0.5 + 0.5 * age);
	ice *= exp(-blue * float3(0.5, 0.17, 0.06));
	ice = lerp(ice, frostLight, frosting * 0.5);
	ice = lerp(ice, frostLight * 1.1, cloud * bubblesIn * cold * 0.3);

	[branch] if (relief > 0.002)
	{
		float opacity = 1.0 - exp(-relief * 2.2);
		cover = max(cover, opacity);
		float3 normal = normalize(float3(-reliefGradient * 9.0, 1.0));
		const float3 light = normalize(float3(-0.55, -0.62, 0.56));
		float shade = clamp(0.95 + 0.6 * (dot(normal, light) - light.z), 0.7, 1.5);
		float specular = pow(saturate(dot(normal, normalize(light + float3(0.0, 0.0, 1.0)))), 40.0) * saturate(relief * 2.0);
		float3 lit = max(ambient, 0.3) * 1.35 * IceTint;
		ice = lerp(ice, lit * shade, opacity) + specular * lit * 2.0;
		ice += Sparkle(q, 4.0, 0.05, 11.0) * LightLevel(ambient) * step(0.15, tex.r) * fernIn;
	}

	float rimOpacity = rim * (0.62 + 0.3 * cold);
	cover = max(cover, rimOpacity);
	ice = lerp(ice, frostLight * 1.25, rimOpacity);

	float bubble = (tex.b - 0.5) * 1.2;
	ice = ice * (1.0 - saturate(-bubble) * 0.7 * bubblesIn) + saturate(bubble) * 1.4 * bubblesIn * frostLight * 1.4;

	float snowIn = glaze * saturate((amount - 0.25) / 0.35);
	float crustZone = smoothstep(0.88, 0.92, uv.y);
	float snow = tex.a * lerp(snowIn * 0.5 * saturate(profile * 3.0), saturate((amount - 0.7) / 0.3) * 0.85, crustZone);
	[branch] if (snow > 0.002)
	{
		float shade = lerp(1.0, clamp(1.0 - (snowGradient.x * 0.6 + snowGradient.y * 0.8) * 8.0, 0.75, 1.25), crustZone);
		ice = lerp(ice, frostLight * 1.3 * shade * lerp(1.0, float3(0.97, 0.99, 1.03), crustZone), saturate(snow));
		cover = max(cover, saturate(snow));
	}
	[branch] if (snowIn > 0.5)
	{
		float hue = CameraFX::Hash12(floor(q * ReferenceHeight / 5.0) + 29.0);
		float3 fringe = hue < 0.05 ? float3(1.0, 0.5, 0.4) : (hue < 0.1 ? float3(0.45, 1.0, 0.6) : (hue < 0.15 ? float3(0.5, 0.65, 1.0) : float3(1.0, 1.0, 1.0)));
		ice += fringe * Sparkle(q, 5.0, 0.035, 23.0) * step(0.3, tex.a) * LightLevel(ambient) * 0.8;
	}

	cover = saturate(cover) * 0.97;
	Blend(c, ice, cover);
	frostCover = cover;
}

float SplatOutline(float2 p, float seed, out float core)
{
	float r = length(p);
	float boundary = 0.4 + (CameraFX::Fbm(p * 2.2 + seed * 13.0, 3) - 0.5) * 0.5;
	core = 1.0 - smoothstep(boundary - 0.07, boundary, r);
	float satellites = 0.0;
	[branch] if (r < 1.6)
	{
		float2 g = p * 3.4 + seed * 7.0;
		float2 cell = floor(g);
		[branch] if (CameraFX::Hash12(cell + 3.0) < 0.3)
		{
			float2 centre = 0.5 + (CameraFX::Hash22(cell + 5.0) - 0.5) * 0.5;
			float radius = 0.08 + 0.12 * CameraFX::Hash12(cell + 9.0);
			satellites = (1.0 - smoothstep(radius * 0.8, radius, length(frac(g) - centre))) * (1.0 - smoothstep(1.0, 1.6, r));
		}
	}
	return max(core, satellites);
}

float Drips(float2 s, float seed, float progress, float speed, uint maxDrips, out float drained)
{
	float height = 0.0;
	drained = 0.0;
	[loop] for (uint k = 0; k < maxDrips; k++)
	{
		float id = seed * 3.17 + k * 13.9;
		[branch] if (k > 0 && CameraFX::Hash12(float2(id, 1.0)) > 0.5)
			continue;
		float delay = 0.04 + 0.16 * CameraFX::Hash12(float2(id, 2.0)) + 0.1 * k;
		float t = saturate((progress - delay) / (1.0 - delay));
		[branch] if (t <= 0.0)
			continue;
		float hops = 3.0 + 2.0 * CameraFX::Hash12(float2(id, 3.0));
		float travel = t * hops;
		float run = (floor(travel) + smoothstep(0.4, 1.0, frac(travel))) / hops;
		float reach = run * speed * (0.7 + 0.5 * CameraFX::Hash12(float2(id, 4.0)));
		drained = max(drained, saturate(reach / speed));
		float x0 = (CameraFX::Hash12(float2(id, 5.0)) - 0.5) * 0.3;
		float y = s.y - (0.1 + 0.06 * CameraFX::Hash12(float2(id, 6.0)));
		[branch] if (y < -0.12 || y > reach + 0.12)
			continue;
		float wander = (CameraFX::ValueNoise(float2(y * 2.6, id)) - 0.5) * 0.12 + (CameraFX::ValueNoise(float2(y * 9.0, id + 3.0)) - 0.5) * 0.02;
		float dx = s.x - x0 - wander * smoothstep(0.0, 0.25, y);
		float width = lerp(0.028, 0.11, 1.0 - smoothstep(0.0, 0.22, y)) * (0.85 + 0.3 * CameraFX::ValueNoise(float2(y * 5.0, id + 7.0)));
		float profile = saturate(1.0 - dx * dx / (width * width));
		float beads = pow(saturate(1.0 - abs(frac(y * 16.0 + CameraFX::ValueNoise(float2(y * 3.0, id)) * 2.0) * 2.0 - 1.0) * 1.6), 2.0);
		float film = lerp(0.72, 0.58, smoothstep(0.0, 0.25, y)) * lerp(1.0, 0.65 + 0.35 * beads, smoothstep(0.06, 0.2, reach - y) * smoothstep(0.4, 0.95, t));
		float trail = sqrt(profile) * film * step(-0.02, y) * step(y, reach);
		float beadRadius = 0.032 + 0.008 * (1.0 - frac(travel));
		float2 bead = float2(dx / beadRadius, (y - reach) / (beadRadius * 1.25));
		height = max(height, max(trail, sqrt(saturate(1.0 - dot(bead, bead))) * 0.62));
	}
	return height;
}

float Spatter(float2 p, float seed)
{
	float height = 0.0;
	float r = length(p);
	bool inside = r < 2.2 && r > 0.18;
	[unroll] for (int layer = 0; layer < 3; layer++)
	{
		[branch] if (!inside)
			break;
		float scale = layer == 0 ? 4.0 : (layer == 1 ? 7.5 : 12.5);
		float2 shift = seed * float2(7.1 + layer * 3.0, 3.9 + layer * 5.0);
		float2 g = p * scale + shift;
		float2 cell = floor(g);
		float2 within = 0.5 + (CameraFX::Hash22(cell + layer * 11.0) - 0.5) * 0.25;
		float2 centre = (cell + within - shift) / scale;
		float reachOut = length(centre);
		float2 heading = centre / max(reachOut, 1e-3);
		float limit = 1.05 + 0.35 * layer;
		float chance = (layer == 2 ? 0.17 : 0.3) * smoothstep(0.0, 0.9, heading.x) * smoothstep(0.24, 0.42, reachOut) * (1.0 - smoothstep(limit * 0.5, limit, reachOut));
		[branch] if (CameraFX::Hash12(cell + 23.0 + layer) >= chance)
			continue;
		float radius = (0.07 + 0.06 * CameraFX::Hash12(cell + 31.0 + layer)) * (layer == 2 ? 0.8 : 1.0);
		float stretch = 1.0 + smoothstep(0.3, 1.5, reachOut) * (0.5 + 0.5 * CameraFX::Hash12(cell + 37.0));
		float2 d = frac(g) - within;
		float along = dot(d, heading);
		float across = dot(d, float2(-heading.y, heading.x));
		float drop = sqrt(saturate(1.0 - pow(along / (radius * stretch), 2.0) - pow(across / radius, 2.0)));
		[branch] if (layer < 2)
		{
			float tail = (along - radius * stretch * 0.6) / (radius * (0.5 + 0.5 * stretch));
			float tailWidth = radius * 0.4 * (1.0 - saturate(tail));
			drop = max(drop, sqrt(step(0.0, tail) * saturate(1.0 - tail) * saturate(1.0 - across * across / max(tailWidth * tailWidth, 1e-6))) * 0.7);
		}
		height = max(height, drop * (layer == 0 ? 0.8 : (layer == 1 ? 0.62 : 0.42)));
	}
	return height;
}

float BloodHeight(float2 p, float2 s, float seed, float progress, float dripSpeed)
{
	float r = length(p * float2(0.8, 1.0));
	float2 heading = p / max(length(p), 1e-4);
	float scallop = CameraFX::Fbm(p * 2.3 + seed * 11.0, 3);
	float lobes = (CameraFX::ValueNoise(heading * 3.2 + seed * 4.0) - 0.5) * 0.12;
	float spines = pow(saturate(CameraFX::ValueNoise(heading * 7.5 + seed * 2.0) * 1.6 - 0.6), 1.5) * 0.15 * saturate(heading.x);
	float boundary = 0.36 + (scallop - 0.5) * 0.28 + lobes + spines;
	float x = saturate((boundary - r) / 0.16);
	float drained;
	float drip = Drips(s, seed, progress, dripSpeed, 2, drained);
	float height = x * (2.0 - x) * (1.0 - 0.3 * drained);
	return max(max(height, Spatter(p, seed)), drip);
}

void ApplyLiquid(inout Composite c, float2 uv, float2 viewUV, float2 p, float2 s, float size, float angle, uint kind, float seed, float progress, float opacity, inout float below, inout float3 probe)
{
	float dripSpeed = kind == SplatOil ? 1.3 : 2.2;
	float smudge = CameraFX::Blurs.x;
	float reach = length(p * float2(0.8, 1.0));
	[branch] if (smudge > 0.001 && reach < 1.0 && c.alpha < 0.62)
	{
		float spread = reach * (0.85 + 0.3 * CameraFX::Fbm(p * 1.7 + seed * 5.0, 3));
		float film = (1.0 - smoothstep(0.25, 0.85, spread)) * opacity * saturate(smudge) * (1.0 - saturate(c.alpha * 1.6));
		[branch] if (film > 0.002)
		{
			float3 smeared = Scene(viewUV, MipForRadius(6.0 + 6.0 * smudge));
			smeared *= kind == SplatOil ? float3(0.9, 0.88, 0.85) : float3(1.0, 0.86, 0.84);
			Blend(c, smeared, film * 0.6);
		}
	}

	const float e = 0.008;
	float3 heights = 0.0;
	[loop] for (uint k = 0; k < 3; k++)
	{
		float2 nudge = k == 0 ? float2(0.0, 0.0) : (k == 1 ? float2(e, 0.0) : float2(0.0, e));
		float value = BloodHeight(p + CameraFX::Rotate(nudge, -angle), s + nudge, seed, progress, dripSpeed);
		heights = k == 0 ? float3(value, 0.0, 0.0) : (k == 1 ? float3(heights.x, value, 0.0) : float3(heights.xy, value));
		[branch] if (k == 0 && value <= 0.002)
			break;
	}
	float height = heights.x;
	[branch] if (height <= 0.002)
		return;
	float2 gradient = float2(heights.y - height, heights.z - height) / e;
	float slope = length(gradient);

	float3 tint = kind == SplatBlood ? float3(0.5, 0.025, 0.02) : (kind == SplatDarkBlood ? float3(0.3, 0.02, 0.03) : float3(0.05, 0.045, 0.04));
	float smear = MipForRadius(CameraFX::Blurs.x * (3.0 + 12.0 * height));
	float3 behind = lerp(Scene(viewUV + ToUV(-gradient * 0.0045 * size), smear), c.color, saturate(below * 1.6));
	float3 fluid = behind * exp(log(max(tint, 0.004)) * height * 1.1) + tint * (0.04 + 0.1 * height);
	fluid *= 1.0 - saturate(slope * 0.02 - 0.2) * 0.3;

	[branch] if (probe.z < 0.0)
	{
		float lightR = LightLevel(Scene(uv + float2(0.05, 0.0), 3.0));
		float lightL = LightLevel(Scene(uv - float2(0.05, 0.0), 3.0));
		float lightD = LightLevel(Scene(uv + float2(0.0, 0.05), 3.0));
		float lightU = LightLevel(Scene(uv - float2(0.0, 0.05), 3.0));
		float2 gradientToLight = float2(lightR - lightL, lightD - lightU);
		probe = float3(gradientToLight / max(length(gradientToLight), 1e-3), saturate(max(max(lightR, lightL), max(lightD, lightU)) * 1.2));
	}
	float2 toLight = probe.xy;
	float brightest = probe.z;
	float rimBand = saturate(slope * 0.025) * smoothstep(0.04, 0.2, height) * (1.0 - smoothstep(0.4, 0.85, height));
	float facing = saturate(dot(gradient / max(slope, 1e-3), toLight) * 0.7 + 0.3);
	fluid += lerp(tint * 2.4, 1.0, 0.15) * rimBand * facing * brightest * 0.16 * (1.0 - saturate(below * 2.0));

	float cover = smoothstep(0.0, 0.05, height) * opacity;
	Blend(c, fluid, cover);
	below = max(below, cover);
}

void ApplyVenom(inout Composite c, float2 viewUV, float2 p, float2 s, float size, float seed, float progress, float opacity)
{
	float core;
	float drained;
	float shape = max(SplatOutline(p * 0.9, seed, core), Drips(s, seed + 5.0, progress, 1.1, 2, drained));
	float mask = saturate(shape) * opacity;
	[branch] if (mask <= 0.001)
		return;
	const float3 goo = float3(0.3, 0.5, 0.1);
	float r = length(p);
	float2 bulge = p / max(r, 1e-3) * core * (1.0 - r) * 0.012 * size;
	float3 film = Scene(viewUV - ToUV(bulge), max(0.5, MipForRadius(CameraFX::Blurs.x * (3.0 + 6.0 * core)))) * goo * 1.3 + goo * 0.06;

	[unroll] for (int layer = 0; layer < 2; layer++)
	{
		float scale = layer == 0 ? 4.5 : 10.0;
		float2 g = p * scale + seed * (5.0 + layer * 3.0) + float2(0.0, -Time() * 0.01 * (layer + 1));
		float2 cell = floor(g);
		[branch] if (CameraFX::Hash12(cell + 101.0 + layer * 13.0) < (layer == 0 ? 0.16 : 0.2) * saturate(core * 1.3))
		{
			float2 centre = 0.5 + (CameraFX::Hash22(cell + 103.0 + layer) - 0.5) * 0.4;
			float radius = 0.16 + 0.16 * CameraFX::Hash12(cell + 107.0 + layer);
			float2 b = (frac(g) - centre) / radius;
			float d = length(b);
			[branch] if (d < 1.0)
			{
				float3 through = Scene(viewUV - ToUV(b * radius / scale * size * 0.6), 0.0);
				float3 bubble = lerp(film, through * float3(0.7, 1.0, 0.55) * 0.9 + film * 0.25, 0.55);
				float facing = dot(b / max(d, 1e-3), LensLight);
				bubble += smoothstep(0.72, 0.93, d) * (1.0 - smoothstep(0.93, 1.0, d)) * saturate(facing) * 0.25;
				bubble += (1.0 - smoothstep(0.0, 0.18, length(b - LensLight * 0.45))) * 0.3;
				film = lerp(film, bubble, layer == 0 ? 0.9 : 0.7);
			}
		}
	}
	Blend(c, film, mask * 0.75);
	c.color += goo * (1.0 - smoothstep(0.0, 0.25, length(p - float2(-0.15, -0.2)))) * core * 0.18 * mask;
}

void ApplyIceHit(inout Composite c, float2 viewUV, float2 p, float seed, float progress, float opacity)
{
	float r = length(p);
	[branch] if (r > 1.2)
		return;
	float angle = atan2(p.y, p.x);
	float spread = 1.0 - pow(1.0 - saturate(progress / 0.06), 3.0);
	float grow = smoothstep(0.01, 0.25, progress);
	float thaw = 1.0 - smoothstep(0.55, 1.0, progress);

	const float rays = 7.0;
	const float sector = 6.2832 / rays;
	float index = floor((angle + 3.14159) / sector);
	float crack = 0.0;
	float glint = 0.0;
	float nearest = 9.0;
	[unroll] for (int j = -1; j <= 1; j++)
	{
		float k = index + j;
		float wrapped = k - rays * floor(k / rays);
		float theta = -3.14159 + (k + 0.5 + (CameraFX::Hash12(float2(wrapped, seed * 17.0)) - 0.5) * 0.7) * sector;
		float reach = (0.55 + 0.6 * CameraFX::Hash12(float2(wrapped, seed * 29.0))) * spread;
		float jag = (CameraFX::ValueNoise(float2(r * 10.0, wrapped * 5.3 + seed)) - 0.5) * 0.06 + (CameraFX::ValueNoise(float2(r * 33.0, wrapped * 3.1 + seed)) - 0.5) * 0.015;
		float sn, cs;
		sincos(angle - theta, sn, cs);
		float signedDistance = r * sn - jag * saturate(r * 4.0);
		float on = step(0.0, cs) * (1.0 - smoothstep(reach - 0.05, reach, r));
		float width = 0.01 * (1.0 - 0.6 * saturate(r / max(reach, 1e-3))) + 0.0025;
		float d = abs(signedDistance);
		nearest = min(nearest, cs > 0.0 ? d : 9.0);
		crack = max(crack, (1.0 - smoothstep(width * 0.35, width, d)) * on);
		float2 normal = float2(-sin(theta), cos(theta)) * (signedDistance > 0.0 ? 1.0 : -1.0);
		glint = max(glint, exp(-pow((d - width * 1.3) / (width * 0.5), 2.0)) * pow(saturate(dot(normal, -LensLight) * 0.7 + 0.3), 2.0) * on);
	}
	float ringRadius = 0.24 + 0.06 * CameraFX::Hash12(float2(seed, 3.0)) + (CameraFX::ValueNoise(float2(angle * 5.0, seed * 7.0)) - 0.5) * 0.07;
	float ringOn = smoothstep(0.2, 0.35, spread) * step(CameraFX::Hash12(float2(index, seed * 5.0)), 0.75);
	crack = max(crack, (1.0 - smoothstep(0.002, 0.007, abs(r - ringRadius))) * ringOn);
	nearest = min(nearest, ringOn > 0.0 ? abs(r - ringRadius) : 9.0);

	float wedge = index + (r > ringRadius ? 31.0 : 0.0);
	float2 tilt = CameraFX::Hash22(float2(wedge, seed * 13.0)) * 2.0 - 1.0;
	float broken = (1.0 - smoothstep(0.5, 1.1, r)) * smoothstep(0.0, 0.3, spread);
	float3 through = Scene(viewUV + ToUV(tilt * 0.009 * broken), 0.0);
	float3 shard = through * (0.94 + 0.1 * CameraFX::Hash12(float2(wedge, seed))) * float3(0.93, 0.97, 1.0) + float3(0.02, 0.03, 0.05);
	float sheen = pow(saturate(dot(normalize(p + 1e-4), normalize(tilt + 1e-4)) * 0.5 + 0.5), 4.0);
	shard += IceTint * sheen * broken * 0.14 * (0.45 + 0.8 * LightLevel(through));
	Blend(c, shard, broken * opacity * thaw);

	float feather = pow(CameraFX::Ridge(float2(r * 26.0 + nearest * 42.0, nearest * 70.0 - r * 8.0) + seed * 5.0), 6.0);
	float nearCrack = exp(-nearest / (0.025 + 0.05 * grow));
	float nearCore = 1.0 - smoothstep(0.08, 0.35 + 0.1 * grow, r);
	float grain = CameraFX::Fbm(p * 7.0 + seed * 3.0, 3);
	float frost = saturate((max(nearCrack, nearCore * 0.8) * (0.3 + 0.7 * grain * grain) + feather * nearCrack * 0.9) * grow - 0.1) * thaw;
	float3 frostBehind = Scene(viewUV, 2.0);
	Blend(c, frostBehind * 0.6 + (0.24 + 0.3 * LightLevel(frostBehind)) * IceTint, frost * 0.75 * opacity);

	float crackFade = opacity * (1.0 - smoothstep(0.75, 1.0, progress));
	Blend(c, 0.0, crack * 0.45 * crackFade);
	c.color += IceTint * glint * 0.55 * crackFade;

	float melt = smoothstep(0.15, 0.75, progress);
	float lumpEdge = 0.13 * (1.0 - 0.45 * melt) * (0.4 + 1.2 * CameraFX::Fbm(float2(cos(angle), sin(angle)) * 1.8 + seed * 11.0, 3));
	float lump = (1.0 - smoothstep(lumpEdge - 0.03, lumpEdge, r)) * (0.75 + 0.25 * CameraFX::Fbm(p * 11.0 + seed, 3));
	float3 slush = Scene(viewUV, 2.5);
	float3 snow = float3(0.9, 0.93, 0.97) * (0.62 + 0.38 * CameraFX::Fbm(p * 22.0 + seed * 4.0, 3)) * (0.62 + 0.5 * LightLevel(slush));
	Blend(c, lerp(snow, slush * 0.85 + 0.1, melt), lump * (1.0 - 0.6 * melt) * opacity);
}

void ApplyFireHit(inout Composite c, float2 p, float seed, float progress, float opacity)
{
	float3 light = 0.0;
	[loop] for (int j = 0; j < 9; j++)
	{
		float heading = CameraFX::Hash12(float2(seed * 9.1, j)) * 6.2832;
		float reach = progress * (0.45 + 0.65 * CameraFX::Hash12(float2(seed * 3.7, j + 9.0)));
		float2 d = p - (float2(cos(heading), sin(heading)) * reach + float2(0.0, progress * progress * 0.3));
		light += lerp(EmberHot, EmberCool, progress) * exp(-dot(d, d) * 520.0);
	}
	light *= 1.0 - progress;
	light += float3(1.0, 0.5, 0.15) * exp(-dot(p, p) * 6.0) * (1.0 - smoothstep(0.0, 0.35, progress)) * 0.15;
	c.light += light * 1.2 * opacity;
	float soot = (1.0 - smoothstep(0.2, 0.55, length(p) + (CameraFX::Fbm(p * 3.0 + seed, 3) - 0.5) * 0.3)) * smoothstep(0.05, 0.25, progress) * 0.28;
	Blend(c, float3(0.02, 0.015, 0.012), soot * opacity);
}

void ApplyGrit(inout Composite c, float2 p, float seed, float opacity)
{
	float spread = 1.0 - smoothstep(0.45, 1.05, length(p));
	[branch] if (spread <= 0.0)
		return;
	float occlusion = 0.0;
	float rim = 0.0;
	[unroll] for (int layer = 0; layer < 3; layer++)
	{
		float scale = layer == 0 ? 34.0 : (layer == 1 ? 18.0 : 9.0);
		float keep = (layer == 0 ? 0.5 : (layer == 1 ? 0.22 : 0.06)) * (0.35 + 0.65 * spread);
		float2 g = p * scale + seed * (11.0 + 7.0 * layer);
		float2 cell = floor(g);
		float salt = 140.0 + layer * 16.0;
		[branch] if (CameraFX::Hash12(cell + salt) < keep)
		{
			float2 local = frac(g) - 0.5 - (CameraFX::Hash22(cell + salt + 1.0) - 0.5) * 0.45;
			float2 f = CameraFX::Rotate(local, CameraFX::Hash12(cell + salt + 3.0) * 6.2832);
			f.x *= 1.0 + 1.3 * CameraFX::Hash12(cell + salt + 4.0);
			float radius = (layer == 0 ? 0.24 : (layer == 1 ? 0.3 : 0.36)) * (0.6 + 0.8 * CameraFX::Hash12(cell + salt + 5.0));
			float d = lerp(length(f), max(abs(f.x), abs(f.y)) * 1.15, 0.55) + (CameraFX::ValueNoise(g * 5.0 + layer) - 0.5) * radius * 0.6;
			float body = 1.0 - smoothstep(radius * 0.55, radius, d);
			float core = 1.0 - smoothstep(radius * 0.2, radius * 0.65, d);
			occlusion = max(occlusion, lerp(body * 0.6, body, core) * (layer == 0 ? 0.75 : 0.92));
			rim = max(rim, saturate(body - core) * saturate(-local.y / max(radius, 1e-3) + 0.2));
		}
	}
	float mask = spread * opacity * 0.8;
	float lum = LightLevel(c.color);
	Blend(c, c.color * 0.42 + float3(0.066, 0.053, 0.037), occlusion * mask);
	c.color += rim * mask * 0.4 * (0.25 + lum);
}

void ApplySplats(inout Composite c, float2 uv, float2 q, float2 viewUV)
{
	float aspect = Aspect();
	uint count = min((uint)(CameraFX::Combat2.w + 0.5), CameraFX::MaxSplats);
	float below = 0.0;
	float3 probe = float3(0.0, 0.0, -1.0);
	[loop] for (uint i = 0; i < count; i++)
	{
		float4 placement = CameraFX::Splats[i * 2];
		float4 data = CameraFX::Splats[i * 2 + 1];
		float size = max(placement.z, 0.005);
		float opacity = saturate(placement.w);
		float2 s = (q - float2(placement.x * aspect, placement.y)) / size;
		[branch] if (opacity <= 0.001 || dot(s, s) > 9.0)
			continue;
		uint kind = (uint)(data.x + 0.5);
		float seed = data.y;
		float progress = saturate(data.z);
		float angle = data.w;
		float2 p = CameraFX::Rotate(s, -angle);
		[branch] if (kind <= SplatOil)
			ApplyLiquid(c, uv, viewUV, p, s, size, angle, kind, seed, progress, opacity, below, probe);
		else if (kind == SplatVenom)
			ApplyVenom(c, viewUV, p, s, size, seed, progress, opacity);
		else if (kind == SplatIce)
			ApplyIceHit(c, viewUV, p, seed, progress, opacity);
		else if (kind == SplatFire)
			ApplyFireHit(c, p, seed, progress, opacity);
		else if (kind == SplatGrit)
			ApplyGrit(c, p, seed, opacity);
	}
}

void ApplyBurn(inout Composite c, float2 q, float edgeR, float amount)
{
	float time = Time();
	float noise = CameraFX::Fbm(q * 3.0 + float2(0.0, -time * 0.3), 4);
	float fine = CameraFX::ValueNoise(q * 16.0 + float2(time * 0.5, -time * 1.8));
	float front = edgeR + (noise - 0.5) * 0.35 + (fine - 0.5) * 0.05;
	float start = 1.12 - amount * 0.38;
	[branch] if (front < start - 0.3)
		return;
	float charred = smoothstep(start + 0.02, start + 0.12, front);
	float burnLine = exp(-pow((front - start - 0.02) / 0.02, 2.0));
	float heat = exp(-pow((front - start) / 0.1, 2.0));
	// Photosensitive Safe (Combat3.w) slows the flame's flicker from about eight changes a second to two.
	float flickerRate = CameraFX::Combat3.w > 0.5 ? 2.0 : 8.0;
	float flicker = 0.7 + 0.3 * CameraFX::ValueNoise(float2(time * flickerRate, front * 30.0));
	Blend(c, float3(0.02, 0.012, 0.008), charred * 0.55);
	float3 flame = lerp(float3(1.0, 0.3, 0.04), float3(1.0, 0.62, 0.22), saturate(fine * 1.4 - 0.2));
	Blend(c, flame, burnLine * flicker * 0.6 * amount);
	c.light += flame * burnLine * flicker * 0.18 * amount;
	c.light += float3(1.0, 0.38, 0.08) * heat * 0.07 * amount;

	float2 g = q * 18.0 + float2(0.0, time * 1.6);
	float2 cell = floor(g);
	[branch] if (CameraFX::Hash12(cell + 61.0) < 0.12)
	{
		float2 local = frac(g) - 0.5 - (CameraFX::Hash22(cell + 63.0) - 0.5) * 0.5;
		float spark = exp(-dot(local * float2(1.0, 0.45), local * float2(1.0, 0.45)) * 220.0);
		float near = smoothstep(start - 0.25, start, front) * (1.0 - charred * 0.5);
		float life = sin(3.14159 * frac(time * 0.7 + CameraFX::Hash12(cell + 67.0)));
		c.light += EmberHot * spark * near * life * amount * 1.4;
	}
}

float Zigzag(float along, float seed, float jitter)
{
	float offset = (CameraFX::ValueNoise(float2(along * 5.0, seed * 3.1)) - 0.5) * 0.14 + (CameraFX::ValueNoise(float2(along * 13.0, seed * 4.3)) - 0.5) * 0.06 +
	               (CameraFX::ValueNoise(float2(along * 34.0, seed * 5.7)) - 0.5) * 0.025;
	float x = along * 90.0 + seed * 11.0;
	float h0 = CameraFX::Hash12(float2(floor(x), seed + jitter));
	float h1 = CameraFX::Hash12(float2(floor(x) + 1.0, seed + jitter));
	offset += (lerp(h0, h1, frac(x)) - 0.5) * 0.012;
	return offset * smoothstep(0.0, 0.08, along);
}

void ApplyDischarge(inout Composite c, float2 centred, float amount, float age, float burst, bool calm)
{
	float hitAge = floor(age * 20.0) / 20.0;
	float jitter = calm ? 0.0 : floor(Time() * 20.0) * 0.003;
	float core = 0.0;
	float sheath = 0.0;
	float glow = 0.0;
	float flash = 0.0;
	[loop] for (uint b = 0; b < 5; b++)
	{
		float seed = burst * 7.0 + b;
		float t = hitAge - CameraFX::Hash12(float2(burst, b + 0.5)) * 0.12;
		[branch] if (t < 0.0 || t > 0.9 || CameraFX::Hash12(float2(seed, 1.0)) > 0.6 + 0.35 * amount)
			continue;
		float originAngle = CameraFX::Hash12(float2(seed, 17.0)) * 6.2832;
		float2 origin = float2(cos(originAngle), sin(originAngle)) * (0.04 + 0.16 * sqrt(CameraFX::Hash12(float2(seed, 18.0))));
		float heading = originAngle + (CameraFX::Hash12(float2(seed, 3.0)) - 0.5) * 2.2;
		float2 dir = float2(cos(heading), sin(heading));
		float2 normal = float2(-dir.y, dir.x);
		float curve = (CameraFX::Hash12(float2(seed, 21.0)) - 0.5) * 0.8;
		float reach = 0.4 + 0.65 * CameraFX::Hash12(float2(seed, 7.0)) + amount * 0.1;
		float life = 0.6 + 0.8 * CameraFX::Hash12(float2(seed, 29.0));
		float leader = 0.05 + 0.06 * CameraFX::Hash12(float2(seed, 19.0));
		float since = t - leader;
		float stroke;
		[branch] if (calm)
			stroke = smoothstep(-0.05, 0.05, since) * exp(-max(since - 0.05, 0.0) / (0.12 * life)) * 0.85;
		else
		{
			float restrike = 0.08 + 0.06 * CameraFX::Hash12(float2(seed, 31.0));
			stroke = (since >= 0.0 ? exp(-since / (0.08 * life)) : 0.0) + (since >= restrike ? 0.7 * exp(-(since - restrike) / (0.06 * life)) : 0.0);
		}
		flash += calm ? 0.0 : stroke * 0.15;

		float2 local = centred - origin;
		float along = dot(local, dir);
		float across = dot(local, normal);
		[branch] if (along < -0.08 || along > reach + 0.08 || abs(across) > 0.35)
			continue;
		float after = since > 0.0 ? 0.4 * exp(-since / (0.3 * life)) : 0.0;
		float headAt = reach * (floor(saturate(t / leader) * 7.0) + 1.0) / 7.0;
		float amp = 0.8 + 0.6 * CameraFX::Hash12(float2(seed, 33.0));
		float d = abs(across - Zigzag(along, seed, jitter) * amp - curve * along * along);
		float u = saturate(along / reach);
		float weight = (0.7 + 0.3 * (1.0 - smoothstep(0.0, 0.4, u))) * smoothstep(0.0, 0.08 + 0.18 * CameraFX::Hash12(float2(seed, 35.0)), along) * (1.0 - smoothstep(0.55, 1.0, u));
		float gaps = smoothstep(0.25, 0.45, CameraFX::ValueNoise(float2(along * 26.0, seed * 1.3)));
		float lit = since < 0.0 ? step(along, headAt) * 0.4 : stroke + after;
		float width = 0.003 + 0.0045 * (1.0 - u);
		float k = lit * weight;
		core += exp(-d * d / (width * width * 0.4)) * k * gaps;
		sheath += exp(-d * d / (width * width * 3.0)) * k * lerp(0.5, 1.0, gaps);
		glow += (stroke + after * 0.6) * weight * exp(-d / 0.03);

		[loop] for (uint j = 0; j < 2; j++)
		{
			float branchSeed = seed * 3.7 + j * 11.0;
			float start = reach * (0.15 + 0.55 * CameraFX::Hash12(float2(branchSeed, 1.0)));
			float branchLength = reach * (0.12 + 0.3 * CameraFX::Hash12(float2(branchSeed, 2.0)));
			float2 from = origin + dir * start + normal * (Zigzag(start, seed, jitter) * amp + curve * start * start);
			float turn = (CameraFX::Hash12(float2(branchSeed, 3.0)) < 0.5 ? -1.0 : 1.0) * (0.35 + 0.7 * CameraFX::Hash12(float2(branchSeed, 4.0)));
			float2 branchDir = CameraFX::Rotate(dir, turn);
			float2 rel = centred - from;
			float branchAlong = dot(rel, branchDir);
			[branch] if (branchAlong < 0.0 || branchAlong > branchLength)
				continue;
			float bd = abs(dot(rel, float2(-branchDir.y, branchDir.x)) - Zigzag(branchAlong, branchSeed, jitter) * amp * 0.6);
			float taper = 1.0 - smoothstep(0.0, 1.0, branchAlong / branchLength);
			float bw = width * (0.45 + 0.35 * taper);
			float bk = (since < 0.0 ? step(start + branchAlong, headAt) * 0.4 : stroke * 0.35 + after * 0.5) * taper * weight;
			core += exp(-bd * bd / (bw * bw * 0.4)) * bk * 0.7;
			sheath += exp(-bd * bd / (bw * bw * 3.0)) * bk;
			glow += bk * 0.4 * exp(-bd / 0.02);
		}
	}

	float glowAmount = saturate(glow * 0.8) * amount;
	Blend(c, c.color * float3(0.5, 0.72, 1.15) + float3(0.03, 0.1, 0.38), glowAmount * 0.35);
	c.light += float3(0.12, 0.35, 1.0) * glowAmount * 0.3;
	Blend(c, ArcColor * 1.35, saturate(sheath * 0.8) * amount * 0.75);
	c.light += float3(0.95, 0.97, 1.0) * saturate(core) * 1.3 * amount;
	c.light += c.color * float3(0.4, 0.6, 1.0) * saturate(flash) * 0.35 * amount;
}

void ApplyLastingShock(inout Composite c, float2 uv, float2 q, float2 centred, float amount, bool calm)
{
	float time = Time();
	float aspect = Aspect();
	float tq = floor(time * 20.0) / 20.0;
	float edge = length(abs(uv - 0.5) * 2.0) * 0.8;
	[branch] if (edge <= 0.45)
		return;
	float core = 0.0;
	float sheath = 0.0;
	float glow = 0.0;
	[loop] for (uint e = 0; e < 8; e++)
	{
		float period = 0.6 + 0.8 * CameraFX::Hash12(float2(e, 0.4));
		float clock = tq / period + CameraFX::Hash12(float2(e, 0.1));
		float seed = floor(clock) * 5.0 + e + 500.0;
		float t = frac(clock) * period;
		float duration = 0.25 + 0.35 * CameraFX::Hash12(float2(seed, 1.0));
		[branch] if (t > duration)
			continue;
		float place = CameraFX::Hash12(float2(seed, 2.0)) * 4.0;
		float f = frac(place);
		float hx = aspect * 0.5 - 0.015;
		float hy = 0.485;
		float2 start;
		float2 tangent;
		float2 inward;
		[branch] if (place < 1.0)
		{
			start = float2(lerp(-hx, hx, f), -hy);
			tangent = float2(1.0, 0.0);
			inward = float2(0.0, 1.0);
		}
		else if (place < 2.0)
		{
			start = float2(hx, lerp(-hy, hy, f));
			tangent = float2(0.0, 1.0);
			inward = float2(-1.0, 0.0);
		}
		else if (place < 3.0)
		{
			start = float2(lerp(hx, -hx, f), hy);
			tangent = float2(-1.0, 0.0);
			inward = float2(0.0, -1.0);
		}
		else
		{
			start = float2(-hx, lerp(hy, -hy, f));
			tangent = float2(0.0, -1.0);
			inward = float2(1.0, 0.0);
		}
		bool flick = CameraFX::Hash12(float2(seed, 3.0)) < 0.18;
		float tilt = flick ? 0.9 + 0.4 * CameraFX::Hash12(float2(seed, 4.0)) : 0.1 + 0.45 * CameraFX::Hash12(float2(seed, 4.0));
		float turn = CameraFX::Hash12(float2(seed, 5.0)) < 0.5 ? -1.0 : 1.0;
		float2 dir = normalize(tangent * turn * cos(tilt) + inward * sin(tilt));
		float reach = flick ? 0.18 + 0.14 * CameraFX::Hash12(float2(seed, 6.0)) : 0.07 + 0.16 * CameraFX::Hash12(float2(seed, 6.0));
		float2 local = centred - start;
		float along = dot(local, dir);
		[branch] if (along < -0.02 || along > reach + 0.02)
			continue;
		float jitter = calm ? 0.0 : floor(tq * 20.0) * 0.003;
		float d = abs(dot(local, float2(-dir.y, dir.x)) - Zigzag(along, seed, jitter) * 0.35);
		float envelope = smoothstep(0.0, 0.03, t) * (1.0 - smoothstep(duration * 0.45, duration, t));
		float flicker = calm ? 1.0 : 0.55 + 0.45 * CameraFX::Hash12(float2(seed, floor(tq * 20.0)));
		float taper = 1.0 - smoothstep(reach * 0.55, reach, along);
		float width = 0.0016 + 0.0012 * taper;
		float k = envelope * flicker * taper * (flick ? 1.0 : 0.85);
		core += exp(-d * d / (width * width * 0.4)) * k;
		sheath += exp(-d * d / (width * width * 3.0)) * k;
		glow += k * exp(-d / 0.015);
	}
	float frameGlow = smoothstep(0.78, 1.0, edge) * (0.6 + 0.4 * CameraFX::ValueNoise(float2(q.x * 3.0 + q.y * 2.0, time * 1.5)));
	c.light += float3(0.1, 0.3, 1.0) * frameGlow * 0.05 * amount;
	float glowAmount = saturate(glow * 0.6) * amount;
	Blend(c, c.color * float3(0.6, 0.78, 1.12) + float3(0.02, 0.07, 0.25), glowAmount * 0.45);
	Blend(c, ArcColor * 1.3, saturate(sheath * 0.75) * amount * 0.75);
	c.light += float3(0.92, 0.96, 1.0) * saturate(core) * 1.1 * amount;
}

void ApplySlash(inout Composite c, float2 q, float2 viewUV, float strength, float angle, float2 hit)
{
	float age = (1.0 - strength) * 0.4;
	float envelope = exp(-max(age - 0.15, 0.0) * 8.0) * saturate(strength * 4.0);
	float2 dir = float2(cos(angle), sin(angle));
	float2 perp = float2(-dir.y, dir.x);
	float2 centre = lerp(hit, float2(0.5 * Aspect(), 0.5), 0.25);
	float2 d = q - centre;
	float along = dot(d, dir);
	float across = dot(d, perp) - along * along * 0.75;
	const float reach = 0.82;
	float head = lerp(-reach, reach, smoothstep(0.0, 1.0, saturate(age * 5.5)));
	float behind = head - along;
	float drawn = smoothstep(-0.03, 0.02, behind) * (1.0 - smoothstep(reach * 0.7, reach * 1.6, behind));
	float taper = 1.0 - smoothstep(reach * 0.55, reach, abs(along));
	float width = 0.05 * (0.3 + taper) * (0.75 + 0.5 * CameraFX::ValueNoise(float2(along * 9.0, angle * 13.0)));
	float band = exp(-(across * across) / (width * width)) * taper * drawn;
	[branch] if (band * envelope <= 0.002)
		return;
	float turbulence = (CameraFX::ValueNoise(float2(along * 14.0, across * 30.0 + age * 20.0)) - 0.5) * 0.03 * band;
	float3 smear = 0.0;
	[unroll] for (int s = 0; s < 8; s++)
	{
		float2 offset = dir * ((s / 7.0) - 0.5) * 0.19 * band + perp * turbulence;
		smear += Scene(viewUV + ToUV(offset), 0.0);
	}
	smear /= 8.0;
	Blend(c, smear * 1.1 + 0.02, band * envelope * 0.7);
	float lead = exp(-pow((across + width * 0.45) / (width * 0.35), 2.0)) * taper * drawn * smoothstep(0.0, 0.12, behind) * (1.0 - smoothstep(0.08, 0.3, behind));
	c.color += float3(0.95, 0.97, 1.0) * lead * envelope * 0.4 * (0.6 + 0.4 * LightLevel(smear));
	float back = exp(-pow((across - width * 0.35) / (width * 0.18), 2.0)) * taper * drawn * smoothstep(0.02, 0.15, behind);
	c.color += float3(0.9, 0.93, 1.0) * back * envelope * 0.18;
	float2 tip = float2(along - head, across + width * 0.45) / float2(0.05, 0.012);
	c.color += float3(1.0, 0.98, 0.95) * exp(-dot(tip, tip)) * envelope * saturate(1.0 - age * 6.0) * 0.4;
	float wake = exp(-pow((across - width * 0.7) / (width * 0.55), 2.0)) * taper * drawn;
	Blend(c, 0.0, wake * envelope * 0.1);
}

void ApplyFlash(inout Composite c, float2 q, float2 viewUV, float strength, float age, float2 hit, float warmth)
{
	float2 d = q - hit;
	float glow = exp(-dot(d, d) * 200.0) * exp(-age * 20.0) * strength;
	[branch] if (glow <= 0.002)
		return;
	float split = Scene(viewUV + ToUV(d * 0.06 * glow), 0.0).r;
	Blend(c, float3(split, c.color.g, c.color.b), saturate(glow));
	float3 tint = lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.75, 0.55), warmth);
	c.light += (c.color * 0.5 + 0.1) * tint * glow;
}

#if defined(COMBAT_PASS)
void ApplyOverlay(inout Composite c, float2 uv, float2 q, float edgeR, bool full, out Water water, out float frostCover)
{
	frostCover = 0.0;
	water = (Water)0;
	float aspect = Aspect();
	float2 centred = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	bool calm = CameraFX::Combat3.w > 0.5;

	[branch] if (CameraFX::Combat2.w > 0.5)
	{
		[branch] if (full)
			ApplySplats(c, uv, q, uv);
		else
		{
			uint count = min((uint)(CameraFX::Combat2.w + 0.5), CameraFX::MaxSplats);
			[loop] for (uint i = 0; i < count; i++)
			{
				float4 placement = CameraFX::Splats[i * 2];
				float4 data = CameraFX::Splats[i * 2 + 1];
				float2 s = (q - float2(placement.x * aspect, placement.y)) / max(placement.z, 0.005);
				[branch] if ((uint)(data.x + 0.5) == SplatFire && placement.w > 0.001 && dot(s, s) < 9.0)
					ApplyFireHit(c, CameraFX::Rotate(s, -data.w), data.y, saturate(data.z), saturate(placement.w));
			}
		}
	}

	[branch] if (CameraFX::Combat.y > 0.001 && CameraFX::Combat4.x < 1.0)
		ApplyDischarge(c, centred, saturate(CameraFX::Combat.y), CameraFX::Combat4.x, CameraFX::Combat4.y, calm);

	[branch] if (CameraFX::Combat4.z > 0.001)
		ApplyLastingShock(c, uv, q, centred, saturate(CameraFX::Combat4.z), calm);
}
#else
void ApplyOverlay(inout Composite c, float2 uv, float2 q, float edgeR, bool full, out Water water, out float frostCover)
{
	frostCover = 0.0;
	water = (Water)0;
	float2 viewUV = uv;
	float aspect = Aspect();

	[branch] if (full)
	{
		[branch] if (CameraFX::Weather.x > 0.001 || CameraFX::Weather.z > 0.001 || CameraFX::Weather3.x > 0.001 || CameraFX::LensLook.x > 0.001)
			viewUV = ApplyWater(c, uv, q, water);

		[branch] if (CameraFX::Weather2.z > 0.001)
			ApplyDust(c, q, edgeR, saturate(CameraFX::Weather2.z));

		[branch] if (CameraFX::Weather2.y > 0.001)
			ApplyAsh(c, uv, q, edgeR, CameraFX::Weather2.y);

		[branch] if (CameraFX::Weather2.x > 0.001)
			ApplyMist(c, q, viewUV, edgeR, saturate(CameraFX::Weather2.x));

		[branch] if (CameraFX::Weather3.y > 0.001 && CameraFX::Weather3.z < 2.8)
			ApplyBreath(c, uv, q, viewUV, CameraFX::Weather3.y, CameraFX::Weather3.z, CameraFX::Weather3.w, CameraFX::Combat4.w);

		[branch] if (CameraFX::Weather.w > 0.001)
			ApplyEntryFog(c, q, viewUV, edgeR, saturate(CameraFX::Weather.w));

		[branch] if (CameraFX::Lens.x > 0.001)
		{
			float3 ambient = clamp(Scene(uv, CameraFX::Blur.w - 1.0) * 0.6 + Scene(float2(0.5, 0.5), CameraFX::Blur.w) * 0.4, 0.02, 1.2);
			ApplyFrost(c, uv, q, viewUV, CameraFX::Lens.x, ambient, frostCover);
		}

		[branch] if (CameraFX::Combat2.x > 0.001)
			ApplySlash(c, q, viewUV, CameraFX::Combat2.x, CameraFX::Combat2.y, float2(CameraFX::Combat3.x * aspect, CameraFX::Combat3.y));

		[branch] if (CameraFX::Combat.w > 0.001)
		{
			float age = CameraFX::Weather3.z;
			float pulse = smoothstep(0.0, 0.25, age) * (1.0 - smoothstep(0.35, 1.0, age));
			Blend(c, 0.0, CameraFX::Combat.w * pulse * smoothstep(0.45, 1.1, CameraFX::EdgeDistance(uv)) * 0.18);
		}
	}
	else
	{
		[branch] if (CameraFX::Weather2.x > 0.001)
			c.light += MistHalo(uv, saturate(CameraFX::Weather2.x));
	}

	[branch] if (CameraFX::Combat.z > 0.001)
		ApplyBurn(c, q, edgeR, saturate(CameraFX::Combat.z));

	[branch] if (CameraFX::Combat.x > 0.001)
		ApplyFlash(c, q, viewUV, CameraFX::Combat.x, CameraFX::Combat3.z, float2(CameraFX::Combat3.x * aspect, CameraFX::Combat3.y), CameraFX::Combat2.z);
}
#endif
float3 SunLight(float2 uv, float2 q, float3 color, Water water)
{
	float strength = CameraFX::Lens.y * CameraFX::SunScreen.z * LightVisibility.Load(int3(0, 0, 0));
	float3 light = 0.0;
	[branch] if (strength > 0.001)
	{
		float2 toSun = float2((CameraFX::SunScreen.x - uv.x) * Aspect(), CameraFX::SunScreen.y - uv.y);
		float d = length(toSun);
		float near = exp(-d * d * 2.6) * 0.82 + exp(-d * 1.25) * 0.18;
		float3 sunColor = CameraFX::SunColor.rgb;
		light += sunColor * near * strength * 0.05;
		float dirtAmount = CameraFX::Lens.w;
		[branch] if (dirtAmount > 0.001)
		{
			float dirt = Dirt(q, 1.0, 0.0);
			float3 bloom = Scene(uv, 4.5);
			float3 source = bloom * smoothstep(0.6, 1.2, LightLevel(bloom)) * 0.6 + sunColor * near * 0.7;
			light += saturate(dirt * source * dirtAmount * strength * 0.85) * saturate(1.0 - color);
		}
		[branch] if (water.mask > 0.001)
		{
			float facing = saturate(dot(water.normal, toSun / max(d, 1e-4)));
			light += sunColor * (pow(facing, 3.0) * 1.2 + water.rim * 0.6) * water.mask * near * strength * 0.5;
		}
	}
	return light;
}

float3 MoonLight(float2 uv, float2 q, float4 moon, float visibility)
{
	float strength = moon.z * visibility;
	float3 light = 0.0;
	[branch] if (strength > 0.001)
	{
		float aspect = Aspect();
		float radius = max(moon.w, 0.008);
		float d = length(float2((moon.x - uv.x) * aspect, moon.y - uv.y));
		float aureole = exp(-d * d / (radius * radius * 7.0)) * 0.11 + exp(-d / (radius * 5.0)) * 0.022;
		float ring = exp(-pow((d - radius * 4.2) / (radius * 0.9), 2.0)) * 0.016;
		float ghostDistance = length(float2((1.0 - moon.x - uv.x) * aspect, 1.0 - moon.y - uv.y));
		float ghost = exp(-ghostDistance * ghostDistance / (radius * radius * 2.5)) * 0.014 + exp(-pow((ghostDistance - radius * 1.4) / (radius * 0.3), 2.0)) * 0.007;
		light = MoonTint * (aureole + ring + ghost);
		[branch] if (CameraFX::Lens.w > 0.001)
			light += MoonTint * Dirt(q, 1.0, 0.0) * exp(-d * d * 8.0) * CameraFX::Lens.w * 0.2;
		light *= strength;
	}
	return light;
}

float4 main(CameraFX::Interpolants input) : SV_Target
{
	float2 uv = input.TexCoord;
	float mode = CameraFX::Output.x;
	bool uiLayer = mode > 0.5 && mode < 2.5;
	bool overlay = mode < 2.5;
	bool lights = mode < 1.5 || mode > 2.5;
	float aspect = Aspect();
	float2 q = float2(uv.x * aspect, uv.y);
	float edgeR = length(uv - 0.5) / 0.7071;

	float3 base = Scene(uv, 0.0);
	Composite c;
	c.color = base;
	c.alpha = 0.0;
	c.light = 0.0;

	Water water;
	float frostCover;
	ApplyOverlay(c, uv, q, edgeR, overlay, water, frostCover);

#if defined(COMBAT_PASS)
	[branch] if (!lights)
		c.light = 0.0;
#else
	[branch] if (lights)
	{
		float3 extra = 0.0;
		extra += SunLight(uv, q, c.color, water);
		extra += MoonLight(uv, q, CameraFX::MoonScreen[0], LightVisibility.Load(int3(1, 0, 0)));
		extra += MoonLight(uv, q, CameraFX::MoonScreen[1], LightVisibility.Load(int3(2, 0, 0)));
		c.light += extra * (1.0 - frostCover * 0.7);
		[branch] if (CameraFX::Weather2.w > 0.001)
			c.light += Embers(q, saturate(CameraFX::Weather2.w));
	}
	else
	{
		c.light = 0.0;
	}
#endif

	// Camera FX's own fade, or the game's (LensLook.w) when the game applied the modifiers itself.
	float visible = 1.0 - saturate(max(CameraFX::FadeColor.a, CameraFX::LensLook.w));
	float alpha = overlay ? saturate(c.alpha) : 0.0;
	float3 premultiplied = overlay ? (c.color - base * (1.0 - alpha)) * visible : float3(0.0, 0.0, 0.0);
	alpha *= visible;
	float3 light = max(c.light, 0.0) * visible;

	float4 output;
	[branch] if (uiLayer)
	{
		float gain = max(CameraFX::Output.w, 1e-3);
		output = float4((premultiplied + light) / gain, alpha);
	}
	else if (CameraFX::Output.z > 0.5)
	{
		float3 sceneLinear = SceneTexture.SampleLevel(LinearSampler, uv, 0.0).rgb;
		float3 composite = premultiplied + base * (1.0 - alpha) + light;
		output = float4(CameraFX::FromWorking(composite) - sceneLinear * (1.0 - alpha), alpha);
	}
	else
	{
		output = float4(premultiplied + light, alpha);
	}
	return output;
}
