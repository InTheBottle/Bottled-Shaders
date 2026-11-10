#pragma once

#include "Buffer.h"
#include "Features/ProceduralLightning/BoltGenerator.h"

struct ProceduralLightning : Feature
{
	virtual inline std::string GetName() override { return "Procedural Lightning"; }
	virtual std::string GetDisplayName() override { return T("feature.procedural_lightning.name", "Procedural Lightning"); }
	virtual inline std::string GetShortName() override { return "ProceduralLightning"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.procedural_lightning.description", "Draws real branching lightning bolts during thunderstorms, timed to the game's own lightning flashes and thunder."),
			{ T("feature.procedural_lightning.key_feature_1", "A new bolt shape every strike, with forks that taper out"),
				T("feature.procedural_lightning.key_feature_2", "Leader, return stroke and re-strike flicker like real lightning"),
				T("feature.procedural_lightning.key_feature_3", "Hidden by mountains and buildings, softened by fog, lights up the clouds"),
				T("feature.procedural_lightning.key_feature_4", "Uses each weather's lightning color, and costs nothing between strikes") } };
	}

	static constexpr uint32_t kMaxStrikes = 4;
	static constexpr uint32_t kMaxStrokes = 7;

	struct Settings
	{
		bool Enabled = true;
		bool UseWeatherColor = true;
		std::array<float, 3> Color = { 0.62f, 0.76f, 1.0f };
		float Intensity = 12.0f;
		float Width = 1.0f;
		float CloudGlow = 1.0f;
		float FogAbsorption = 0.7f;
		float Duration = 1.0f;
		uint MaxBoltsPerFlash = 2;
		uint MaxRestrikes = 3;
		float MinDistance = 40000.0f;
		float MaxDistance = 160000.0f;
		float CloudHeight = 70000.0f;
		float ViewBias = 0.75f;
		float Alpha = 0.35f;
		float Beta = 0.45f;
		float Gamma = 0.55f;
		uint MaxDepth = 9;
		uint MaxBranchDepth = 4;
	};

	struct alignas(16) Segment
	{
		float3 Start;
		float StartEnergy;
		float3 End;
		float EndEnergy;
		float StartArrival;
		float EndArrival;
		float Width;
		uint32_t Packed;
	};
	static_assert(sizeof(Segment) == 48);

	struct alignas(16) StrikeData
	{
		float4 Color;
		float4 Glow;
		float Main;
		float Branch;
		float Front;
		float CloudGlow;
	};
	static_assert(sizeof(StrikeData) == 48);

	struct alignas(16) FrameData
	{
		StrikeData Strikes[kMaxStrikes];
		float4 FogParams;
		float2 RenderSize;
		float Intensity;
		float FogAbsorption;
		float MinSigma;
		float MaxSigma;
		uint32_t SegmentCount;
		float WidthToGlow;
	};
	static_assert(sizeof(FrameData) == 240);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void Render(RE::BSShaderAccumulator* a_accumulator);

private:
	struct Stroke
	{
		float Time = 0.0f;
		float Amplitude = 0.0f;
	};

	struct Strike
	{
		bool Active = false;
		bool Preview = false;
		float Age = 0.0f;
		float Lifetime = 0.0f;
		uint32_t StrokeCount = 0;
		std::array<Stroke, kMaxStrokes> Strokes{};
		float4 Color;
		float4 Glow;
	};

	struct DebugInfo
	{
		uint64_t strikes = 0;
		uint64_t renderedFrames = 0;
		uint32_t lastSegments = 0;
	};

	void PollEngineFlash(const RE::Sky* a_sky);
	void SpawnBolts(const RE::Sky* a_sky, bool a_preview);
	bool SpawnStrike(const RE::Sky* a_sky, bool a_preview);
	void UpdateStrikes(float a_deltaTime);
	void ClearStrikes();
	void ClearTemporalMask();
	void Draw();

	float4 GetBoltColor(const RE::Sky* a_sky) const;
	float GetStrikeFloor(float a_cameraZ) const;
	float GetViewYaw();
	float Uniform(float a_min, float a_max);
	int GetActiveShapePreset() const;
	void ApplyShapePreset(int a_preset);

	ID3D11VertexShader* GetBoltVS();
	ID3D11PixelShader* GetBoltPS();

	std::array<Strike, kMaxStrikes> strikes{};
	std::array<StrikeData, kMaxStrikes> frameStrikes{};
	std::vector<Segment> segments;
	std::vector<Lightning::BoltSegment> generated;
	std::mt19937 rng{ std::random_device{}() };

	bool segmentsDirty = false;
	bool temporalMaskDirty = false;
	bool testStrikeQueued = false;
	bool shaderCompileFailed = false;
	uint32_t lastFlashStartTime = 0;
	uint32_t lastRenderedFrame = UINT32_MAX;

	eastl::unique_ptr<Buffer> segmentBuffer;
	eastl::unique_ptr<ConstantBuffer> frameConstants;

	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11VertexShader> boltVS;
	winrt::com_ptr<ID3D11PixelShader> boltPS;

	DebugInfo debug;
};
