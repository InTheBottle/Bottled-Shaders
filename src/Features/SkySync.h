#pragma once
#include "RE/M/Moon.h"

#include "Utils/Moon.h"

/** @brief Synchronizes volumetric lighting and shadow direction with actual sun and moon positions. */
struct SkySync : Feature
{
private:
	static constexpr std::string_view MOD_ID = "153543";

public:
	virtual inline std::string GetName() override { return "Sky Sync"; }
	virtual std::string GetDisplayName() override { return T("feature.sky_sync.name", "Sky Sync"); }
	virtual inline std::string GetShortName() override { return "SkySync"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.sky_sync.description", "Synchronizes volumetric lighting and shadows with the actual sun and moon positions in the sky."),
			{ T("feature.sky_sync.key_feature_1", "Fixes the mismatch between the positions of the sun and moons and the lighting direction"),
				T("feature.sky_sync.key_feature_2", "Includes a configurable alternative sun path for more realistic and dramatic lighting"),
				T("feature.sky_sync.key_feature_3", "Smoothly switches the light source between the sun and moons based on visibility"),
				T("feature.sky_sync.key_feature_4", "Moon light source can be switched between Masser, Secunda, or the brightest"),
				T("feature.sky_sync.key_feature_5", "Automatic calculation of moon lighting intensity based on moon phase"),
				T("feature.sky_sync.key_feature_6", "Fixes the sun appearing higher on the horizon when the player gains altitude") } };
	};

	struct Settings
	{
		bool Enabled = true;
		bool UseAlternateSunPath = false;
		int32_t MoonLightSource = 0;
		int32_t SunPath = 0;
		float CustomAngle = -35.0f;
		float MinShadowElevation = 10.0f;
		float ShadowTransitionDuration = 100.0f;
		bool DimSunlightUnderHorizon = true;
		bool DimVolumetricLighting = true;
		float HorizonFadeHours = 0.7f;
		float NewMoonIntensity = 0.05f;
		float CrescentMoonIntensity = 0.25f;
		float FullMoonIntensity = 1.0f;
	};

	Settings settings;

	/** @brief Draws the ImGui settings panel for Sky Sync configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual bool IsCore() const override { return true; }

	/**
	 * @brief Dims sunlight color when the sun is below the horizon during sky color updates.
	 * @param sky The sky object whose directional light color may be modified.
	 */
	void OnSkyUpdateColors(RE::Sky* sky);

	/** @brief Installs rendering hooks and detects conflicting mods after plugin load. */
	virtual void PostPostLoad() override;
	/** @brief Checks for conflicting ESP files after game data is loaded. */
	virtual void DataLoaded() override;
	/** @brief Requests immediate celestial synchronization after loading an existing save. */
	virtual void GameLoaded() override;

	struct Sky_Update
	{
		static void thunk(RE::Sky* sky);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	enum class Caster : uint8_t
	{
		Sun,
		Masser,
		Secunda,
		None
	};

	/**
	 * @brief Gets the world-space direction towards a sun or moon, preferring the one Sky Sync computed this frame.
	 * @param sky The sky whose root rotation maps Sky Sync's directions into world space.
	 * @param caster The celestial body to query; must not be Caster::None.
	 * @return The unit direction, or straight up (0,0,1) if none is available.
	 */
	RE::NiPoint3 GetCelestialDirection(const RE::Sky* sky, Caster caster) const;

	/**
	 * @brief Moon that would cast shadows under the "Moon light source" setting, when it is currently visible.
	 * @return Caster::Masser or Caster::Secunda, or Caster::None when that moon is hidden or faded out.
	 */
	Caster GetMoonLightCaster(const RE::Sky* sky) const;

	/**
	 * @brief Gets the world-space direction towards the current shadow caster, without the minimum shadow elevation clamp.
	 * @param sky The sky whose root rotation maps Sky Sync's directions into world space.
	 * @return The unit direction, or nullopt if Sky Sync did not update the caster this frame.
	 */
	std::optional<RE::NiPoint3> GetCelestialLightDirection(const RE::Sky* sky) const;

private:
	enum class CellFlagExt : uint16_t
	{
		kSunlightShadows = 1 << 15,
	};

	enum class MoonLightSource : uint8_t
	{
		Brightest,
		Masser,
		Secunda,
		Count
	};

	enum class SunPath : uint8_t
	{
		Southern,
		Northern,
		Vanilla,
		Custom,
		Count
	};

	/** @brief Where the current game hour sits relative to the climate's sunrise and sunset windows. */
	struct DayPhase
	{
		float dim = 1.0f;
		bool sunSetting = false;
		bool sunRising = false;
		bool sunBelowHorizon = false;
	};

	struct ShadowFader
	{
		RE::NiPoint3 currentDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 startDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 celestialDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 startCelestialDir = { 0.0f, 0.0f, 1.0f };
		Caster target = Caster::Sun;
		float fadeTimer = 0.0f;
		bool transitioning = false;
		bool sunriseReleased = false;
		bool sunsetHeadingLocked = false;
		float frozenHeading = 0.0f;
		float vlIntensityFactor = 1.0f;

		void Update(const RE::Sky* sky, RE::NiPoint3 dirs[], Caster best, const DayPhase& phase, float minElevRadians, float fadeDuration, float fadeAdvance, bool snap);
		void LockSunElevation(RE::NiPoint3& sunDir, const DayPhase& phase, float minElevRadians);
		static void SetLighting(const RE::Sky* sky, RE::NiPoint3 dir, float minElevRadians);
		static void SetDirection(RE::NiPoint3& dir, float headingRadians, float elevRadians);
		static void SetElevation(RE::NiPoint3& dir, float elevRadians);
		static float ComputeVLFactor(const RE::NiPoint3& current, const RE::NiPoint3& target);
		static RE::NiPoint3 LerpDirection(const RE::NiPoint3& from, const RE::NiPoint3& to, float t);
		void Reset();
	};

	static constexpr float SunHorizonDistance = 280.0f;
	static constexpr float SunPeakDistance = 400.0f;
	static constexpr float SouthernSunAngle = 90.0f - 35.0f;
	static constexpr float NorthernSunAngle = 90.0f + 35.0f;
	static constexpr float VanillaSunAngle = 90.0f + 5.0f;
	static constexpr float SecondsPerGameHour = 3600.0f;
	static constexpr float SunsetHeadingLockThreshold = 0.5f;
	static constexpr float VLFadeStartAngle = 2.0f;
	static constexpr float VLFadeEndAngle = 10.0f;
	static constexpr float MaxHorizonFadeHours = 1.5f;

	inline static RE::NiPoint3* gSunPosition = nullptr;
	inline static RE::Setting* gSunAlphaTransTime = nullptr;

	std::string conflictingMod;
	RE::TESObjectCELL* currentCell = nullptr;
	bool currentCellInterior = false;
	RE::TESWorldSpace* currentCellWorldspace = nullptr;
	float sunAngle = 90.0f;
	float currentSkyRotation = D3D11_FLOAT32_MAX;
	float lastGameHour = -1.0f;
	bool immediateTransitionReady = false;

	RE::NiPoint3 rawDirections[3] = {};
	DayPhase dayPhase;
	ShadowFader shadowFader;

	void DisableOnConflict(std::string_view conflictName);

	void ApplyConflictDisable();

	void PreparePendingTransitions();

	bool Update(const RE::Sky* sky);

	void SetSunAngle();

	void SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell);

	DayPhase ComputeDayPhase(const RE::TESClimate::Timing& timing, float hour) const;

	void ProcessSun(const RE::Sky* sky, RE::NiPoint3& outDir);

	/** @brief Zeroes the sun disc and glare alpha outside vanilla's sun fade window, whose exclusive bounds leave them opaque at the exact end hours. */
	static void HideSunOutsideFadeWindow(const RE::Sky* sky);

	/**
	 * @brief Reads a moon's sky-local facing direction and blend alpha.
	 * @return The moon's blend alpha, or a negative value when the moon is missing or hidden.
	 */
	static float ReadMoon(const RE::Moon* moon, RE::NiPoint3& outDir);

	/**
	 * @brief Applies the "Moon light source" preference to the two moons.
	 * @param masserAlpha Masser's blend alpha, negative when it is unavailable.
	 * @param secundaAlpha Secunda's blend alpha, negative when it is unavailable.
	 * @return The preferred available moon, the brighter one for Brightest, or Caster::None when neither is available.
	 */
	Caster SelectMoonCaster(float masserAlpha, float secundaAlpha) const;

	static void CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance);

	static void CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, float time, float sunrise, float sunset, float sunAngle);

	static void SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, float distance);
};
