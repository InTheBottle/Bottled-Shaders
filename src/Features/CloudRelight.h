#pragma once

struct CloudRelight : Feature
{
	struct alignas(16) Settings
	{
		uint enabled = true;
		float cloudRelightMix = 1.0f;
		float cloudOriginalMix = 0.68f;
		float silverLiningMix = 1.0f;

		float silverLiningSpread = 0.0f;
		float pad[3] = {};
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);
	static_assert(sizeof(Settings) == 32);

	Settings settings;

	virtual inline std::string GetName() override { return "Cloud Relight"; }
	virtual std::string GetDisplayName() override { return T("feature.cloud_relight.name", "Cloud Relight"); }
	virtual inline std::string GetShortName() override { return "CloudRelight"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }
	virtual inline std::string_view GetShaderDefineName() override { return "CLOUD_RELIGHT"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Sky; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.cloud_relight.description", "Relights vanilla cloud textures using the active directional light, cloud self-shadowing, and silver-lining phase lighting."),
			{ T("feature.cloud_relight.key_feature_1", "Vanilla cloud color and relit cloud color blending"),
				T("feature.cloud_relight.key_feature_2", "Silver-lining and forward-scattering phase lighting"),
				T("feature.cloud_relight.key_feature_3", "Directional cloud self-shadowing from the active sun or moon") }
		};
	}

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	bool IsSuppressedByEffects11() const;
	Settings GetCommonBufferData() const;
};
