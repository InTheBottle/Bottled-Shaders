#pragma once

#include <unordered_map>

namespace CameraFXLens
{
	struct WeatherAir
	{
		float ash = 0.0f;
		float embers = 0.0f;
		float dust = 0.0f;
	};

	struct WeatherOptions
	{
		bool rain = true;
		float rainStrength = 1.0f;
		bool waterSheet = true;
		bool weatherFrost = true;
		float weatherFrostStrength = 1.0f;
		bool coldBreath = true;
		bool entryFog = true;
		bool condensation = true;
		bool atmosphere = true;
		float atmosphereStrength = 1.0f;
		float coldTheme = 0.0f;
	};

	struct WeatherState
	{
		float rain = 0.0f;
		float lookUp = 0.0f;
		float sheet = 0.0f;
		float splash = 0.0f;
		float exposure = 1.0f;
		float entryFog = 0.0f;
		float condensation = 0.0f;
		float ash = 0.0f;
		float dust = 0.0f;
		float embers = 0.0f;
		float heat = 0.0f;
		float frost = 0.0f;
		float breathCold = 0.0f;
		float2 wind{};
		float windSpeed = 0.0f;
		bool underwater = false;
		bool swimming = false;
		bool exterior = false;
	};

	class WeatherLens
	{
	public:
		static void ResolveForms();
		void Update(float a_step, float a_delta, const WeatherOptions& a_options);
		void Reset();
		const WeatherState& Get() const { return state; }

	private:
		struct Targets
		{
			float rain = 0.0f;
			float condensation = 0.0f;
			float ash = 0.0f;
			float dust = 0.0f;
			float embers = 0.0f;
			float heat = 0.0f;
			float frost = 0.0f;
			float breathCold = 0.0f;
		};

		void Query(const WeatherOptions& a_options);
		float SampleExposure();
		WeatherAir ClassifyWeather(const RE::TESWeather* a_weather);

		WeatherState state;
		Targets targets;
		float queryTimer = 0.0f;
		float exposureTimer = 0.0f;
		float exposure = 1.0f;
		float recentCold = 0.0f;
		bool wasUnderwater = false;
		bool wasInWater = false;
		bool wasExterior = true;
		bool initialised = false;
		std::unordered_map<RE::FormID, WeatherAir> weatherCache;
	};
}
