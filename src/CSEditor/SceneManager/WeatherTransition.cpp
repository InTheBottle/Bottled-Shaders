#include "WeatherTransition.h"

#include <bit>

namespace
{
	/// Raised by Sky::SetWeather to rush the running transition; CommonLib leaves it unnamed.
	constexpr auto kAccelerateFlag = static_cast<RE::Sky::Flags>(1 << 3);

	float lastRawPct = 1.0f;
	float lastEasedPct = 1.0f;
	bool accelerateSnapshotRestored = false;

	/// The engine keeps the blend at the moment acceleration began in CommonLib's lastMoonPhaseUpdate slot.
	float GetAccelerateSnapshot(const RE::Sky* sky) { return std::bit_cast<float>(sky->lastMoonPhaseUpdate); }

	/** @brief Overwrites the accelerate start blend. */
	void SetAccelerateSnapshot(RE::Sky* sky, float pct) { sky->lastMoonPhaseUpdate = std::bit_cast<std::uint32_t>(pct); }

	float EaseTransition(float linear)
	{
		const float progress = std::clamp(linear, 0.0f, 1.0f);
		return progress * progress * progress * (progress * (progress * 6.0f - 15.0f) + 10.0f);
	}
}

namespace WeatherTransition
{
	void BeforeUpdate(RE::Sky* sky)
	{
		// The engine returns early without recomputing when there is no climate, so it must not keep our eased value.
		if (sky->currentWeatherPct == lastEasedPct)
			sky->currentWeatherPct = lastRawPct;

		// SetWeather snapshots the blend we eased between frames; the engine resumes from it on its linear scale.
		const bool accelerating = sky->flags.any(kAccelerateFlag);
		if (accelerating && !accelerateSnapshotRestored && GetAccelerateSnapshot(sky) == lastEasedPct)
			SetAccelerateSnapshot(sky, lastRawPct);
		accelerateSnapshotRestored = accelerating;
	}

	void AfterUpdate(RE::Sky* sky)
	{
		lastRawPct = sky->currentWeatherPct;
		lastEasedPct = EaseTransition(lastRawPct);
		sky->currentWeatherPct = lastEasedPct;
	}

	float GetRawPct()
	{
		return lastRawPct;
	}
}
