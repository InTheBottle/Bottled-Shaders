#pragma once

struct ProceduralNightSky : Feature
{
	struct Settings
	{
		bool enabled = true;
		float starIntensity = 1.0f;
		float starTwinkle = 0.25f;
		float auroraIntensity = 1.0f;
		float auroraSpeed = 1.0f;
		float auroraLighting = 1.0f;
	};

	struct alignas(16) PerFrameData
	{
		uint enabled;
		float starIntensity;
		float starTwinkle;
		float radianceLimit;

		float3 auroraLowColor;
		float auroraIntensity;

		float3 auroraHighColor;
		float auroraSpeed;

		float auroraLighting;
		float3 pad = { 0.0f, 0.0f, 0.0f };
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 64);

	Settings settings;

	virtual inline std::string GetName() override { return "Procedural Night Sky"; }
	virtual std::string GetDisplayName() override { return T("feature.procedural_night_sky.name", "Procedural Night Sky"); }
	virtual inline std::string GetShortName() override { return "ProceduralNightSky"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }
	virtual inline std::string_view GetShaderDefineName() override { return "PROCEDURAL_NIGHT_SKY"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Sky; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.procedural_night_sky.description", "Replaces the vanilla stars and aurora with procedural stars and animated aurora curtains."),
			{ T("feature.procedural_night_sky.key_feature_1", "Stars with natural brightness and color that stay sharp at any resolution"),
				T("feature.procedural_night_sky.key_feature_2", "Animated aurora curtains across the whole sky, following each weather's aurora colors and timing"),
				T("feature.procedural_night_sky.key_feature_3", "Follows vanilla star fading, moons, clouds and reflections") }
		};
	}

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	PerFrameData GetCommonBufferData() const;

	/** @brief Draws the aurora from the transparency pass, the slot the vanilla mesh uses, so clouds never cover it. */
	void Render(RE::BSShaderAccumulator* a_accumulator);

	/** @brief Returns true while the aurora should add to the sky IBL. */
	bool HasAuroraLight() const;

private:
	static float GetAuroraVisibility(const RE::Sky* a_sky);

	ID3D11VertexShader* GetAuroraVS();
	ID3D11PixelShader* GetAuroraPS();
	void DrawAurora();

	bool shaderCompileFailed = false;
	uint32_t lastRenderedFrame = UINT32_MAX;

	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11VertexShader> auroraVS;
	winrt::com_ptr<ID3D11PixelShader> auroraPS;
};
