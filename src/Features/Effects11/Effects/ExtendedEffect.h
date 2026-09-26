#pragma once

#include "Effect.h"
#include "../UITree.h"

#include <span>

#ifdef ENABLE_ENB_EXTENDER

class ExtendedEffect : public Effect
{
public:
	void LoadWeatherData();
	/** @brief Writes per-weather values edited in the UI back to the weather ini files they came from. */
	void SaveWeatherData();
	void ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID);
	/** @brief Stores a UI edit of uiVariables[index] in the active weather's values if that weather supplies it, else as the preset value. */
	void CommitUIEdit(size_t index, uint32_t weatherID);
	void ApplyTimeOfDayInterpolation();

	void Unload() override;
	bool IsTechniqueEnabled(TechniqueInfo& info) override;

	// Rendering
	void RenderImGui() override;
	static void RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter = UITree::FilterMode::All);

private:
	using WeatherValues = std::unordered_map<std::string, std::string>;
	std::unordered_map<uint32_t, WeatherValues> weatherData;
	std::unordered_map<std::string, std::unordered_set<std::string>> editedWeatherKeys;  ///< weather file name -> keys edited since the last save
	bool weatherValuesApplied = false;                                                   ///< Live values currently hold weather values rather than preset values

	std::unordered_map<std::string, int> bindingCache;

	int ResolveTechniqueBinding(const std::string& variableName);
	static float GetPeriodWeight(const std::string& period);
};

using EffectBase = ExtendedEffect;

#else

using EffectBase = Effect;

#endif
