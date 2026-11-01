Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

static const float TargetSize = 1024.0;
static const float SourcePixelsPerTap = 1280.0;

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 SampleSource(float2 uv)
{
	float4 color = SourceTexture.SampleLevel(LinearSampler, uv, 0);
	return isfinite(color) ? clamp(color, 0.0, 64512.0) : 0.0;
}

float4 main(PS_INPUT input) : SV_Target
{
	uint width, height;
	SourceTexture.GetDimensions(width, height);
	uint2 taps = max(2, uint2(ceil(float2(width, height) / SourcePixelsPerTap)));

	float4 color = 0.0;
	[loop] for (uint y = 0; y < taps.y; y++)
	{
		[loop] for (uint x = 0; x < taps.x; x++)
		{
			color += SampleSource(input.txcoord0.xy + ((float2(x, y) + 0.5) / taps - 0.5) / TargetSize);
		}
	}
	return color / (taps.x * taps.y);
}
