#include "WeatherLens.h"

#include "Features/HeatHaze.h"
#include "Globals.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"

#include "RE/B/bhkPickData.h"

#include <algorithm>
#include <cctype>

namespace CameraFXLens
{
	namespace
	{
		constexpr float kQueryInterval = 0.1f;
		constexpr float kExposureInterval = 0.3f;
		constexpr float kExposureDistance = 2500.0f;
		constexpr float kExposureTilt = 0.57f;
		constexpr float kSheetDrySeconds = 6.0f;
		constexpr float kClimbOutSheet = 0.7f;
		constexpr float kSwimSplash = 0.55f;
		constexpr float kEntryFogSeconds = 10.0f;
		constexpr float kRecentColdSeconds = 20.0f;
		constexpr float kBlizzardWindSpeed = 150.0f / 255.0f;
		constexpr float kDustWindSpeed = 200.0f / 255.0f;
		constexpr float kHeatNear = 80.0f;
		constexpr float kHeatFar = 320.0f;
		constexpr float kActorSkipDistance = 20.0f;
		constexpr uint32_t kMaxActorSkips = 3;

		struct WeatherEntry
		{
			RE::FormID localID;
			std::string_view plugin;
			WeatherAir air;
		};

		constexpr WeatherEntry kWeatherEntries[] = {
			{ 0x032336, "Dragonborn.esm", { 1.0f, 0.3f, 0.0f } },
			{ 0x0D9329, "Skyrim.esm", { 0.5f, 0.8f, 0.7f } },
			{ 0x0D64B4, "NAT-ENB.esp", { 1.0f, 0.0f, 0.0f } },
			{ 0x000823, "TrueStormsSE.esp", { 1.0f, 1.0f, 0.0f } },
			{ 0x000825, "TrueStormsSE.esp", { 0.8f, 0.0f, 0.0f } },
			{ 0x00081B, "TrueStormsSE.esp", { 0.0f, 0.0f, 1.0f } },
			{ 0x2401C8, "Glenmoril.esm", { 1.0f, 0.0f, 0.0f } },
			{ 0x0031FD, "Glenmoril.esm", { 0.0f, 0.0f, 0.6f } },
			{ 0x09A995, "legacyorsinium.esm", { 0.8f, 0.5f, 0.0f } },
			{ 0x057760, "legacyorsinium.esm", { 0.8f, 0.0f, 0.0f } },
		};

		std::unordered_map<RE::FormID, WeatherAir> knownWeathers;

		float FlagAmount(const RE::Sky* a_sky, RE::TESWeather::WeatherDataFlag a_flag)
		{
			auto has = [a_flag](const RE::TESWeather* a_weather) { return a_weather && a_weather->data.flags.any(a_flag) ? 1.0f : 0.0f; };
			const float pct = std::clamp(a_sky->currentWeatherPct, 0.0f, 1.0f);
			return a_sky->lastWeather ? std::lerp(has(a_sky->lastWeather), has(a_sky->currentWeather), pct) : has(a_sky->currentWeather);
		}

		bool IsActorLayer(RE::COL_LAYER a_layer)
		{
			return a_layer == RE::COL_LAYER::kCharController || a_layer == RE::COL_LAYER::kBiped || a_layer == RE::COL_LAYER::kBipedNoCC || a_layer == RE::COL_LAYER::kDeadBip;
		}

		bool IsOpen(RE::TES* a_tes, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			const float scale = RE::bhkWorld::GetWorldScale();
			const RE::NiPoint3 ray = a_to - a_from;
			const float length = ray.Length();
			if (!(length > kActorSkipDistance))
				return true;
			const RE::NiPoint3 skip = ray * (kActorSkipDistance / length);
			RE::NiPoint3 from = a_from;
			for (uint32_t cast = 0; cast <= kMaxActorSkips; ++cast) {
				RE::bhkPickData pick{};
				pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0.0f);
				pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
				pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
				a_tes->Pick(pick);
				if (!pick.rayOutput.HasHit())
					return true;
				if (!pick.rayOutput.rootCollidable || !IsActorLayer(pick.rayOutput.rootCollidable->GetCollisionLayer()))
					return false;
				from = from + (a_to - from) * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f) + skip;
				if ((a_to - from).Dot(ray) <= 0.0f)
					return true;
			}
			return true;
		}
	}

	void WeatherLens::ResolveForms()
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler)
			return;
		knownWeathers.clear();
		for (const auto& entry : kWeatherEntries) {
			if (const auto formID = dataHandler->LookupFormID(entry.localID, entry.plugin))
				knownWeathers[formID] = entry.air;
		}
	}

	void WeatherLens::Reset()
	{
		state = {};
		targets = {};
		queryTimer = 0.0f;
		exposureTimer = 0.0f;
		exposure = 1.0f;
		recentCold = 0.0f;
		wasUnderwater = false;
		wasInWater = false;
		wasExterior = true;
		initialised = false;
	}

	WeatherAir WeatherLens::ClassifyWeather(const RE::TESWeather* a_weather)
	{
		if (!a_weather)
			return {};
		const auto formID = a_weather->GetFormID();
		if (const auto known = knownWeathers.find(formID); known != knownWeathers.end())
			return known->second;
		if (const auto cached = weatherCache.find(formID); cached != weatherCache.end())
			return cached->second;

		float ash = 0.0f;
		if (auto* particles = a_weather->precipitationData; particles && !REL::Module::IsVR()) {
			const char* texture = particles->GetRuntimeData().particleTexture.textureName.c_str();
			std::string path = texture ? texture : "";
			std::ranges::transform(path, path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			size_t start = 0;
			while (start < path.size()) {
				while (start < path.size() && !std::isalpha(static_cast<unsigned char>(path[start])))
					++start;
				size_t end = start;
				while (end < path.size() && std::isalpha(static_cast<unsigned char>(path[end])))
					++end;
				if (path.compare(start, 3, "ash") == 0 && end - start >= 3) {
					ash = 1.0f;
					break;
				}
				start = end;
			}
		}
		return weatherCache.emplace(formID, WeatherAir{ ash, 0.0f, 0.0f }).first->second;
	}

	float WeatherLens::SampleExposure()
	{
		auto* tes = RE::TES::GetSingleton();
		auto* camera = RE::Main::WorldRootCamera();
		if (!tes || !camera)
			return 1.0f;
		const RE::NiPoint3 origin = camera->world.translate;
		const RE::NiPoint3 directions[5] = {
			{ 0.0f, 0.0f, 1.0f },
			{ kExposureTilt, 0.0f, 0.82f },
			{ -kExposureTilt, 0.0f, 0.82f },
			{ 0.0f, kExposureTilt, 0.82f },
			{ 0.0f, -kExposureTilt, 0.82f },
		};
		constexpr float weights[5] = { 0.4f, 0.15f, 0.15f, 0.15f, 0.15f };
		float open = 0.0f;
		for (size_t i = 0; i < std::size(directions); ++i) {
			if (IsOpen(tes, origin, origin + directions[i] * kExposureDistance))
				open += weights[i];
		}
		return open;
	}

	void WeatherLens::Query(const WeatherOptions& a_options)
	{
		targets = {};
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* sky = globals::game::sky;
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!player || !cell)
			return;

		state.exterior = !cell->IsInteriorCell() && sky && sky->mode.get() == RE::Sky::Mode::kFull;
		auto* water = RE::TESWaterSystem::GetSingleton();
		state.underwater = water && water->playerUnderwater;
		auto* actorState = player->AsActorState();
		state.swimming = actorState && actorState->IsSwimming();

		if (auto* camera = RE::Main::WorldRootCamera()) {
			const auto& rotate = camera->world.rotate;
			state.lookUp = std::isfinite(rotate.entry[2][0]) ? std::clamp(rotate.entry[2][0], -1.0f, 1.0f) : 0.0f;
			if (sky) {
				const RE::NiPoint3 right{ rotate.entry[0][2], rotate.entry[1][2], rotate.entry[2][2] };
				const RE::NiPoint3 wind{ std::cos(sky->windAngle), std::sin(sky->windAngle), 0.0f };
				const float speed = std::clamp(sky->windSpeed, 0.0f, 1.0f);
				state.wind = { wind.Dot(right) * speed, 0.0f };
				state.windSpeed = speed;
			}
		}

		float ash = 0.0f;
		float embers = 0.0f;
		float dust = 0.0f;
		float snow = 0.0f;
		bool exposureWanted = false;
		if (state.exterior) {
			const float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
			const auto current = ClassifyWeather(sky->currentWeather);
			const auto last = sky->lastWeather ? ClassifyWeather(sky->lastWeather) : current;
			ash = std::lerp(last.ash, current.ash, pct);
			embers = std::lerp(last.embers, current.embers, pct);
			dust = std::lerp(last.dust, current.dust, pct);
			snow = FlagAmount(sky, RE::TESWeather::WeatherDataFlag::kSnow) * (1.0f - std::clamp(ash, 0.0f, 1.0f));
			const float rain = FlagAmount(sky, RE::TESWeather::WeatherDataFlag::kRainy);

			if (a_options.rain && !state.underwater && rain > 0.0f) {
				exposureWanted = true;
				if (exposureTimer <= 0.0f) {
					exposureTimer = kExposureInterval;
					exposure = SampleExposure();
				}
				state.exposure = exposure;
				targets.rain = rain * exposure * a_options.rainStrength;
			}

			if (a_options.atmosphere) {
				targets.ash = ash * a_options.atmosphereStrength;
				targets.embers = embers * a_options.atmosphereStrength;
				const float gale = rain < 0.01f && snow < 0.01f ? 0.6f * Util::Smoothstep(kDustWindSpeed, 1.0f, state.windSpeed) : 0.0f;
				targets.dust = std::max(dust, gale) * (1.0f - std::clamp(ash, 0.0f, 1.0f)) * a_options.atmosphereStrength;
			}

			if (a_options.condensation) {
				const float fogNear = std::isfinite(sky->fogNear) ? std::max(sky->fogNear, 0.0f) : 0.0f;
				const float fogFar = std::isfinite(sky->fogFar) ? sky->fogFar : 0.0f;
				const float fogPower = std::isfinite(sky->fogPower) ? std::max(sky->fogPower, 0.01f) : 1.0f;
				const float fogMax = std::isfinite(sky->fogClamp) ? std::clamp(sky->fogClamp, 0.0f, 1.0f) : 0.0f;
				if (fogFar > fogNear + 1.0f && fogMax > 0.05f) {
					const float reach = std::pow(std::min(0.5f / fogMax, 1.0f), 1.0f / fogPower);
					const float half = fogNear + (fogFar - fogNear) * reach;
					targets.condensation = (1.0f - Util::Smoothstep(2000.0f, 20000.0f, half)) * std::clamp(fogMax * 2.0f, 0.0f, 1.0f);
				}
			}

			if (a_options.weatherFrost) {
				const bool blizzard = sky->currentWeather && sky->currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) &&
				                      ash < 0.5f && state.windSpeed >= kBlizzardWindSpeed;
				targets.frost = std::max(snow * 0.35f, blizzard ? 0.75f : 0.0f) * a_options.weatherFrostStrength;
			}
		}

		if (!exposureWanted)
			exposureTimer = 0.0f;

		targets.heat = 1.0f - Util::Smoothstep(kHeatNear, kHeatFar, globals::features::heatHaze.GetNearestFireDistance());

		if (a_options.coldBreath || a_options.entryFog)
			targets.breathCold = state.exterior ? std::max(snow * 0.85f, a_options.coldTheme) : a_options.coldTheme;
	}

	void WeatherLens::Update(float a_step, float a_delta, const WeatherOptions& a_options)
	{
		queryTimer -= a_delta;
		if (queryTimer <= 0.0f || !initialised) {
			queryTimer = kQueryInterval;
			exposureTimer -= kQueryInterval;
			Query(a_options);
		}

		if (!initialised) {
			initialised = true;
			wasUnderwater = state.underwater;
			wasInWater = state.underwater || state.swimming;
			wasExterior = state.exterior;
		}

		if (state.underwater) {
			state.sheet = 0.0f;
			state.rain = 0.0f;
		} else if (wasUnderwater) {
			state.sheet = 1.0f;
		} else if (wasInWater && !state.swimming) {
			state.sheet = std::max(state.sheet, kClimbOutSheet);
		} else {
			state.sheet = std::max(0.0f, state.sheet - a_step / kSheetDrySeconds);
		}
		const bool splashing = state.swimming && !state.underwater && a_options.waterSheet;
		state.splash = Util::Approach(state.splash, splashing ? kSwimSplash : 0.0f, 2.0f, 0.5f, a_step);
		if (!a_options.waterSheet) {
			state.sheet = 0.0f;
			state.splash = 0.0f;
		}
		wasUnderwater = state.underwater;
		wasInWater = state.underwater || state.swimming;

		recentCold = std::max(recentCold - a_step / kRecentColdSeconds, state.exterior ? state.breathCold : 0.0f);
		if (a_options.entryFog && wasExterior && !state.exterior && recentCold > 0.35f)
			state.entryFog = 1.0f;
		else
			state.entryFog = std::max(0.0f, state.entryFog - a_step / kEntryFogSeconds);
		if (!a_options.entryFog)
			state.entryFog = 0.0f;
		wasExterior = state.exterior;

		state.rain = Util::Approach(state.rain, targets.rain, 0.8f, 1.0f / 6.0f, a_step);
		state.condensation = Util::Approach(state.condensation, targets.condensation, 0.2f, 0.1f, a_step);
		state.ash = Util::Approach(state.ash, targets.ash, 0.1f, 0.05f, a_step);
		state.dust = Util::Approach(state.dust, targets.dust, 0.25f, 0.1f, a_step);
		state.embers = Util::Approach(state.embers, targets.embers, 0.5f, 0.3f, a_step);
		state.heat = Util::Approach(state.heat, targets.heat, 0.8f, 0.5f, a_step);
		state.frost = Util::Approach(state.frost, targets.frost, 0.03f, 0.2f, a_step);
		state.breathCold = Util::Approach(state.breathCold, targets.breathCold, 0.2f, 0.05f, a_step);
	}
}
