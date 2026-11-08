#include "CameraFX/Common.hlsli"

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
SamplerState LinearSampler : register(s0);

static const int RadialSteps = 4;
static const int HitSteps = 8;

float3 SampleScene(float2 uv, float mip)
{
	return CameraFX::ToWorking(SceneTexture.SampleLevel(LinearSampler, saturate(uv), mip).rgb);
}

float GetViewDepth(float2 uv)
{
	float depth = DepthTexture.Load(int3(uv * CameraFX::DepthParams.xy, 0));
	float4 camera = CameraFX::CameraData;
	return CameraFX::DepthParams.z > 0.5 ? camera.w / (camera.y + depth * camera.z) : camera.w / (camera.x - depth * camera.z);
}

float GetCircleParam(float distance, float start, float ramp)
{
	float circle = max(0.0, distance - start);
	return circle > 0.0 ? CameraFX::Radial.x * (1.0 - 1.0 / (ramp * circle + 1.0)) : 0.0;
}

float3 SampleRadial(float2 uv, float mip)
{
	float3 color = SampleScene(uv, mip);
	[branch] if (CameraFX::Radial.x > 0.0)
	{
		float2 offset = CameraFX::Radial2.yz - uv;
		float distance = length(offset);
		float amount = max(0.0, GetCircleParam(distance, CameraFX::Radial.y, CameraFX::Radial.z) - GetCircleParam(distance, CameraFX::Radial.w, CameraFX::Radial2.x));
		float2 delta = (amount / RadialSteps) * (offset / max(distance, 1e-4));
		[unroll] for (int i = 1; i <= RadialSteps; i++)
			color += SampleScene(uv + delta * i, mip) + SampleScene(uv - delta * i, mip);
		color /= 2.0 * RadialSteps + 1.0;
	}
	return color;
}

float GetBlurMip(float2 uv)
{
	float mip = CameraFX::Blur.x > 0.0 ? log2(1.0 + CameraFX::Blur.x) : 0.0;

	[branch] if (CameraFX::Blur.z > 0.0 && CameraFX::DepthParams.w > 0.5)
	{
		float viewDepth = GetViewDepth(uv);
		float range = max(CameraFX::DepthOfField.y, 1.0);
		float back = saturate((viewDepth - CameraFX::DepthOfField.x) / range);
		float front = saturate((CameraFX::DepthOfField.x - viewDepth) / range);
		float mode = CameraFX::DepthOfField.z;
		float coc = mode > 1.5 ? back : (mode > 0.5 ? front : max(front, back));
		mip = max(mip, coc * saturate(CameraFX::Blur.z) * CameraFX::Blur.w);
	}

	float exhaustion = saturate(CameraFX::Themes.y);
	mip = max(mip, exhaustion * (0.6 + 2.4 * CameraFX::ThemeParams.y));

	float edge = CameraFX::EdgeDistance(uv);
	float heartbeat = saturate(CameraFX::Themes.x) * CameraFX::Blurs.y;
	[branch] if (heartbeat > 0.0)
		mip = max(mip, smoothstep(0.5, 1.1, edge) * heartbeat * (0.3 + 0.9 * CameraFX::ThemeParams.x));

	float beast = saturate(CameraFX::Transform.x);
	[branch] if (beast > 0.0)
		mip = max(mip, smoothstep(0.2, 1.0, edge) * beast * (1.2 + 2.0 * CameraFX::Transform.z));

	float hit = saturate(CameraFX::HitBlur.x);
	[branch] if (hit > 0.0)
		mip = max(mip, hit * (0.4 + 1.6 * smoothstep(0.15, 0.9, edge)));
	return min(mip, CameraFX::Blur.w);
}

float3 ApplyHitSmear(float3 color, float2 uv, float mip)
{
	float amount = saturate(CameraFX::HitBlur.x);
	float2 away = uv - CameraFX::HitBlur.yz;
	float edge = smoothstep(0.1, 0.9, CameraFX::EdgeDistance(uv));
	float2 delta = away * (amount * (0.35 + 0.65 * edge) * 0.045 / HitSteps);
	float spacing = length(delta * CameraFX::Resolution.xy);
	float smearMip = min(max(mip, log2(1.0 + spacing * 0.6)), CameraFX::Blur.w);
	float2 doubled = CameraFX::Blur.y > 0.0 ? float2(CameraFX::Blur.y, CameraFX::Blur.y * 0.5) : 0.0;
	float3 sum = color;
	float total = 1.0;
	[unroll] for (int i = 1; i <= HitSteps; i++)
	{
		float weight = 1.0 - i / (HitSteps + 1.0);
		sum += SampleScene(uv - delta * i + doubled * ((i & 1) ? 1.0 : -1.0), smearMip) * weight;
		total += weight;
	}
	return sum / total;
}

float3 ApplyThemes(float3 color, float2 uv)
{
	float edge = CameraFX::EdgeDistance(uv);

	float lowHealth = saturate(CameraFX::Themes.x);
	[branch] if (lowHealth > 0.0)
	{
		float pulse = CameraFX::ThemeParams.x;
		float strength = smoothstep(0.45, 1.15, edge) * lowHealth * (0.75 + 0.25 * pulse);
		color = lerp(color, CameraFX::Luminance(color).xxx, 0.15 * lowHealth);
		color = lerp(color, color * float3(1.0, 0.4, 0.35), strength * 0.55);
		color *= 1.0 - strength * 0.3;
	}

	float exhaustion = saturate(CameraFX::Themes.y);
	[branch] if (exhaustion > 0.0)
	{
		float lid = CameraFX::ThemeParams.y;
		float curve = (uv.x - 0.5) * (uv.x - 0.5) * 0.35;
		float fromEdge = min(uv.y, 1.0 - uv.y) - curve * lid;
		float open = smoothstep(lid * 0.55 - 0.04, lid * 0.55 + 0.06, fromEdge);
		open = lerp(1.0, open, saturate(lid * 10.0));
		float vignette = 1.0 - smoothstep(0.45, 1.05, edge) * 0.55 * exhaustion;
		color *= open * vignette;
	}

	float hunger = saturate(CameraFX::Themes.z);
	[branch] if (hunger > 0.0)
	{
		float amount = hunger * (0.3 + 0.45 * CameraFX::ThemeParams.z);
		color = lerp(color, CameraFX::Luminance(color).xxx, amount);
		color *= 1.0 - smoothstep(0.5, 1.05, edge) * 0.3 * hunger;
	}

	float poison = saturate(CameraFX::Themes.w);
	[branch] if (poison > 0.0)
	{
		float ring = smoothstep(0.3, 1.0, edge);
		color = lerp(color, color * float3(0.7, 1.0, 0.55) + float3(0.02, 0.05, 0.0), ring * poison * 0.85);
	}

	float beast = saturate(CameraFX::Transform.x);
	[branch] if (beast > 0.0)
	{
		float pulse = CameraFX::Transform.z;
		bool vampire = CameraFX::Transform.y > 0.5;
		float ring = smoothstep(0.25, 1.0, edge);
		float3 haze = vampire ? float3(0.42, 0.12, 0.3) : float3(0.62, 0.16, 0.05);
		float luma = CameraFX::Luminance(color);
		color = lerp(color, luma * haze * 2.2 + haze * 0.04, ring * beast * (0.55 + 0.3 * pulse));
		color *= 1.0 - ring * beast * (0.3 + 0.25 * pulse);
		color = lerp(color, luma.xxx, beast * (vampire ? 0.35 : 0.15) * (1.0 - ring));
	}

	return color;
}

float3 ApplyGrade(float3 color)
{
	float luma = CameraFX::Luminance(color);
	color = lerp(luma.xxx, color, CameraFX::Grade.x);
	color = lerp(color, luma * CameraFX::TintColor.rgb, CameraFX::TintColor.a);
	color *= CameraFX::Grade.y;
	color = lerp(CameraFX::Output.y.xxx, color, CameraFX::Grade.z);
	return lerp(color, CameraFX::FadeColor.rgb, CameraFX::FadeColor.a);
}

float4 main(CameraFX::Interpolants input) : SV_Target
{
	float2 uv = input.TexCoord;
	float alpha = SceneTexture.SampleLevel(LinearSampler, uv, 0.0).a;

	float2 sampleUV = uv;
	float poison = saturate(CameraFX::Themes.w);
	[branch] if (poison > 0.0)
	{
		float time = CameraFX::ThemeParams.w;
		sampleUV += float2(sin(time * 1.3 + uv.y * 5.0), cos(time * 1.1 + uv.x * 4.0)) * 0.0035 * poison;
	}

	float blurMip = GetBlurMip(uv);
	float3 color;
	[branch] if (CameraFX::Blur.y > 0.0)
	{
		float2 offset = float2(CameraFX::Blur.y, CameraFX::Blur.y * 0.5);
		color = 0.5 * (SampleRadial(sampleUV - offset, blurMip) + SampleRadial(sampleUV + offset, blurMip));
	}
	else
	{
		color = SampleRadial(sampleUV, blurMip);
	}

	[branch] if (CameraFX::HitBlur.x > 0.0)
		color = ApplyHitSmear(color, sampleUV, blurMip);

	color = ApplyThemes(color, uv);
	color = ApplyGrade(color);
	return float4(CameraFX::FromWorking(color), alpha);
}
