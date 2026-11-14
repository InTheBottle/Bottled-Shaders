#include "CloudRelight.h"

#include <algorithm>

#include "../I18n/I18n.h"
#include "CloudShadows.h"
#include "Effects11.h"

#define I18N_KEY_PREFIX "feature.cloud_relight."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudRelight::Settings,
	enabled,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread)

namespace
{
	void ClampSettings(CloudRelight::Settings& settings)
	{
		settings.cloudRelightMix = std::clamp(settings.cloudRelightMix, 0.0f, 2.0f);
		settings.cloudOriginalMix = std::clamp(settings.cloudOriginalMix, 0.0f, 2.0f);
		settings.silverLiningMix = std::clamp(settings.silverLiningMix, 0.0f, 1.0f);
		settings.silverLiningSpread = std::clamp(settings.silverLiningSpread, -0.99f, 0.99f);
	}
}

void CloudRelight::DrawSettings()
{
	const Util::LockedSection enbLock(IsSuppressedByEffects11(),
		T(TKEY("effects11_owns_clouds"), "Effects 11 lights the clouds with its own system while its preset is active. Cloud Relight applies under Post Processing and the vanilla pipeline."));

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), (bool*)&settings.enabled);

	ImGui::SeparatorText(T(TKEY("cloud_relighting"), "Cloud Relighting"));

	ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.cloudOriginalMix, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("vanilla_mix_tooltip"), "Multiplier on the original vanilla cloud color before relighting is applied."));

	ImGui::SliderFloat(T(TKEY("relight_mix"), "Relight Mix"), &settings.cloudRelightMix, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("relight_mix_tooltip"), "Multiplier on the directional light contribution added to clouds."));

	ImGui::SeparatorText(T(TKEY("silver_lining"), "Silver Lining"));

	ImGui::SliderFloat(T(TKEY("silver_lining_accent"), "Silver Lining Accent"), &settings.silverLiningMix, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("silver_lining_accent_tooltip"), "Strength of the broad glow and sharp silver lining added to the cloud body. Does not reduce body lighting."));

	ImGui::SliderFloat(T(TKEY("silver_lining_spread"), "Silver Lining Spread"), &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("silver_lining_spread_tooltip"),
							  "Positive: silver lining spreads into thicker cloud areas.\n"
							  "Negative: silver lining is confined to thinner cloud edges."));

	if (!globals::features::cloudShadows.loaded) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", T(TKEY("cloud_shadows_required"), "Cloud Relight needs Cloud Shadows to be installed and enabled."));
	}
}

#undef I18N_KEY_PREFIX

void CloudRelight::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampSettings(settings);
}

void CloudRelight::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudRelight::RestoreDefaultSettings()
{
	settings = {};
}

bool CloudRelight::IsSuppressedByEffects11() const
{
	const auto& effects11 = globals::features::effects11;
	return effects11.loaded && effects11.IsActive() && effects11.enableEffect;
}

CloudRelight::Settings CloudRelight::GetCommonBufferData() const
{
	auto data = settings;
	ClampSettings(data);
	data.enabled = loaded && settings.enabled && !IsSuppressedByEffects11();
	return data;
}
