#pragma once

struct ProceduralNightSky : Feature
{
	struct Settings
	{
		bool enabled = true;
		float starIntensity = 1.0f;
		float starTwinkle = 0.25f;
		float shootingStarRate = 2.0f;
		float shootingStarIntensity = 1.0f;
	};

	struct alignas(16) PerFrameData
	{
		uint enabled;
		float starIntensity;
		float starTwinkle;
		float radianceLimit;

		float3 shootingStarHead;
		float shootingStarIntensity;

		float3 shootingStarTail;
		float shootingStarWidth;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 48);

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
			T("feature.procedural_night_sky.description", "Replaces the vanilla stars with procedural stars."),
			{ T("feature.procedural_night_sky.key_feature_1", "Stars with natural brightness and color that stay sharp at any resolution"),
				T("feature.procedural_night_sky.key_feature_3", "Follows vanilla star fading, moons, clouds and reflections"),
				T("feature.procedural_night_sky.key_feature_4", "Occasional shooting stars across clear night skies") }
		};
	}

	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	PerFrameData GetCommonBufferData() const;

private:
	struct ShootingStar
	{
		float3 start;
		float3 direction;
		float speed = 0.0f;
		float life = 0.0f;
		float age = 0.0f;
		float trail = 0.0f;
		float width = 0.0f;
		float brightness = 0.0f;
	};

	void UpdateShootingStar();
	void SpawnShootingStar();
	float NextShootingStarRandom();

	ShootingStar shootingStar;
	float shootingStarTimer = 20.0f;
	uint32_t shootingStarSeed = 0x6D2B79F5u;
};
