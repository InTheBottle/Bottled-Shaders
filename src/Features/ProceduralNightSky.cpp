#include "ProceduralNightSky.h"

#include "Globals.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/Sun.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.procedural_night_sky."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralNightSky::Settings,
	enabled,
	starIntensity,
	starTwinkle,
	shootingStarRate,
	shootingStarIntensity)

ProceduralNightSky::PerFrameData ProceduralNightSky::GetCommonBufferData() const
{
	PerFrameData data{};
	if (!loaded || !settings.enabled)
		return data;

	data.enabled = true;
	data.starIntensity = settings.starIntensity;
	data.starTwinkle = settings.starTwinkle;
	data.radianceLimit = Util::Sun::GetMainTargetRadianceLimit();

	if (shootingStar.age < shootingStar.life) {
		const float headAngle = shootingStar.age * shootingStar.speed;
		const float tailAngle = std::max(headAngle - shootingStar.trail, 0.0f);
		const float progress = shootingStar.age / shootingStar.life;
		const float fadeIn = std::clamp(progress / 0.12f, 0.0f, 1.0f);
		const float fadeOut = std::clamp((1.0f - progress) / 0.3f, 0.0f, 1.0f);
		data.shootingStarHead = shootingStar.start * std::cos(headAngle) + shootingStar.direction * std::sin(headAngle);
		data.shootingStarTail = shootingStar.start * std::cos(tailAngle) + shootingStar.direction * std::sin(tailAngle);
		data.shootingStarIntensity = settings.shootingStarIntensity * shootingStar.brightness * fadeIn * fadeOut;
		data.shootingStarWidth = shootingStar.width;
	}
	return data;
}

float ProceduralNightSky::NextShootingStarRandom()
{
	shootingStarSeed ^= shootingStarSeed << 13;
	shootingStarSeed ^= shootingStarSeed >> 17;
	shootingStarSeed ^= shootingStarSeed << 5;
	return static_cast<float>(shootingStarSeed & 0xFFFFFFu) / 16777216.0f;
}

void ProceduralNightSky::SpawnShootingStar()
{
	constexpr float kTau = 6.2831853f;
	auto range = [this](float a_min, float a_max) { return a_min + (a_max - a_min) * NextShootingStarRandom(); };

	const float azimuth = NextShootingStarRandom() * kTau;
	const float elevation = range(0.35f, 1.2f);
	const float3 start{ std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation) };
	const float3 east{ -std::sin(azimuth), std::cos(azimuth), 0.0f };
	const float3 north = start.Cross(east);

	const float heading = NextShootingStarRandom() * kTau;
	float3 direction = east * std::cos(heading) - north * (0.2f + 0.8f * std::abs(std::sin(heading)));
	direction.Normalize();

	const bool fireball = NextShootingStarRandom() < 0.08f;
	shootingStar.start = start;
	shootingStar.direction = direction;
	shootingStar.speed = range(0.45f, 1.1f);
	shootingStar.life = range(0.45f, 1.1f) * (fireball ? 1.5f : 1.0f);
	shootingStar.age = 0.0f;
	shootingStar.trail = range(0.1f, 0.28f);
	shootingStar.width = range(0.0011f, 0.002f) * (fireball ? 1.8f : 1.0f);
	shootingStar.brightness = range(4.0f, 10.0f) * (fireball ? 3.0f : 1.0f);
}

void ProceduralNightSky::UpdateShootingStar()
{
	const auto ui = globals::game::ui;
	if (ui && ui->GameIsPaused())
		return;

	const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	if (shootingStar.age < shootingStar.life) {
		shootingStar.age += deltaTime;
		return;
	}

	if (!settings.enabled || settings.shootingStarRate <= 0.0f || settings.shootingStarIntensity <= 0.0f)
		return;

	shootingStarTimer -= deltaTime;
	if (shootingStarTimer > 0.0f)
		return;

	SpawnShootingStar();
	shootingStarTimer = -std::log(std::max(1.0f - NextShootingStarRandom(), 1e-4f)) * 60.0f / settings.shootingStarRate;
}

void ProceduralNightSky::Reset()
{
	UpdateShootingStar();
}

void ProceduralNightSky::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Replaces the vanilla stars. They only show when the weather and time of day would show them."));

	if (ImGui::TreeNodeEx(T(TKEY("stars"), "Stars"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("star_intensity"), "Brightness"), &settings.starIntensity, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("star_twinkle"), "Twinkle"), &settings.starTwinkle, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("shooting_star_rate"), "Shooting Stars Per Minute"), &settings.shootingStarRate, 0.0f, 20.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shooting_star_rate_tooltip"), "How often a shooting star crosses the sky. They only show where stars do, so clouds and daylight hide them. 0 turns them off."));
		ImGui::SliderFloat(T(TKEY("shooting_star_intensity"), "Shooting Star Brightness"), &settings.shootingStarIntensity, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}
}

void ProceduralNightSky::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.starIntensity, defaults.starIntensity, 0.0f, 4.0f);
	sanitize(settings.starTwinkle, defaults.starTwinkle, 0.0f, 1.0f);
	sanitize(settings.shootingStarRate, defaults.shootingStarRate, 0.0f, 20.0f);
	sanitize(settings.shootingStarIntensity, defaults.shootingStarIntensity, 0.0f, 4.0f);
}

void ProceduralNightSky::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ProceduralNightSky::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
