#include "SkySync.h"
#include "../I18n/I18n.h"
#include "RE/B/BSVolumetricLightingRenderData.h"

#include "Utils/Game.h"
#include "VolumetricLighting.h"

#define I18N_KEY_PREFIX "feature.sky_sync."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkySync::Settings,
	Enabled,
	UseAlternateSunPath,
	MoonLightSource,
	SunPath,
	CustomAngle,
	MinShadowElevation,
	ShadowTransitionDuration,
	DimSunlightUnderHorizon,
	DimVolumetricLighting,
	HorizonFadeHours,
	NewMoonIntensity,
	CrescentMoonIntensity,
	FullMoonIntensity)

void SkySync::DrawSettings()
{
	const char* sunPathNames[] = {
		T(TKEY("sun_path_southern"), "Southern Sky"),
		T(TKEY("sun_path_northern"), "Northern Sky"),
		T(TKEY("sun_path_vanilla"), "Vanilla"),
		T(TKEY("sun_path_custom"), "Custom")
	};
	const char* moonLightSourceNames[] = {
		T(TKEY("moon_light_source_brightest"), "Brightest"),
		T(TKEY("moon_light_source_masser"), "Masser"),
		T(TKEY("moon_light_source_secunda"), "Secunda")
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("enabled_tooltip"), "Enable or disable Sky Sync features."));
	}

	ImGui::Checkbox(T(TKEY("use_alternate_sun_path"), "Use alternate sun path"), &settings.UseAlternateSunPath);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("use_alternate_sun_path_tooltip"), "Calculate sun position based on time of day and season instead of vanilla movement."));
	}

	if (settings.UseAlternateSunPath) {
		if (ImGui::SliderInt(T(TKEY("sun_path"), "Sun path"), &settings.SunPath, 0, static_cast<uint8_t>(SunPath::Count) - 1, sunPathNames[settings.SunPath], ImGuiSliderFlags_AlwaysClamp))
			SetSunAngle();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("sun_path_tooltip"), "Choose the trajectory the sun takes across the sky."));
		}

		if (settings.SunPath == static_cast<int32_t>(SunPath::Custom)) {
			if (ImGui::SliderFloat(T(TKEY("custom_angle"), "Custom angle"), &settings.CustomAngle, -90.0f, 90.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				SetSunAngle();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("custom_angle_tooltip"), "Set a custom angle for the sun's trajectory."));
			}
		}
	}

	ImGui::SliderInt(T(TKEY("moon_light_source"), "Moon light source"), &settings.MoonLightSource, 0, static_cast<uint8_t>(MoonLightSource::Count) - 1, moonLightSourceNames[settings.MoonLightSource], ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("moon_light_source_tooltip"), "Select which moon casts shadows during the night."));
	}

	ImGui::SliderFloat(T(TKEY("min_shadow_elevation"), "Min Shadow Elevation"), &settings.MinShadowElevation, 0.0f, 45.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_shadow_elevation_tooltip"), "The minimum angle sunlight will set to. Caps shadow length. Higher = shorter shadows at sunset/sunrise."));
	}

	ImGui::SliderFloat(T(TKEY("shadow_transition_duration"), "Shadow Transition Duration"), &settings.ShadowTransitionDuration, 0.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shadow_transition_duration_tooltip"), "How long (in game-time units) the shadow direction takes to fade between sources. 100 = ~5 seconds at timescale 20."));
	}

	ImGui::Checkbox(T(TKEY("dim_sunlight_under_horizon"), "Dim Sunlight Under Horizon"), &settings.DimSunlightUnderHorizon);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("dim_sunlight_under_horizon_tooltip"), "Fade directional light to zero as the sun goes below the horizon."));
	}

	ImGui::Checkbox(T(TKEY("fade_volumetric_lighting"), "Fade Volumetric Lighting"), &settings.DimVolumetricLighting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("fade_volumetric_lighting_tooltip"), "Also fade volumetric lighting with the directional dim around dawn and dusk."));
	}

	if (settings.DimSunlightUnderHorizon || settings.DimVolumetricLighting) {
		ImGui::SliderFloat(T(TKEY("horizon_fade_duration"), "Horizon Fade Duration"), &settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours, "%.1f h", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("horizon_fade_duration_tooltip"), "How long (in game hours) the dim eases out after sunset and back in before sunrise."));
		}
	}

	ImGui::SliderFloat(T(TKEY("new_moon_intensity"), "New Moon Intensity"), &settings.NewMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("crescent_intensity"), "Crescent Intensity"), &settings.CrescentMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("full_moon_intensity"), "Full Moon Intensity"), &settings.FullMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

	if (ImGui::TreeNodeEx("Debug", ImGuiTreeNodeFlags_None)) {
		static constexpr const char* CasterNames[] = { "Sun", "Masser", "Secunda", "None" };
		static constexpr const char* PhaseNames[] = { "Full", "Waning Gibbous", "Waning Quarter", "Waning Crescent", "New", "Waxing Crescent", "Waxing Quarter", "Waxing Gibbous" };

		auto getPhase = [](const RE::Moon* moon) -> const char* {
			if (!moon || !moon->moonMesh)
				return "Unknown";
			if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get())) {
				if (auto tex = prop->GetBaseTexture())
					return PhaseNames[static_cast<int>(Util::Moon::GetPhaseFromTexture(tex->name.c_str()))];
			}
			return "Unknown";
		};

		auto drawMoonEntry = [&](const char* label, const RE::Moon* moon, const float4& baseColor) {
			float4 color = {};
			if (moon && moon->root && !moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				color = Util::Moon::GetBlendColor(moon, baseColor, settings.NewMoonIntensity, settings.CrescentMoonIntensity, settings.FullMoonIntensity);
			ImVec4 swatch = { color.x, color.y, color.z, 1.0f };
			ImGui::ColorButton(label, swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
			ImGui::SameLine();
			ImGui::Text("%s  [%s]  color (%.3f, %.3f, %.3f, %.3f)", label, getPhase(moon), color.x, color.y, color.z, color.w);
		};

		const auto sky = globals::game::sky;
		drawMoonEntry("Masser", sky ? sky->masser : nullptr, Util::Moon::MasserBaseColor);
		drawMoonEntry("Secunda", sky ? sky->secunda : nullptr, Util::Moon::SecundaBaseColor);

		ImGui::Text("Dim: %.3f", dayPhase.dim);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::Text("Shadow target: %s", CasterNames[static_cast<int>(shadowFader.target)]);
		ImGui::Text("Shadow dir:    (%.2f, %.2f, %.2f)", shadowFader.currentDir.x, shadowFader.currentDir.y, shadowFader.currentDir.z);
		ImGui::Text("VL intensity factor: %.3f", shadowFader.vlIntensityFactor);
		if (shadowFader.transitioning) {
			const float t = settings.ShadowTransitionDuration > 0.0f ? shadowFader.fadeTimer / settings.ShadowTransitionDuration : 1.0f;
			ImGui::ProgressBar(t, { -1.0f, 0.0f }, "");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::Text("Transitioning %.0f%%", t * 100.0f);
		} else {
			ImGui::TextDisabled("No transition");
		}

		ImGui::TreePop();
	}
}

void SkySync::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MoonLightSource = std::clamp(settings.MoonLightSource, static_cast<int32_t>(MoonLightSource::Brightest), static_cast<int32_t>(MoonLightSource::Secunda));
	settings.SunPath = std::clamp(settings.SunPath, static_cast<int32_t>(SunPath::Southern), static_cast<int32_t>(SunPath::Custom));
	settings.CustomAngle = std::clamp(settings.CustomAngle, -90.0f, 90.0f);
	settings.MinShadowElevation = std::clamp(settings.MinShadowElevation, 0.0f, 45.0f);
	settings.ShadowTransitionDuration = std::max(settings.ShadowTransitionDuration, 0.0f);
	settings.HorizonFadeHours = std::clamp(settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours);
	settings.NewMoonIntensity = std::clamp(settings.NewMoonIntensity, 0.0f, 1.0f);
	settings.CrescentMoonIntensity = std::clamp(settings.CrescentMoonIntensity, 0.0f, 1.0f);
	settings.FullMoonIntensity = std::clamp(settings.FullMoonIntensity, 0.0f, 1.0f);
	SetSunAngle();

	// Feature::Load marks the feature loaded again on every settings reload, so the conflict has to be reapplied.
	if (!conflictingMod.empty())
		ApplyConflictDisable();
}

void SkySync::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkySync::RestoreDefaultSettings()
{
	settings = {};
	SetSunAngle();
	if (!conflictingMod.empty())
		ApplyConflictDisable();
}

void SkySync::PostPostLoad()
{
	if (Util::Moon::IsMoonAndStarsLoaded())
		logger::info("[Sky Sync] Moon and Stars detected, compatibility enabled");

	if (GetModuleHandle(L"EVLaS.dll")) {
		DisableOnConflict("EVLaS");
		return;
	}

	stl::detour_thunk<Sky_Update>(REL::RelocationID(25682, 26229));
	Util::SetCelestialTransitionHandlerAvailable(true);

	gSunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	logger::info("[Sky Sync] Installed hooks");
}

void SkySync::DataLoaded()
{
	const auto data = RE::TESDataHandler::GetSingleton();
	if (data && (data->LookupLoadedModByName("DVLaSS.esp"sv) || data->LookupLoadedLightModByName("DVLaSS.esp"sv)))
		DisableOnConflict("DVLaSS");

	if (const auto collection = globals::game::gameSettingCollection)
		gSunAlphaTransTime = collection->GetSetting("fSunAlphaTransTime");
}

void SkySync::GameLoaded()
{
	Util::RequestGameLoadTransition();
}

void SkySync::DisableOnConflict(std::string_view conflictName)
{
	conflictingMod = conflictName;
	ApplyConflictDisable();
}

void SkySync::ApplyConflictDisable()
{
	failedLoadedMessage = fmt::format("Disabled as {} has been detected, both cannot be used together", conflictingMod);
	loaded = false;
	settings.Enabled = false;
	Util::SetCelestialTransitionHandlerAvailable(false);
	logger::warn("[Sky Sync] {}", failedLoadedMessage);
}

void SkySync::OnSkyUpdateColors(RE::Sky* sky)
{
	if (!loaded || !settings.Enabled || !sky)
		return;

	// Sky::Update calls this before the Sky_Update hook recomputes the phase, so the values are one frame old.
	const float dim = dayPhase.dim;
	if (settings.DimSunlightUnderHorizon && dim < 1.0f) {
		auto& dirLight = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSunlight)];
		dirLight.red *= dim;
		dirLight.green *= dim;
		dirLight.blue *= dim;
	}

	float vlFactor = shadowFader.vlIntensityFactor;
	if (settings.DimVolumetricLighting)
		vlFactor *= dim;
	if (vlFactor != 1.0f)
		VolumetricLighting::GetRenderData().intensity *= vlFactor;
}

void SkySync::Sky_Update::thunk(RE::Sky* sky)
{
	func(sky);
	auto& skySync = globals::features::skySync;
	skySync.PreparePendingTransitions();
	if (skySync.Update(sky))
		Util::CompleteCelestialTransition();
}

void SkySync::PreparePendingTransitions()
{
	const auto request = Util::ConsumeCelestialTransitionRequest();
	if (!request.timeJump && !request.gameLoad)
		return;

	if (request.gameLoad) {
		currentCell = nullptr;
		currentCellInterior = false;
		currentCellWorldspace = nullptr;
		currentSkyRotation = D3D11_FLOAT32_MAX;
		lastGameHour = -1.0f;
	}

	immediateTransitionReady = true;
}

bool SkySync::Update(const RE::Sky* sky)
{
	std::fill(std::begin(rawDirections), std::end(rawDirections), RE::NiPoint3{});

	if (!settings.Enabled) {
		dayPhase = {};
		return std::exchange(immediateTransitionReady, false);
	}

	// A pending immediate transition stays queued through these early-outs until a frame can act on it.
	if (!sky || !sky->root)
		return false;

	const auto sun = sky->sun;
	const auto climate = sky->currentClimate;
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!sun || !sun->root || !sun->sunGlareNode || !sun->light || !climate || !player) {
		dayPhase = {};
		return false;
	}

	const auto cell = player->GetParentCell();
	bool contextChanged = false;

	if (cell != currentCell) {
		// Crossing between exterior cells of one worldspace keeps the same sky, so only interior/exterior
		// and worldspace changes (and the cell-less frames around loads) count as a lighting context change.
		const auto prevCell = currentCell;
		contextChanged = !cell || !prevCell ||
		                 cell->IsInteriorCell() != currentCellInterior ||
		                 cell->GetRuntimeData().worldSpace != currentCellWorldspace;
		if (cell)
			SetSkyRotation(sky, cell);
		else
			currentCell = nullptr;  // keep the cache in sync so a cell-less frame doesn't reset every frame
	}

	// Exterior worldspaces always run; interior cells require the sunlight-shadows flag.
	if (cell && cell->IsInteriorCell() && !cell->cellFlags.all(static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows))) {
		dayPhase = {};
		return false;
	}

	dayPhase = ComputeDayPhase(climate->timing, sky->currentGameHour);

	RE::NiPoint3 directions[3] = {};
	ProcessSun(sky, directions[static_cast<int>(Caster::Sun)]);
	const float masserAlpha = ReadMoon(sky->masser, directions[static_cast<int>(Caster::Masser)]);
	const float secundaAlpha = ReadMoon(sky->secunda, directions[static_cast<int>(Caster::Secunda)]);
	std::copy(std::begin(directions), std::end(directions), std::begin(rawDirections));

	const Caster best = dayPhase.sunBelowHorizon ? SelectMoonCaster(masserAlpha, secundaAlpha) : Caster::Sun;

	// Fade in game time so waiting, fast travel and timescale changes all advance it consistently.
	const float gameHour = sky->currentGameHour;
	float fadeAdvance = 0.0f;
	if (lastGameHour >= 0.0f) {
		float hourDelta = gameHour - lastGameHour;
		if (hourDelta > 12.0f)
			hourDelta -= 24.0f;
		else if (hourDelta < -12.0f)
			hourDelta += 24.0f;
		fadeAdvance = std::abs(hourDelta) * SecondsPerGameHour;
	}
	lastGameHour = gameHour;

	const bool transitionCompleted = std::exchange(immediateTransitionReady, false);
	const bool snap = transitionCompleted || contextChanged;
	if (snap)
		shadowFader.Reset();

	const float minElevRadians = DirectX::XMConvertToRadians(settings.MinShadowElevation);
	shadowFader.Update(sky, directions, best, dayPhase, minElevRadians, settings.ShadowTransitionDuration, fadeAdvance, snap);
	return transitionCompleted;
}

void SkySync::SetSunAngle()
{
	switch (static_cast<SunPath>(settings.SunPath)) {
	case SunPath::Southern:
		sunAngle = SouthernSunAngle;
		break;
	case SunPath::Northern:
		sunAngle = NorthernSunAngle;
		break;
	case SunPath::Vanilla:
		sunAngle = VanillaSunAngle;
		break;
	case SunPath::Custom:
		sunAngle = 90.0f + settings.CustomAngle;
		break;
	default:;
	}
}

void SkySync::SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell)
{
	// If the interior cell isn't initialised it won't have the north rotation extra data ready, skip for a frame
	if (cell->IsInteriorCell() && cell->cellState == static_cast<RE::TESObjectCELL::CellState>(0))
		return;

	currentCell = cell;
	currentCellInterior = cell->IsInteriorCell();
	currentCellWorldspace = cell->GetRuntimeData().worldSpace;
	const float rotation = cell->GetNorthRotation();
	if (rotation == currentSkyRotation)
		return;

	currentSkyRotation = rotation;
	sky->root->local.rotate = RE::NiMatrix3{ RE::NiPoint3{ 0.0f, 0.0f, -rotation } };
	RE::NiUpdateData updateData;
	sky->root->Update(updateData);
}

SkySync::DayPhase SkySync::ComputeDayPhase(const RE::TESClimate::Timing& timing, const float hour) const
{
	const float sunriseBegin = timing.sunrise.begin / 6.0f;
	const float sunriseMiddle = (timing.sunrise.begin + timing.sunrise.end) / 12.0f;
	const float sunsetMiddle = (timing.sunset.begin + timing.sunset.end) / 12.0f;
	const float sunsetEnd = timing.sunset.end / 6.0f;
	const float fadeHours = settings.HorizonFadeHours;

	// Hours elapsed from a to b, wrapping across midnight so gap windows survive past 24h.
	auto hoursBetween = [](float from, float to) {
		float d = to - from;
		return d < 0.0f ? d + 24.0f : d;
	};

	DayPhase phase;
	phase.sunSetting = hour >= sunsetMiddle && hour < sunsetEnd;
	phase.sunRising = hour >= sunriseBegin && hour < sunriseMiddle;
	phase.sunBelowHorizon = hour >= sunsetEnd || hour < sunriseBegin;

	const float sinceSunset = hoursBetween(sunsetEnd, hour);
	const float untilSunrise = hoursBetween(hour, sunriseBegin);

	if (phase.sunSetting) {
		// Dusk: sun dipping under the horizon, fade the directional light out.
		const float range = sunsetEnd - sunsetMiddle;
		const float t = range > 0.0f ? (hour - sunsetMiddle) / range : 1.0f;
		phase.dim = std::sqrt(1.0f - t);
	} else if (fadeHours > 0.0f && sinceSunset < fadeHours) {
		// Caster has swapped to the moon but the colour is still dusk-bright; ease the dim back out.
		phase.dim = sinceSunset / fadeHours;
	} else if (fadeHours > 0.0f && untilSunrise > 0.0f && untilSunrise <= fadeHours) {
		// Still on the moon but the colour is brightening toward dawn; ease the dim back in.
		phase.dim = untilSunrise / fadeHours;
	} else if (phase.sunRising) {
		// Dawn: sun rising above the horizon, fade the directional light in.
		const float range = sunriseMiddle - sunriseBegin;
		const float t = range > 0.0f ? (hour - sunriseBegin) / range : 1.0f;
		phase.dim = std::sqrt(t);
	}

	return phase;
}

void SkySync::ProcessSun(const RE::Sky* sky, RE::NiPoint3& outDir)
{
	const auto sun = sky->sun;
	float dist;

	if (settings.UseAlternateSunPath) {
		const auto& timing = sky->currentClimate->timing;
		const float sunrise = (timing.sunrise.begin / 6.0f + timing.sunrise.end / 6.0f) * 0.5f - 0.25f;
		const float sunset = (timing.sunset.begin / 6.0f + timing.sunset.end / 6.0f) * 0.5f + 0.25f;
		CalculateAlternateSunDirectionAndDistance(outDir, dist, sky->currentGameHour, sunrise, sunset, sunAngle);
	} else
		CalculateSunDirectionAndDistance(sun, outDir, dist);

	SetSunPosition(sun, outDir, dist);
	HideSunOutsideFadeWindow(sky);
}

void SkySync::HideSunOutsideFadeWindow(const RE::Sky* sky)
{
	if (!gSunAlphaTransTime)
		return;

	// Same float ops as Sun::Update so the bounds match its exactly, but made inclusive
	constexpr float HoursPerTimingUnit = 1.0f / 6.0f;
	auto middleHour = [](const RE::TESClimate::Timing::Interval& interval) {
		return (interval.end * HoursPerTimingUnit + interval.begin * HoursPerTimingUnit) * 0.5f;
	};
	const auto& timing = sky->currentClimate->timing;
	const float halfTransition = gSunAlphaTransTime->GetFloat() * 0.5f;
	const float fadeInStart = middleHour(timing.sunrise) - halfTransition;
	const float fadeOutEnd = middleHour(timing.sunset) + halfTransition;
	const float hour = sky->currentGameHour;
	if (hour > fadeInStart && hour < fadeOutEnd)
		return;

	for (const auto& geometry : { sky->sun->sunBase, sky->sun->sunGlare }) {
		if (const auto prop = geometry ? skyrim_cast<RE::BSSkyShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get()) : nullptr)
			prop->kBlendColor.alpha = 0.0f;
	}
}

float SkySync::ReadMoon(const RE::Moon* moon, RE::NiPoint3& outDir)
{
	if (!moon || !moon->root || moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return -1.0f;

	outDir = Util::Moon::GetFacingAxis(moon->root->local.rotate);

	const auto prop = moon->moonMesh ? skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
	return prop ? prop->kBlendColor.alpha : 0.0f;
}

SkySync::Caster SkySync::SelectMoonCaster(const float masserAlpha, const float secundaAlpha) const
{
	const bool masserAvailable = masserAlpha >= 0.0f;
	const bool secundaAvailable = secundaAlpha >= 0.0f;

	switch (static_cast<MoonLightSource>(settings.MoonLightSource)) {
	case MoonLightSource::Masser:
		if (masserAvailable)
			return Caster::Masser;
		break;
	case MoonLightSource::Secunda:
		if (secundaAvailable)
			return Caster::Secunda;
		break;
	default:
		break;
	}

	if (!masserAvailable && !secundaAvailable)
		return Caster::None;
	if (!masserAvailable)
		return Caster::Secunda;
	if (!secundaAvailable)
		return Caster::Masser;
	return secundaAlpha > masserAlpha ? Caster::Secunda : Caster::Masser;
}

RE::NiPoint3 SkySync::GetCelestialDirection(const RE::Sky* sky, const Caster caster) const
{
	const auto idx = static_cast<size_t>(caster);
	assert(idx < std::size(rawDirections));
	if (!sky || !sky->root)
		return { 0.0f, 0.0f, 1.0f };

	RE::NiPoint3 dir = rawDirections[idx];
	if (dir.SqrLength() > 0.0f)
		dir = sky->root->world.rotate * dir;
	else if (caster == Caster::Sun)  // SetSunPosition writes only the local translate, so the world one may be stale
		dir = sky->sun && sky->sun->root ? sky->sun->root->world.translate - sky->root->world.translate : RE::NiPoint3{};
	else
		dir = Util::Moon::GetDirection(caster == Caster::Masser ? sky->masser : sky->secunda);

	if (dir.Unitize() <= FLT_EPSILON)
		return { 0.0f, 0.0f, 1.0f };
	return dir;
}

SkySync::Caster SkySync::GetMoonLightCaster(const RE::Sky* sky) const
{
	if (!sky)
		return Caster::None;

	RE::NiPoint3 unused;
	const float masserAlpha = ReadMoon(sky->masser, unused);
	const float secundaAlpha = ReadMoon(sky->secunda, unused);

	// Same moon the shadows come from, but only while it is actually visible.
	const Caster caster = SelectMoonCaster(masserAlpha, secundaAlpha);
	const float alpha = caster == Caster::Masser ? masserAlpha : caster == Caster::Secunda ? secundaAlpha :
	                                                                                         -1.0f;
	return alpha > 0.0f ? caster : Caster::None;
}

std::optional<RE::NiPoint3> SkySync::GetCelestialLightDirection(const RE::Sky* sky) const
{
	// The sun direction is only zero when Update bailed before running the shadow fader this frame
	if (!sky || !sky->root || rawDirections[static_cast<size_t>(Caster::Sun)].SqrLength() == 0.0f)
		return std::nullopt;

	RE::NiPoint3 dir = sky->root->world.rotate * shadowFader.celestialDir;
	if (dir.Unitize() <= FLT_EPSILON)
		return std::nullopt;
	return dir;
}

inline void SkySync::CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance)
{
	outDir = sun->root->local.translate;
	if (outDistance = outDir.Unitize(); outDistance < FLT_EPSILON) {
		outDir = { 0.0f, 0.0f, 1.0f };
		outDistance = SunPeakDistance;
	}
}

inline void SkySync::CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, const float time, const float sunrise, const float sunset, const float sunAngle)
{
	const float dayLength = std::max(sunset - sunrise, 1e-3f);
	const float phi = DirectX::XM_PI * ((time - sunrise) / dayLength);
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, phi);

	float tiltRadians = DirectX::XMConvertToRadians(sunAngle);
	float cosTilt, sinTilt;
	DirectX::XMScalarSinCosEst(&sinTilt, &cosTilt, tiltRadians);

	outDir = { cosPhi, -sinPhi * cosTilt, sinPhi * sinTilt };

	if (const float length = outDir.Unitize(); length < FLT_EPSILON)
		outDir = { 0.0f, 0.0f, 1.0f };

	const float elevationRatio = std::max(sinPhi, 0.0f);
	outDist = std::lerp(SunHorizonDistance, SunPeakDistance, elevationRatio);
}

inline void SkySync::SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, const float distance)
{
	const auto position = dir * distance;
	sun->root->local.translate = position;
	sun->sunGlareNode->local.translate = position;
	*gSunPosition = position;
}

void SkySync::ShadowFader::Reset()
{
	fadeTimer = 0.0f;
	transitioning = false;
	sunriseReleased = false;
	sunsetHeadingLocked = false;
	frozenHeading = 0.0f;
}

void SkySync::ShadowFader::Update(const RE::Sky* sky, RE::NiPoint3 dirs[], const Caster best, const DayPhase& phase, const float minElevRadians, const float fadeDuration, const float fadeAdvance, const bool snap)
{
	// No valid caster points straight up so shadows fall directly down.
	auto casterDir = [&](Caster c) {
		return c == Caster::None ? RE::NiPoint3{ 0.0f, 0.0f, 1.0f } : dirs[static_cast<int>(c)];
	};

	const RE::NiPoint3 celestialTargetDir = casterDir(best);
	LockSunElevation(dirs[static_cast<int>(Caster::Sun)], phase, minElevRadians);

	if (best != target) {
		target = best;
		startDir = currentDir;
		startCelestialDir = celestialDir;
		fadeTimer = 0.0f;
		transitioning = true;
	}

	const RE::NiPoint3 targetDir = casterDir(target);

	float t = 1.0f;
	if (transitioning) {
		fadeTimer = snap ? fadeDuration : std::min(fadeTimer + fadeAdvance, fadeDuration);
		t = fadeDuration > 0.0f ? fadeTimer / fadeDuration : 1.0f;
		transitioning = t < 1.0f;
	}

	if (transitioning) {
		currentDir = LerpDirection(startDir, targetDir, t);
		celestialDir = LerpDirection(startCelestialDir, celestialTargetDir, t);
	} else {
		currentDir = targetDir;
		celestialDir = celestialTargetDir;
	}

	// Fade VL out as it settles into the no-caster fallback, otherwise fade with shadow alignment.
	vlIntensityFactor = target == Caster::None ? 1.0f - t : ComputeVLFactor(currentDir, targetDir);
	SetLighting(sky, currentDir, minElevRadians);
}

void SkySync::ShadowFader::LockSunElevation(RE::NiPoint3& sunDir, const DayPhase& phase, const float minElevRadians)
{
	// Dusk: lock elevation to the minimum so the shadow can't tilt back up as the sun goes under,
	// and once dimming passes the threshold lock heading too so it stops sweeping while the VL fades.
	// Dawn: lock at the minimum until the sun naturally rises above it, then follow it.
	if (phase.sunSetting) {
		if (phase.dim <= SunsetHeadingLockThreshold) {
			if (!sunsetHeadingLocked) {
				frozenHeading = std::atan2(sunDir.y, sunDir.x);
				sunsetHeadingLocked = true;
			}
			SetDirection(sunDir, frozenHeading, minElevRadians);
		} else {
			SetElevation(sunDir, minElevRadians);
		}
	} else if (phase.sunRising) {
		if (!sunriseReleased) {
			if (DirectX::XMScalarASinEst(sunDir.z) >= minElevRadians)
				sunriseReleased = true;
			else
				SetElevation(sunDir, minElevRadians);
		}
	} else {
		sunriseReleased = false;
		sunsetHeadingLocked = false;
	}
}

void SkySync::ShadowFader::SetLighting(const RE::Sky* sky, RE::NiPoint3 dir, const float minElevRadians)
{
	if (DirectX::XMScalarASinEst(dir.z) < minElevRadians)
		SetElevation(dir, minElevRadians);

	RE::NiMatrix3& m = sky->sun->light->local.rotate;
	m.entry[0][0] = -dir.x;
	m.entry[1][0] = -dir.y;
	m.entry[2][0] = -dir.z;

	RE::NiUpdateData updateData;
	sky->sun->light->Update(updateData);
}

inline void SkySync::ShadowFader::SetDirection(RE::NiPoint3& dir, float headingRadians, float elevRadians)
{
	float sinElev, cosElev, sinHeading, cosHeading;
	DirectX::XMScalarSinCosEst(&sinElev, &cosElev, elevRadians);
	DirectX::XMScalarSinCosEst(&sinHeading, &cosHeading, headingRadians);

	dir.x = cosElev * cosHeading;
	dir.y = cosElev * sinHeading;
	dir.z = sinElev;
}

inline void SkySync::ShadowFader::SetElevation(RE::NiPoint3& dir, float elevRadians)
{
	SetDirection(dir, std::atan2(dir.y, dir.x), elevRadians);
}

float SkySync::ShadowFader::ComputeVLFactor(const RE::NiPoint3& current, const RE::NiPoint3& target)
{
	const float dot = std::clamp(current.Dot(target), -1.0f, 1.0f);
	const float angle = DirectX::XMConvertToDegrees(DirectX::XMScalarACosEst(dot));

	return std::clamp((VLFadeEndAngle - angle) / (VLFadeEndAngle - VLFadeStartAngle), 0.0f, 1.0f);
}

RE::NiPoint3 SkySync::ShadowFader::LerpDirection(const RE::NiPoint3& from, const RE::NiPoint3& to, float t)
{
	RE::NiPoint3 dir = {
		std::lerp(from.x, to.x, t),
		std::lerp(from.y, to.y, t),
		std::lerp(from.z, to.z, t)
	};
	if (dir.Unitize() <= FLT_EPSILON)
		dir = { 0.0f, 0.0f, 1.0f };
	return dir;
}

#undef I18N_KEY_PREFIX
