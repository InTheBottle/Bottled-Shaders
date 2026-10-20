#pragma once

/// Eases the sky's weather blend, run around Sky::UpdateWeather.
/// The engine rebuilds currentWeatherPct from the clock every frame, so the eased value never feeds back.
namespace WeatherTransition
{
	/** @brief Puts back the raw blend the engine expects. */
	void BeforeUpdate(RE::Sky* sky);

	/** @brief Remembers the engine's raw blend and replaces it with the eased one. */
	void AfterUpdate(RE::Sky* sky);

	float GetRawPct();
}
