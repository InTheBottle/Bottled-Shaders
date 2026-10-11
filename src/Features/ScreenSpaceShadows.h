#pragma once

#include "Buffer.h"

#include <mutex>
#include <string>
#include <unordered_map>

struct ScreenSpaceShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_shadows.name", "Screen Space Shadows"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kShadows; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.screen_space_shadows.description", "Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\nThis technique adds fine-detail shadows that traditional shadow mapping might miss."),
			{ T("feature.screen_space_shadows.key_feature_1", "Enhanced contact shadows"),
				T("feature.screen_space_shadows.key_feature_2", "Improved shadow detail"),
				T("feature.screen_space_shadows.key_feature_3", "Better shadow accuracy"),
				T("feature.screen_space_shadows.key_feature_4", "Fine-scale shadow effects"),
				T("feature.screen_space_shadows.key_feature_5", "Configurable shadow contrast") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = 0.02f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 1.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		uint AdaptiveReach = 1;
		uint pad0[2];
	};

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;

	struct RaymarchVariant
	{
		winrt::com_ptr<ID3D11ComputeShader> shader;
		bool pending = false;
		bool failed = false;
	};
	std::mutex raymarchMutex;
	std::unordered_map<uint, RaymarchVariant> raymarchVariants;
	std::unordered_map<uint, std::string> raymarchSampleCountTexts;
	uint raymarchGeneration = 0;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	/** @brief Creates the raymarch constant buffer, point border sampler, and shadow output texture. */
	virtual void SetupResources() override;

	/** @brief Draws the ImGui settings UI for screen-space shadow configuration. */
	virtual void DrawSettings() override;

	/** @brief Releases every compiled raymarch variant for recompilation. */
	virtual void ClearShaderCache() override;
	/** @brief Releases every compiled raymarch variant so they are recompiled on next use. */
	void InvalidateRaymarchShaders();
	/**
	 * @brief Queues a background compile for a sample count that has no variant yet. Takes raymarchMutex itself
	 * and releases it before enqueueing, so the compile callback can never deadlock against the caller.
	 */
	void RequestRaymarch(uint a_sampleCount);
	/** @brief True while a background compile for this sample count is still outstanding. */
	bool IsRaymarchPending(uint a_sampleCount);
	/** @brief Queues background compiles for every reach band of the current base sample count. */
	void PrefetchRaymarchVariants();
	/**
	 * @brief Calculates the resolution-scaled and quantized sample count for the raymarch shader.
	 * @param a_reachScale Multiplier on the ray length, 1 for the fixed-length behaviour.
	 */
	uint GetScaledSampleCount(float a_reachScale);
	/**
	 * @brief Ray-length multiplier that keeps the world-space reach stable as the sun's angle to the view changes.
	 * @param a_lightDirection Normalised world-space direction the directional light travels.
	 * @return 1 when adaptive reach is off, otherwise a multiplier in [0.5, 2].
	 */
	float GetAdaptiveReachScale(const float3& a_lightDirection);
	/**
	 * @brief Picks the sample count for this frame. The reach scale is held in square-root-of-two bands with
	 * hysteresis so a slowly moving sun does not swap shaders, and small resolution changes are ignored.
	 */
	uint SelectSampleCount(float a_reachScale);
	uint lastRequestedSampleCount = 0;
	uint lastCompiledSampleCount = 0;
	float lastReachScale = 1.0f;
	int reachBand = 0;
	/**
	 * @brief Returns the ready raymarch variant for a sample count, or the nearest ready variant while the
	 * requested one compiles in the background. Never compiles on the calling thread.
	 * @param a_usedSampleCount Receives the sample count of the returned variant.
	 * @return The compute shader, or nullptr when no variant has finished compiling yet.
	 */
	ID3D11ComputeShader* GetComputeRaymarch(uint a_sampleCount, uint& a_usedSampleCount);

	/** @brief Clears the shadow texture and dispatches shadow ray marching if conditions are met. */
	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Dispatches the Bend SSS compute shader to generate screen-space contact shadows. */
	void DrawShadows();

	virtual void RestoreDefaultSettings() override;

};
