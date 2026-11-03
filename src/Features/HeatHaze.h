#pragma once

#include "Buffer.h"

struct HeatHaze : Feature
{
	virtual inline std::string GetName() override { return "Heat Haze"; }
	virtual std::string GetDisplayName() override { return T("feature.heat_haze.name", "Heat Haze"); }
	virtual inline std::string GetShortName() override { return "HeatHaze"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.heat_haze.description", "Makes the air shimmer above campfires, hearths, braziers and forges."),
			{ T("feature.heat_haze.key_feature_1", "Finds fires in the world automatically, no setup or patches needed"),
				T("feature.heat_haze.key_feature_2", "Bends only what is behind the hot air, so nearer objects stay sharp"),
				T("feature.heat_haze.key_feature_3", "No GPU cost while no fire is on screen") } };
	}

	static constexpr uint32_t kMaxPlumes = 12;

	struct Settings
	{
		bool Enabled = true;
		float Strength = 1.0f;
		float MaxDistance = 2500.0f;
	};

	struct FrameData
	{
		float4 Plumes[kMaxPlumes];
		float4 PlumeParams[kMaxPlumes];
		float2 UVMax;
		float Time;
		float Strength;
	};
	static_assert(sizeof(FrameData) % 16 == 0);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void Render(RE::BSShaderAccumulator* a_accumulator);

private:
	struct Fire
	{
		RE::NiPoint3 center;
		float radius = 0.0f;
		float seed = 0.0f;
	};

	void ScanFires();
	void SelectPlumes();
	void Draw();
	bool EnsureSceneCopy(const D3D11_TEXTURE2D_DESC& a_desc);

	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetPS();

	std::vector<Fire> fires;
	std::vector<RE::NiBound> fireSamples;
	std::vector<std::pair<float, uint32_t>> fireOrder;
	std::unordered_map<RE::FormID, bool> fireBaseCache;

	FrameData frameData{};
	uint32_t plumeCount = 0;
	float time = 0.0f;
	float scanTimer = 0.0f;
	uint32_t lastRenderedFrame = UINT32_MAX;

	eastl::unique_ptr<Texture2D> sceneCopy;
	eastl::unique_ptr<ConstantBuffer> frameConstants;

	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	bool shaderCompileFailed = false;
};
