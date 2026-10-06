#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

struct SceneCulling : Feature
{
	virtual inline std::string GetName() override { return "Scene Culling"; }
	virtual std::string GetDisplayName() override { return T("feature.scene_culling.name", "Scene Culling"); }
	virtual inline std::string GetShortName() override { return "SceneCulling"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kShadows; }
	virtual inline std::string GetFeatureModLink() override { return "https://github.com/dnai92/skyrimPerformanceSolution"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.scene_culling.description", "Drops draw calls and engine work that cannot be seen: small distant objects in the sun and torch shadow maps, far characters' shadows, whole scene-graph branches below the culling thresholds, footprint-sized decals far from the camera, and redundant per-frame light assignment for lights that have not moved."),
			{ T("feature.scene_culling.key_feature_1", "Sun shadow culling of small distant objects, scaled by shadow length so low sun keeps its long shadows"),
				T("feature.scene_culling.key_feature_2", "Torch and point light shadow culling, with objects next to the light always kept"),
				T("feature.scene_culling.key_feature_3", "Character and creature shadows dropped beyond a configurable distance"),
				T("feature.scene_culling.key_feature_4", "Whole scene-graph branches skipped in the shadow and skylighting passes when the branch already meets the rule"),
				T("feature.scene_culling.key_feature_5", "Dynamic lights only re-search the geometry they light when they actually moved") } };
	}

	struct Settings
	{
		bool EnableSunShadowCulling = true;
		int SunMinCascade = 1;
		float SunMinDistance = 1500.f;
		float SunMaxRadius = 150.f;
		float SunMinAngularSize = 0.035f;
		float SunMinElevation = 25.f;

		bool EnablePointShadowCulling = true;
		bool PointShadowCullingInteriors = false;
		float PointMinDistance = 1000.f;
		float PointMaxRadius = 150.f;
		float PointMinAngularSize = 0.035f;

		bool EnableActorShadowCulling = true;
		float ActorShadowDistance = 3500.f;
		bool ActorShadowCullingPointLights = true;

		bool EnableSubtreePruning = true;

		bool EnableDecalCulling = true;
		float DecalMaxDistance = 1500.f;
		float DecalMaxRadius = 32.f;

		bool EnableLightGatherThrottle = true;
		float LightGatherMinMove = 8.f;
		float LightGatherMinRadiusChange = 16.f;
		float LightGatherMaxAgeMs = 500.f;
	};

	Settings settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void EarlyPrepass() override;

	static constexpr std::size_t kMaxCascades = 4;
	static constexpr std::size_t kMaxPointCameras = 48;
	static constexpr float kMinActorShadowDistance = 2100.f;
	static constexpr float kNearPointLightAngular = 0.1f;
	static constexpr float kMinSunSin = 0.05f;

	enum class CullerKind
	{
		Other,
		Sun,
		Point
	};

	struct Counters
	{
		std::atomic<uint32_t> sunCulled{ 0 };
		std::atomic<uint32_t> sunKept{ 0 };
		std::atomic<uint32_t> pointCulled{ 0 };
		std::atomic<uint32_t> pointKept{ 0 };
		std::atomic<uint32_t> actorCulled{ 0 };
		std::atomic<uint32_t> prunedSun{ 0 };
		std::atomic<uint32_t> prunedPrecip{ 0 };
		std::atomic<uint32_t> decalCulled{ 0 };
		std::atomic<uint32_t> lightGatherCalls{ 0 };
		std::atomic<uint32_t> lightGatherSkipped{ 0 };
	};

	struct CounterSnapshot
	{
		uint32_t sunCulled = 0, sunKept = 0, pointCulled = 0, pointKept = 0, actorCulled = 0;
		uint32_t prunedSun = 0, prunedPrecip = 0, decalCulled = 0, lightGatherCalls = 0, lightGatherSkipped = 0;
	};

	struct LightGatherEntry
	{
		const void* niLight = nullptr;
		RE::NiPoint3 position;
		float radius = 0.f;
		std::chrono::steady_clock::time_point time;
	};

	std::array<std::atomic<const RE::NiCamera*>, kMaxCascades> sunCameras{};
	std::array<std::atomic<const RE::NiCamera*>, kMaxPointCameras> pointCameras{};
	std::atomic<uint32_t> pointCameraCount{ 0 };
	std::atomic<const RE::NiCamera*> precipCamera{ nullptr };
	std::atomic<float> camX{ 0.f }, camY{ 0.f }, camZ{ 0.f };
	std::atomic<float> sunSin{ 1.f };
	std::atomic<bool> sunCullAllowed{ true };
	std::atomic<bool> pointCullAllowed{ true };
	std::atomic<bool> collectCounters{ false };

	Counters counters;
	CounterSnapshot lastCounters;
	bool showCounters = false;
	bool lightGatherHooked = false;

	std::mutex lightGatherLock;
	std::unordered_map<const RE::BSLight*, LightGatherEntry> lightGatherState;
	std::chrono::steady_clock::time_point lightGatherCleanup{};

	CullerKind Classify(const RE::BSCullingProcess* a_culler, uint32_t& a_cascade) const;
	float DistanceToCamera(const RE::NiBound& a_bound) const;
	bool ShouldCullBound(const RE::NiBound& a_bound, float a_minDistance, float a_maxRadius, float a_minAngularSize, float a_radiusScale) const;
	bool ShouldCullSun(RE::BSGeometry& a_geometry, uint32_t a_cascade) const;
	bool ShouldCullPoint(const RE::NiCamera* a_lightCamera, RE::BSGeometry& a_geometry) const;
	bool ShouldCullActorShadow(RE::BSGeometry& a_geometry, bool a_pointLight);
	bool ShouldCullDecal(RE::BSGeometry& a_geometry);
	bool ShouldPrune(RE::BSCullingProcess* a_culler, RE::NiAVObject* a_object, bool a_parabolic);
	bool ShouldSkipLightGather(RE::BSLight* a_light);
	void Count(std::atomic<uint32_t>& a_counter);

	static bool BelongsToActor(const RE::BSGeometry& a_geometry);
	static bool IsActorRoot(const RE::NiAVObject* a_object);

	struct Hooks
	{
		struct BSCullingProcess_AppendVirtual
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSParabolicCullingProcess_AppendVirtual
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N, bool Parabolic>
		struct CullingProcess_Process
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_alphaGroupIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShaderProperty_GetRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags, RE::BSShaderAccumulator* a_accumulator);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ShadowSceneNode_UpdateLightGeometry
		{
			static void thunk(RE::ShadowSceneNode* a_sceneNode, RE::BSLight* a_light);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
		static void InstallLate();
	};
};
