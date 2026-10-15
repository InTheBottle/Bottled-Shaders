#include "FeatureListRenderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "Feature.h"
#include "FeatureConstraints.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/AdvancedSettingsRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/ThemeManager.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/UI.h"
#include "Utils/VectorIcons.h"
#include "WeatherVariableRegistry.h"

namespace
{
	constexpr size_t CORE_MENU_COUNT = 2;

	bool IsCoreMenu(const std::string& menuName)
	{
		return menuName == T("menu.features.general", "General") || menuName == T("menu.features.advanced", "Advanced");
	}

	constexpr std::string_view kNoSettingsSectionKey = "No Settings";
	constexpr std::string_view kPinnedSectionKey = "__Pinned";

	void ExpandSectionOf(const std::string& shortName, std::map<std::string, bool>& categoryExpansionStates)
	{
		for (Feature* feat : Feature::GetFeatureList()) {
			if (feat->GetShortName() == shortName) {
				categoryExpansionStates[std::string(feat->HasSettings() ? feat->GetCategory() : kNoSettingsSectionKey)] = true;
				return;
			}
		}
	}

	// Color for the [ALPHA]/[BETA] stage marker. Alpha (less stable) reads as an error,
	// Beta as a warning.
	ImVec4 StageTagColor(Feature::ReleaseStage stage)
	{
		const auto& statusPalette = globals::menu->GetTheme().StatusPalette;
		return stage == Feature::ReleaseStage::Alpha ? statusPalette.Error : statusPalette.Warning;
	}

	/**
	 * @brief Determines if the left feature panel should be visible based on auto-hide settings and mouse position
	 * @return true if panel should be visible, false if it should be hidden
	 */
	bool ShouldShowLeftPanel()
	{
		bool autoHideEnabled = globals::menu->GetSettings().AutoHideFeatureList;
		static bool leftPanelVisible = true;
		static float hoverStartTime = 0.0f;
		static bool wasHovering = false;

		if (!autoHideEnabled) {
			leftPanelVisible = true;
			return true;
		}

		// Get mouse position and window bounds
		ImVec2 mousePos = ImGui::GetMousePos();
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		float currentTime = static_cast<float>(ImGui::GetTime());

		// Use constants for auto-hide behavior
		const float activationZoneWidth = ThemeManager::Constants::AUTOHIDE_ACTIVATION_ZONE_WIDTH;
		const float expandDelay = ThemeManager::Constants::AUTOHIDE_EXPAND_DELAY;
		const float panelWidth = windowSize.x * ThemeManager::Constants::AUTOHIDE_PANEL_WIDTH_RATIO;

		// Calculate relative X position
		const float relativeX = mousePos.x - windowPos.x;

		// For activation: only check if mouse is at left edge (allow any Y position for easier triggering)
		// Prevent negative X from triggering, but don't restrict Y-axis for activation
		bool mouseInActivationZone = relativeX >= 0.0f && relativeX < activationZoneWidth;

		// For staying visible: check both X and Y to ensure mouse is actually over the panel area
		const bool mouseOverPanelX = relativeX >= 0.0f && relativeX < panelWidth;
		const bool mouseOverPanelY = mousePos.y >= windowPos.y && mousePos.y <= (windowPos.y + windowSize.y);
		bool mouseOverPanel = leftPanelVisible && mouseOverPanelX && mouseOverPanelY;

		// Track hover start time
		if (mouseInActivationZone && !wasHovering) {
			hoverStartTime = currentTime;
			wasHovering = true;
		} else if (!mouseInActivationZone) {
			wasHovering = false;
		}

		// Expand only after delay has elapsed
		bool shouldExpand = mouseInActivationZone && (currentTime - hoverStartTime >= expandDelay);

		// Update visibility: expand with delay, or stay visible while mouse is over panel
		if (shouldExpand || mouseOverPanel) {
			leftPanelVisible = true;
		} else if (!mouseOverPanel && !mouseInActivationZone) {
			leftPanelVisible = false;
		}

		return leftPanelVisible;
	}

	void SeparatorTextWithFont(const char* text, Menu::FontRole role)
	{
		MenuFonts::FontRoleGuard guard(role);
		ImGui::SeparatorText(text);
	}

	struct TitleMetrics
	{
		float width = 0.0f;
		float height = 0.0f;
	};

	TitleMetrics DrawPageTitle(const std::string& title, const std::string& version, const std::string& stageTag = "", ImVec4 stageColor = {})
	{
		auto& themeSettings = globals::menu->GetTheme();
		auto& palette = themeSettings.Palette;
		auto& featureHeading = themeSettings.FeatureHeading;

		// Sanitize and clamp to UI slider range to prevent malformed theme JSON from destabilizing layout
		float titleScale = featureHeading.FeatureTitleScale;
		if (!std::isfinite(titleScale)) {
			titleScale = ThemeManager::Constants::DEFAULT_FEATURE_TITLE_SCALE;
		}
		titleScale = std::clamp(titleScale, 1.0f, 3.0f);

		ImVec2 startPos = ImGui::GetCursorScreenPos();

		ImVec2 titleSize;
		{
			MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
			titleSize = ImGui::CalcTextSize(title.c_str());
			titleSize.x *= titleScale;
			titleSize.y *= titleScale;

			ImGui::SetWindowFontScale(titleScale);
			ImGui::TextUnformatted(title.c_str());
			ImGui::SetWindowFontScale(1.0f);
		}

		float annotationX = startPos.x + titleSize.x + ImGui::GetStyle().ItemSpacing.x;
		float rightEdge = startPos.x + titleSize.x;

		auto drawAnnotation = [&](const std::string& text, const ImVec4& color) {
			ImVec2 size;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				size = ImGui::CalcTextSize(text.c_str());
				size.x *= titleScale;
				size.y *= titleScale;
			}

			ImGui::SetCursorScreenPos(ImVec2(annotationX, startPos.y + titleSize.y - size.y));
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				ImGui::TextColored(color, "%s", text.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			rightEdge = annotationX + size.x;
			annotationX += size.x + ImGui::GetStyle().ItemSpacing.x;
		};

		if (!stageTag.empty())
			drawAnnotation(stageTag, stageColor);

		if (!version.empty()) {
			std::string formattedVersion = version;
			std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');
			ImVec4 versionColor = palette.Text;
			versionColor.w *= ThemeManager::Constants::VERSION_TEXT_OPACITY;
			drawAnnotation("v" + formattedVersion, versionColor);
		}

		if (!stageTag.empty() || !version.empty())
			ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + titleSize.y + ImGui::GetStyle().ItemSpacing.y * 0.25f));

		return { rightEdge - startPos.x, titleSize.y };
	}

	void DrawPageDescription(const std::string& description)
	{
		if (description.empty())
			return;
		MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
		ImVec4 descColor = globals::menu->GetTheme().Palette.Text;
		descColor.w *= 0.7f;
		ImGui::PushStyleColor(ImGuiCol_Text, descColor);
		ImGui::TextWrapped("%s", description.c_str());
		ImGui::PopStyleColor();
	}

	// ---------------------------------------------------------------------------
	// Persistent state for the reactive constraint warning popup.
	// DrawMenuVisitor is reconstructed every frame (it's a temporary passed to
	// std::visit), so member state is lost immediately.  These file-scope
	// variables survive across frames so the popup can actually render.
	// ---------------------------------------------------------------------------

	// Set of constraint keys we have already "seen" (and therefore warned about
	// or suppressed).  Keyed as "featureShortName|settingPath".
	std::unordered_set<std::string> g_knownConstraintKeys;
	bool g_knownConstraintKeysInitialised = false;

	// Pending popup state: non-empty when we have new constraints to show.
	bool g_reactiveWarningShow = false;
	std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> g_reactiveWarningConstraints;

	// "Don't show again" checkbox state inside the modal (reset each time popup opens).
	bool g_dontShowAgainCheckbox = false;

	// ---------------------------------------------------------------------------
	// Selection is tracked by a stable key rather than by list index: the menu list
	// is rebuilt every frame, so collapsing a category above the selection removes
	// entries and shifts every later index, silently moving the right pane to a
	// different feature. The index is re-derived from this key each frame.
	// ---------------------------------------------------------------------------
	std::string g_selectedEntryKey;

	std::string FeatureEntryKey(const std::string& shortName)
	{
		return "F:" + shortName;
	}

	std::string BuiltInEntryKey(const std::string& menuName)
	{
		return "B:" + menuName;
	}

	/** @brief Stable identity for a menu entry; empty for headers and labels, which are not selectable. */
	std::string EntryKey(const FeatureListRenderer::MenuFuncInfo& entry)
	{
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&entry))
			return BuiltInEntryKey(menu->name);
		if (const auto* feat = std::get_if<Feature*>(&entry))
			return FeatureEntryKey((*feat)->GetShortName());
		if (const auto* pinned = std::get_if<FeatureListRenderer::PinnedFeature>(&entry))
			return FeatureEntryKey(pinned->feature->GetShortName());
		return {};
	}

	std::string DescribeLocation(const FeatureListRenderer::MenuFuncInfo& entry)
	{
		Feature* feat = nullptr;
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&entry))
			return menu->name;
		if (const auto* listed = std::get_if<Feature*>(&entry))
			feat = *listed;
		else if (const auto* pinned = std::get_if<FeatureListRenderer::PinnedFeature>(&entry))
			feat = pinned->feature;
		if (!feat)
			return {};
		return std::format("{} / {}", Feature::TranslateCategory(feat->GetCategory()), feat->GetDisplayName());
	}

	ImGuiSelectableFlags NavigationSelectableFlags(bool selectOnNav)
	{
		return ImGuiSelectableFlags_SpanAllColumns | (selectOnNav ? ImGuiSelectableFlags_SelectOnNav : ImGuiSelectableFlags_None);
	}

	struct SearchHit
	{
		bool nameMatch = false;
		std::string settingLabel;
		int extraSettings = 0;
	};

	struct SettingLabel
	{
		Feature* feature = nullptr;
		std::string text;
		std::string lowered;
	};

	constexpr std::pair<std::string_view, std::string_view> kTranslationPrefixAliases[] = {
		{ "expheightfog", "ExponentialHeightFog" },
		{ "ibl", "ImageBasedLighting" },
		{ "perfoverlay", "PerformanceOverlay" },
		{ "sss", "SubsurfaceScattering" },
	};

	std::string ToLowerAscii(std::string_view text)
	{
		std::string result(text);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	std::string NormalizePrefix(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		for (const unsigned char c : text) {
			if (c != '_')
				result += static_cast<char>(std::tolower(c));
		}
		return result;
	}

	bool IsSearchableLabel(std::string_view key, std::string_view text)
	{
		if (text.empty() || text.size() > ThemeManager::Constants::MAX_SEARCHABLE_LABEL_LENGTH || text.find_first_of("\n%{") != std::string_view::npos)
			return false;
		const std::string_view leaf = key.substr(key.rfind('.') + 1);
		constexpr std::string_view kProseLeaves[] = { "tooltip", "_tip", "desc", "description", "_help", "_info", "_note", "warning", "summary" };
		return leaf != "tip" && std::ranges::none_of(kProseLeaves, [&](std::string_view suffix) { return leaf.ends_with(suffix); });
	}

	const std::vector<SettingLabel>& GetSettingLabelIndex(const std::string& locale)
	{
		static std::vector<SettingLabel> index;
		static std::string indexedLocale;
		static bool built = false;
		if (built && locale == indexedLocale)
			return index;
		built = true;
		indexedLocale = locale;
		index.clear();

		std::unordered_map<std::string, Feature*> featuresByPrefix;
		for (Feature* feat : Feature::GetFeatureList())
			featuresByPrefix.emplace(NormalizePrefix(feat->GetShortName()), feat);
		for (const auto& [alias, shortName] : kTranslationPrefixAliases) {
			for (Feature* feat : Feature::GetFeatureList()) {
				if (feat->GetShortName() == shortName)
					featuresByPrefix.emplace(std::string(alias), feat);
			}
		}

		constexpr std::string_view kFeaturePrefix = "feature.";
		std::unordered_set<std::string> seen;
		for (auto& [key, text] : I18n::GetSingleton()->CollectStrings(kFeaturePrefix)) {
			const std::string_view rest = std::string_view(key).substr(kFeaturePrefix.size());
			const size_t dot = rest.find('.');
			if (dot == std::string_view::npos || !IsSearchableLabel(key, text))
				continue;
			const auto it = featuresByPrefix.find(NormalizePrefix(rest.substr(0, dot)));
			if (it == featuresByPrefix.end())
				continue;
			std::string lowered = ToLowerAscii(text);
			if (!seen.insert(it->second->GetShortName() + '|' + lowered).second)
				continue;
			index.push_back({ it->second, std::move(text), std::move(lowered) });
		}

		std::ranges::sort(index, [](const SettingLabel& a, const SettingLabel& b) {
			return a.text.size() != b.text.size() ? a.text.size() < b.text.size() : a.text < b.text;
		});
		return index;
	}

	const std::unordered_map<Feature*, SearchHit>& GetSearchHits(const std::string& query)
	{
		static std::unordered_map<Feature*, SearchHit> hits;
		static std::string cachedQuery;
		static std::string cachedLocale;

		const std::string locale = I18n::GetSingleton()->GetCurrentLocale();
		if (query == cachedQuery && locale == cachedLocale)
			return hits;
		cachedQuery = query;
		cachedLocale = locale;
		hits.clear();
		if (query.empty())
			return hits;

		const std::string needle = ToLowerAscii(query);
		for (Feature* feat : Feature::GetFeatureList()) {
			if (Util::FeatureMatchesSearch(feat, query) ||
				ToLowerAscii(feat->GetDisplayName()).find(needle) != std::string::npos ||
				ToLowerAscii(Feature::TranslateCategory(feat->GetCategory())).find(needle) != std::string::npos)
				hits[feat].nameMatch = true;
		}

		if (needle.size() >= ThemeManager::Constants::MIN_SETTING_SEARCH_LENGTH) {
			for (const auto& label : GetSettingLabelIndex(locale)) {
				if (label.lowered.find(needle) == std::string::npos)
					continue;
				auto& hit = hits[label.feature];
				if (hit.settingLabel.empty())
					hit.settingLabel = label.text;
				else
					++hit.extraSettings;
			}
		}
		return hits;
	}

	bool WasItemEditedThisFrame()
	{
		return ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame;
	}
}

void FeatureListRenderer::RenderFeatureList(
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings,
	float reservedBottomHeight)
{
	ImGui::BeginChild("Menus Table", ImVec2(0, -reservedBottomHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	if (!pendingFeatureSelection.empty()) {
		ExpandSectionOf(pendingFeatureSelection, categoryExpansionStates);
		featureSearch.clear();
	}

	auto menuList = BuildMenuList(featureSearch, categoryExpansionStates, drawGeneralSettings, drawAdvancedSettings);

	ResolveSelectionFromKey(menuList, selectedMenu);

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);

	const bool narrow = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * ThemeManager::Constants::NARROW_LAYOUT_WIDTH_EM;
	if (narrow) {
		RenderNavigatorBar(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates);
		RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
	} else {
		const bool leftPanelVisible = ShouldShowLeftPanel();
		if (!leftPanelVisible)
			focusSearch = false;
		if (ImGui::BeginTable("Menus Table", leftPanelVisible ? 2 : 1, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
			if (leftPanelVisible) {
				ImGui::TableSetupColumn("##ListOfMenus", 0, ThemeManager::Constants::FEATURE_LIST_WIDTH_WEIGHT);
				ImGui::TableSetupColumn("##MenuConfig", 0, ThemeManager::Constants::FEATURE_PAGE_WIDTH_WEIGHT);
				ImGui::TableNextColumn();
				RenderNavigation(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, false);
			} else {
				ImGui::TableSetupColumn("##MenuConfig", 0, 1);
			}
			ImGui::TableNextColumn();
			RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
			ImGui::EndTable();
		}
	}

	ImGui::EndChild();
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	const std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	const bool searching = !featureSearch.empty();
	if (searching) {
		const auto& hits = GetSearchHits(featureSearch);
		std::erase_if(sortedFeatureList, [&hits](Feature* feat) { return !hits.contains(feat); });
	}

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ T("menu.features.general", "General"), drawGeneralSettings, T("menu.features.general_description", "Shader compilation, keybindings, language and the look of this menu.") },
		BuiltInMenu{ T("menu.features.advanced", "Advanced"), drawAdvancedSettings, T("menu.features.advanced_description", "Developer tools, logging, profiling and utility features.") }
	};  // NOTE: The menu list is rebuilt every frame, so category expansion states
	// persist correctly. This is acceptable since the list is small and built
	// infrequently, but could be optimized if performance becomes an issue.

	if (!searching) {
		std::vector<Feature*> pinnedFeatures;
		for (const auto& shortName : globals::menu->GetPinnedFeatures()) {
			const auto it = std::ranges::find_if(featureList, [&shortName](Feature* feat) { return feat->GetShortName() == shortName; });
			if (it != featureList.end() && (*it)->IsInMenu() && (*it)->loaded && (*it)->GetCategory() != FeatureCategories::kUtility)
				pinnedFeatures.push_back(*it);
		}
		if (!pinnedFeatures.empty()) {
			const std::string key(kPinnedSectionKey);
			const bool expanded = categoryExpansionStates.try_emplace(key, true).first->second;
			menuList.push_back(CategoryHeader{ key, T("menu.features.pinned", "Pinned"), static_cast<int>(pinnedFeatures.size()), expanded });
			if (expanded) {
				for (Feature* feat : pinnedFeatures)
					menuList.push_back(PinnedFeature{ feat });
			}
		}
	}

	// Group features by category. Utility features are deliberately absent: they are
	// rendered as sub-tabs of the Advanced page instead of as their own list category.
	std::map<std::string, std::vector<Feature*>, std::less<>> categorizedFeatures;
	std::vector<Feature*> settingsFreeFeatures;
	for (Feature* feat : sortedFeatureList) {
		if (!feat->IsInMenu() || !feat->loaded || feat->GetCategory() == FeatureCategories::kUtility)
			continue;
		if (feat->HasSettings())
			categorizedFeatures[std::string(feat->GetCategory())].push_back(feat);
		else
			settingsFreeFeatures.push_back(feat);
	}

	auto appendSection = [&](const std::string& key, std::string label, const std::vector<Feature*>& features, bool expandedByDefault) {
		if (features.empty())
			return;
		const bool expanded = searching || categoryExpansionStates.try_emplace(key, expandedByDefault).first->second;
		menuList.push_back(CategoryHeader{ key, std::move(label), static_cast<int>(features.size()), expanded });
		if (expanded)
			std::ranges::copy(features, std::back_inserter(menuList));
	};

	for (const auto category : FeatureCategories::kOrdered) {
		if (auto it = categorizedFeatures.find(category); it != categorizedFeatures.end()) {
			appendSection(it->first, Feature::TranslateCategory(category), it->second, true);
			categorizedFeatures.erase(it);
		}
	}

	for (const auto& [category, features] : categorizedFeatures)
		appendSection(category, Feature::TranslateCategory(category), features, true);

	if (searching) {
		auto utilityMatches = GetUtilityFeatures();
		const auto& hits = GetSearchHits(featureSearch);
		std::erase_if(utilityMatches, [&hits](Feature* feat) { return !hits.contains(feat); });
		appendSection(std::string(FeatureCategories::kUtility), Feature::TranslateCategory(FeatureCategories::kUtility), utilityMatches, true);
	}

	appendSection(std::string(kNoSettingsSectionKey), T("menu.features.no_settings_section", "No Settings"), settingsFreeFeatures, false);

	auto unloadedFeatures = sortedFeatureList | std::ranges::views::filter([](Feature* feat) {
		return !feat->loaded && feat->IsInMenu() && !feat->IsHiddenUnreleased() && feat->GetCategory() != FeatureCategories::kUtility && (!FeatureIssues::IsObsoleteFeature(feat->GetShortName()) || globals::state->IsDeveloperMode());
	});
	if (std::ranges::distance(unloadedFeatures) != 0) {
		menuList.push_back(T("menu.features.unloaded_features", "Unloaded Features"));
		std::ranges::copy(unloadedFeatures, std::back_inserter(menuList));
	}
	// Add top section for feature issues (rejected features, obsolete info, etc.)
	if (FeatureIssues::HasFeatureIssues()) {
		menuList.insert(menuList.begin(), BuiltInMenu{ T("menu.features.feature_issues", "Feature Issues"), []() {
														  FeatureIssues::DrawFeatureIssuesUI();
													  } });
	}

	return menuList;
}

void FeatureListRenderer::ResolveSelectionFromKey(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	// Seed the key from whatever index the caller starts with (first frame, or a restored layout).
	if (g_selectedEntryKey.empty()) {
		if (selectedMenu < menuList.size())
			g_selectedEntryKey = EntryKey(menuList[selectedMenu]);
		return;
	}

	for (size_t i = 0; i < menuList.size(); ++i) {
		if (EntryKey(menuList[i]) == g_selectedEntryKey) {
			selectedMenu = i;
			return;
		}
	}

	// The selected entry is gone: its category was collapsed, or the search filtered it out.
	// Falling back to General is deterministic; keeping the stale index would render whichever
	// unrelated entry has shifted into that slot.
	const std::string generalKey = BuiltInEntryKey(T("menu.features.general", "General"));
	for (size_t i = 0; i < menuList.size(); ++i) {
		if (EntryKey(menuList[i]) == generalKey) {
			selectedMenu = i;
			return;
		}
	}

	if (selectedMenu >= menuList.size())
		selectedMenu = menuList.empty() ? 0 : menuList.size() - 1;
}

void FeatureListRenderer::HandlePendingFeatureSelection(
	std::string& pendingFeatureSelection,
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	if (pendingFeatureSelection.empty())
		return;

	for (size_t i = 0; i < menuList.size(); ++i) {
		if (std::holds_alternative<Feature*>(menuList[i])) {
			Feature* feature = std::get<Feature*>(menuList[i]);
			if (feature->GetShortName() == pendingFeatureSelection) {
				selectedMenu = i;
				g_selectedEntryKey = FeatureEntryKey(pendingFeatureSelection);
				logger::info("Navigated to {} feature menu", pendingFeatureSelection);
				pendingFeatureSelection.clear();
				return;
			}
		}
	}

	// Utility features are not list entries; they live as sub-tabs of the Advanced page.
	if (IsUtilityFeature(pendingFeatureSelection)) {
		const std::string advancedName = T("menu.features.advanced", "Advanced");
		for (size_t i = 0; i < menuList.size(); ++i) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i]) && std::get<BuiltInMenu>(menuList[i]).name == advancedName) {
				selectedMenu = i;
				g_selectedEntryKey = EntryKey(menuList[i]);
				AdvancedSettingsRenderer::RequestUtilityTab(pendingFeatureSelection);
				logger::info("Navigated to {} utility tab under Advanced", pendingFeatureSelection);
				break;
			}
		}
	}

	pendingFeatureSelection.clear();  // Clear after processing
}

bool FeatureListRenderer::IsUtilityFeature(const std::string& shortName)
{
	const auto& featureList = Feature::GetFeatureList();
	return std::ranges::any_of(featureList, [&shortName](Feature* feat) {
		return feat->GetShortName() == shortName && feat->GetCategory() == FeatureCategories::kUtility;
	});
}

std::vector<Feature*> FeatureListRenderer::GetUtilityFeatures()
{
	std::vector<Feature*> utilityFeatures;
	for (Feature* feat : Feature::GetFeatureList()) {
		if (!feat->IsInMenu() || feat->GetCategory() != FeatureCategories::kUtility)
			continue;
		if (feat->IsHiddenUnreleased())
			continue;
		if (!feat->loaded && FeatureIssues::IsObsoleteFeature(feat->GetShortName()) && !globals::state->IsDeveloperMode())
			continue;
		utilityFeatures.push_back(feat);
	}

	std::ranges::sort(utilityFeatures, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});
	return utilityFeatures;
}

void FeatureListRenderer::RenderFeaturePage(Feature* feat)
{
	// The Advanced page owns no pending-selection state, so navigation requests raised from a
	// utility page are routed back through the Menu, which re-enters HandlePendingFeatureSelection.
	static std::string pendingSelection;
	DrawMenuVisitor visitor{ pendingSelection };
	visitor(feat);
	if (!pendingSelection.empty()) {
		globals::menu->SelectFeatureMenu(pendingSelection);
		pendingSelection.clear();
	}
}

bool FeatureListRenderer::RenderNavigation(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	bool inPopup)
{
	bool selectionMade = false;

	if (Util::DrawFeatureSearchBar(featureSearch, 0.0f, focusSearch) && !featureSearch.empty()) {
		const auto first = std::ranges::find_if(menuList, [](const MenuFuncInfo& entry) { return std::holds_alternative<Feature*>(entry); });
		if (first != menuList.end()) {
			Feature* feat = std::get<Feature*>(*first);
			if (feat->GetCategory() == FeatureCategories::kUtility) {
				globals::menu->SelectFeatureMenu(feat->GetShortName());
			} else {
				selectedMenu = static_cast<size_t>(std::distance(menuList.begin(), first));
				g_selectedEntryKey = EntryKey(*first);
			}
			selectionMade = true;
		}
	}
	focusSearch = false;

	const std::string currentKey = selectedMenu < menuList.size() ? EntryKey(menuList[selectedMenu]) : std::string();
	const bool selectOnNav = !inPopup;
	const auto& style = ImGui::GetStyle();

	float footerHeight = 0.0f;
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		footerHeight = style.ItemSpacing.y * 2.0f + 1.0f + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(CORE_MENU_COUNT);
	}

	auto isCoreEntry = [](const MenuFuncInfo& entry) {
		const auto* menu = std::get_if<BuiltInMenu>(&entry);
		return menu && IsCoreMenu(menu->name);
	};

	if (ImGui::BeginChild("##FeatureNavList", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_NavFlattened)) {
		bool anyFeature = false;
		for (size_t i = 0; i < menuList.size(); i++) {
			if (isCoreEntry(menuList[i]))
				continue;
			anyFeature = anyFeature || std::holds_alternative<Feature*>(menuList[i]);
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, currentKey, selectOnNav, selectionMade }, menuList[i]);
		}

		if (!featureSearch.empty() && !anyFeature) {
			ImGui::Spacing();
			ImGui::PushTextWrapPos(0.0f);
			const auto noResults = I18n::GetSingleton()->Format("menu.features.no_search_results", { { "search", featureSearch } }, "Nothing matches \"{search}\".");
			Util::TextUnformattedDisabled(noResults.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	ImGui::EndChild();

	ImGui::Separator();
	for (size_t i = 0; i < menuList.size(); i++) {
		if (isCoreEntry(menuList[i]))
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, currentKey, selectOnNav, selectionMade }, menuList[i]);
	}

	return selectionMade;
}

void FeatureListRenderer::RenderNavigatorBar(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::map<std::string, bool>& categoryExpansionStates)
{
	constexpr const char* kPopupId = "##FeatureNavigator";
	static bool navigatorOpenLastFrame = false;
	static bool pressedWhileOpen = false;
	const ImVec2 barStart = ImGui::GetCursorScreenPos();
	const float barWidth = ImGui::GetContentRegionAvail().x;

	bool open = focusSearch;
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		const std::string location = selectedMenu < menuList.size() ? DescribeLocation(menuList[selectedMenu]) : std::string();
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		const bool clicked = Util::Icons::Button(std::format("{}###OpenFeatureNavigator", location).c_str(), Util::Icons::Kind::Menu, ImVec2(-FLT_MIN, 0.0f));
		ImGui::PopStyleVar();
		if (ImGui::IsItemActivated())
			pressedWhileOpen = navigatorOpenLastFrame;
		if (clicked && !pressedWhileOpen)
			open = true;
	}
	const float barBottom = ImGui::GetItemRectMax().y;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T("menu.features.browse_tooltip", "Browse and search features (Ctrl+F)"));

	const ImGuiID popupId = ImGui::GetID(kPopupId);
	if (open && !ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
		ImGui::OpenPopup(popupId);

	ImGui::SetNextWindowPos(ImVec2(barStart.x, barBottom + ImGui::GetStyle().ItemSpacing.y));
	ImGui::SetNextWindowSize(ImVec2(barWidth, ImGui::GetWindowHeight() * ThemeManager::Constants::NAVIGATOR_POPUP_HEIGHT_RATIO));
	if (ImGui::BeginPopupEx(popupId, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
		if (RenderNavigation(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, true))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	navigatorOpenLastFrame = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
	focusSearch = false;

	ImGui::Spacing();
}

void FeatureListRenderer::RenderRightColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t selectedMenu,
	std::string& pendingFeatureSelection)
{
	if (selectedMenu < menuList.size()) {
		std::visit(DrawMenuVisitor{ pendingFeatureSelection }, menuList[selectedMenu]);
	} else {
		ImGui::TextDisabled("%s", T("menu.features.select_item_left", "Please select an item on the left."));
	}
}

void FeatureListRenderer::ListMenuVisitor::Select(const std::string& entryKey)
{
	selectedMenuRef = listId;
	g_selectedEntryKey = entryKey;
	selectionMade = true;
}

void FeatureListRenderer::ListMenuVisitor::operator()(const BuiltInMenu& menu)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const std::string key = BuiltInEntryKey(menu.name);
	// Use error color for Feature Issues menu item
	const bool isFeatureIssues = (menu.name == T("menu.features.feature_issues", "Feature Issues"));
	if (isFeatureIssues)
		ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Error);

	if (ImGui::Selectable(fmt::format(" {} ", menu.name).c_str(), key == currentKey, NavigationSelectableFlags(selectOnNav)))
		Select(key);

	if (isFeatureIssues)
		ImGui::PopStyleColor();
}

void FeatureListRenderer::ListMenuVisitor::operator()(const std::string& label)
{
	// Static list headings share the category header's font, metrics and alignment.
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
	Util::DrawCategoryLabel(label.c_str());
}

void FeatureListRenderer::ListMenuVisitor::operator()(const CategoryHeader& header)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
	bool isExpanded = header.expanded;
	if (Util::DrawCategoryHeader(header.name.c_str(), header.label.c_str(), isExpanded, header.count))
		categoryExpansionStates[header.name] = isExpanded;
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	DrawFeatureRow(feat, false);
}

void FeatureListRenderer::ListMenuVisitor::operator()(const PinnedFeature& pinned)
{
	DrawFeatureRow(pinned.feature, true);
}

void FeatureListRenderer::ListMenuVisitor::DrawFeatureRow(Feature* feat, bool inPinnedSection)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	auto& themeSettings = globals::menu->GetSettings().Theme;

	ImVec4 textColor;

	// Determine the text color based on the state
	if (isDisabled) {
		textColor = themeSettings.StatusPalette.Disable;
	} else if (isLoaded) {
		textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	} else if (hasFailedMessage) {
		textColor = feat->version.empty() ? themeSettings.StatusPalette.Disable : themeSettings.StatusPalette.Error;
	} else {
		// Installed but not loaded means the feature is only pending a restart (green),
		// otherwise it is simply missing (grey).
		textColor = feat->installed ? themeSettings.StatusPalette.RestartNeeded : themeSettings.StatusPalette.Disable;
	}

	const std::string key = FeatureEntryKey(featureName);
	ImGui::PushID(inPinnedSection ? "Pinned" : "Listed");
	ImGui::PushID(featureName.c_str());

	const bool isUtility = feat->GetCategory() == FeatureCategories::kUtility;
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	if (ImGui::Selectable(fmt::format(" {} ", feat->GetDisplayName()).c_str(), key == currentKey, NavigationSelectableFlags(selectOnNav && !isUtility))) {
		if (isUtility) {
			globals::menu->SelectFeatureMenu(featureName);
			selectionMade = true;
		} else {
			Select(key);
		}
	}
	ImGui::PopStyleColor();

	if (isLoaded && !isUtility && ImGui::BeginPopupContextItem("##FeatureRowMenu")) {
		const bool pinned = globals::menu->IsFeaturePinned(featureName);
		if (ImGui::MenuItem(pinned ? T("menu.features.unpin", "Unpin") : T("menu.features.pin", "Pin to Top")))
			globals::menu->SetFeaturePinned(featureName, !pinned);
		ImGui::EndPopup();
	}

	// Display the stage marker behind the name, regardless of loaded state
	if (const auto stage = feat->GetReleaseStage(); stage != Feature::ReleaseStage::Release) {
		ImGui::SameLine();
		ImGui::TextColored(StageTagColor(stage), "%s", Feature::GetReleaseStageTag(stage).c_str());
	}

	const auto& search = globals::menu->featureSearch;
	if (!inPinnedSection && !search.empty()) {
		const auto& hits = GetSearchHits(search);
		if (const auto it = hits.find(feat); it != hits.end() && !it->second.nameMatch && !it->second.settingLabel.empty()) {
			MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
			const std::string hint = it->second.extraSettings > 0 ? std::format("{} (+{})", it->second.settingLabel, it->second.extraSettings) : it->second.settingLabel;
			const float indent = ImGui::GetFontSize();
			ImGui::Indent(indent);
			Util::TextUnformattedDisabled(hint.c_str());
			ImGui::Unindent(indent);
		}
	}

	ImGui::PopID();
	ImGui::PopID();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const BuiltInMenu& menu)
{
	const bool editedBefore = WasItemEditedThisFrame();
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		if (!menu.description.empty()) {
			DrawPageTitle(menu.name, "");
			DrawPageDescription(menu.description);
			ImGui::Separator();
		}
		menu.func();
	}
	ImGui::EndChild();
	if (!editedBefore && WasItemEditedThisFrame() && !globals::menu->AreSettingEditsIgnored())
		globals::menu->MarkUnsavedChanges();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const std::string&)
{
	// std::unreachable() from c++23
	// you are not supposed to have selected a label!
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const CategoryHeader&)
{
	// Category headers are not selectable in the right panel
	ImGui::TextDisabled("%s", T("menu.features.select_feature_left", "Please select a feature from the left."));
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const PinnedFeature& pinned)
{
	(*this)(pinned.feature);
}

void FeatureListRenderer::DrawMenuVisitor::operator()(Feature* feat)
{
	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();

	globals::menu->BeginFeatureEditTracking(feat);
	const bool editedBefore = WasItemEditedThisFrame();
	featureChanged = false;

	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		// Compute scene-controlled state once for both header and settings
		auto* sceneManager = globals::sceneSettingsManager;
		bool sceneControlled = sceneManager->HasActiveSettingsForFeature(featureName) && !sceneManager->IsFeaturePaused(featureName);

		// Render feature header with integrated action buttons
		RenderFeatureHeader(feat, isDisabled, isLoaded, sceneControlled);

		// Render feature settings content
		RenderFeatureSettings(feat, isDisabled, isLoaded, hasFailedMessage, sceneControlled);
	}
	ImGui::EndChild();

	globals::menu->EndFeatureEditTracking(feat, featureChanged || (!editedBefore && WasItemEditedThisFrame()));
	globals::menu->IgnoreSettingEditsThisFrame();
	// Render reactive constraint warning outside the child window so it can appear as a top-level popup
	RenderReactiveConstraintWarningDialog();
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded, bool sceneControlled)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;
	const auto featureName = feat->GetShortName();
	const auto& style = ImGui::GetStyle();

	const float buttonPadding = ThemeManager::Constants::BUTTON_PADDING;
	const float frameHeight = ImGui::GetFrameHeight();

	const char* overrideButtonText = T("menu.features.apply_override", "Apply Override");
	const char* restoreButtonText = T("menu.features.restore_defaults", "Restore Defaults");
	const char* bootLabel = T("menu.features.load_at_boot", "Load at Boot");
	const float bootToggleWidth = frameHeight * 1.6f;
	const float overrideButtonWidth = ImGui::CalcTextSize(overrideButtonText).x + buttonPadding;
	const float restoreButtonWidth = ImGui::CalcTextSize(restoreButtonText).x + ImGui::GetFontSize() + style.ItemInnerSpacing.x + buttonPadding;

	auto overrideManager = SettingsOverrideManager::GetSingleton();
	const bool hasOverrides = overrideManager && overrideManager->HasFeatureOverrides(featureName);
	const bool canOverride = !isDisabled && isLoaded && hasOverrides;
	const bool canRestoreDefaults = !isDisabled && isLoaded && feat->HasSettings();
	const bool canPin = isLoaded && feat->IsInMenu() && feat->GetCategory() != FeatureCategories::kUtility;

	float actionsWidth = ImGui::CalcTextSize(bootLabel).x + style.ItemInnerSpacing.x + bootToggleWidth;
	if (canPin)
		actionsWidth += frameHeight + style.ItemSpacing.x;
	if (canOverride)
		actionsWidth += overrideButtonWidth + style.ItemSpacing.x;
	if (canRestoreDefaults)
		actionsWidth += restoreButtonWidth + style.ItemSpacing.x;

	auto drawActions = [&]() {
		bool first = true;
		auto nextItem = [&first](float width) {
			if (!first) {
				ImGui::SameLine();
				if (ImGui::GetContentRegionAvail().x < width)
					ImGui::NewLine();
			}
			first = false;
		};

		if (canPin) {
			nextItem(frameHeight);
			const bool pinned = globals::menu->IsFeaturePinned(featureName);
			const ImU32 starColor = ImGui::GetColorU32(pinned ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
			if (Util::Icons::IconButton("##PinFeature", pinned ? Util::Icons::Kind::StarFilled : Util::Icons::Kind::Star, starColor))
				globals::menu->SetFeaturePinned(featureName, !pinned);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", pinned ? T("menu.features.unpin_tooltip", "Remove from the Pinned section.") :
				                           T("menu.features.pin_tooltip", "Pin to the top of the feature list."));
			}
		}

		// Apply Override button (when feature has available overrides)
		if (canOverride) {
			nextItem(overrideButtonWidth);
			{
				auto _disabled = Util::DisableGuard(sceneControlled);
				if (ImGui::Button(overrideButtonText, { overrideButtonWidth, 0 })) {
					if (feat->ReapplyOverrideSettings()) {
						featureChanged = true;
						logger::info("Successfully reapplied override settings for {}", featureName);
					} else {
						logger::warn("Failed to reapply override settings for {}", featureName);
					}
				}
			}

			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (sceneControlled) {
					ImGui::Text(
						"%s",
						T("menu.features.cannot_apply_overrides_scene",
							"Cannot apply overrides while scene-specific settings are active.\n"
							"Pause scene settings for this feature first."));
				} else {
					ImGui::Text(
						"%s",
						T("menu.features.restore_override_tooltip",
							"Restores original override settings from mod files.\n"
							"This will discard your customizations and revert to\n"
							"the mod author's recommended settings."));
				}
			}
		}

		if (canRestoreDefaults) {
			nextItem(restoreButtonWidth);
			if (Util::Icons::Button(restoreButtonText, Util::Icons::Kind::Reload, { restoreButtonWidth, 0 })) {
				feat->RestoreDefaultSettings();
				featureChanged = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.features.restore_defaults_tooltip", "Restore default settings for this feature.\nRevert undoes this until you save."));
			}
		}

		// Enable/Disable at boot toggle
		nextItem(ImGui::CalcTextSize(bootLabel).x + style.ItemInnerSpacing.x + bootToggleWidth);
		bool bootEnabled = !isDisabled;
		auto bootTooltip = [isDisabled]() {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					T("menu.features.boot_toggle_tooltip",
						"Toggle feature loading at boot.\n"
						"Current state: %s\n"
						"Restart required for changes to take effect.\n"
						"Disabling removes performance impact."),
					!isDisabled ? T("menu.features.enabled", "Enabled") : T("menu.features.disabled", "Disabled"));
			}
		};

		const ImVec2 labelPos = ImGui::GetCursorScreenPos();
		const ImVec2 labelSize = ImGui::CalcTextSize(bootLabel);
		bool toggled = ImGui::InvisibleButton("##BootLabel", ImVec2(labelSize.x, frameHeight));
		ImGui::GetWindowDrawList()->AddText(ImVec2(labelPos.x, labelPos.y + (frameHeight - labelSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), bootLabel);
		bootTooltip();

		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + frameHeight * 0.1f);
		{
			const bool failed = !feat->failedLoadedMessage.empty();
			if (failed)
				ImGui::PushStyleColor(ImGuiCol_Text, themeSettings.StatusPalette.Error);
			toggled = Util::FeatureToggle("##BootToggle", &bootEnabled) || toggled;
			if (failed)
				ImGui::PopStyleColor();
		}
		bootTooltip();

		if (toggled) {
			bool newState = feat->ToggleAtBootSetting();
			featureChanged = true;
			logger::info("{}: {} at boot.", featureName, newState ? "Disabled" : "Enabled");
		}
	};

	const float availableWidth = ImGui::GetContentRegionAvail().x;
	const ImVec2 titleStartPos = ImGui::GetCursorScreenPos();

	auto [description, keyFeatures] = feat->GetFeatureSummary();
	(void)keyFeatures;  // Not used for subtitle display

	const auto stage = feat->GetReleaseStage();
	const auto title = DrawPageTitle(feat->GetDisplayName(), isLoaded ? feat->version : "", Feature::GetReleaseStageTag(stage), StageTagColor(stage));
	const ImVec2 cursorAfterTitle = ImGui::GetCursorScreenPos();

	const bool actionsInline = title.width + style.ItemSpacing.x * 2.0f + actionsWidth <= availableWidth;
	if (actionsInline) {
		ImGui::SetCursorScreenPos(ImVec2(titleStartPos.x + availableWidth - actionsWidth, titleStartPos.y + (title.height - frameHeight) * 0.5f));
		drawActions();
		ImGui::SetCursorScreenPos(cursorAfterTitle);
	}

	DrawPageDescription(description);

	if (!actionsInline) {
		ImGui::Spacing();
		drawActions();
	}

	ImGui::Separator();
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;

	if (isDisabled) {
		ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."));
		ImGui::Spacing();
		ImGui::Text("%s", T("menu.features.enable_to_access_config", "Turn on Load at Boot above and restart the game to change its settings."));
	} else {
		if (isLoaded) {
			auto weatherRegistry = WeatherVariables::GlobalWeatherRegistry::GetSingleton();
			if (weatherRegistry->HasWeatherSupport(feat->GetShortName())) {
				bool paused = weatherRegistry->IsFeaturePaused(feat->GetShortName());
				if (ImGui::Checkbox(T("menu.features.pause_weather_overrides", "Pause Weather Overrides"), &paused)) {
					weatherRegistry->SetFeaturePaused(feat->GetShortName(), paused);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"%s",
						T("menu.features.pause_weather_tooltip",
							"Temporarily disable weather-based setting adjustments for this feature.\n"
							"This state is not saved."));
				}
				ImGui::Separator();
			}

			// Scene-specific settings toggle (Interior Only / TimeOfDay / Weather-Specific)
			// Show toggle whenever scene entries exist for this feature, even if feature-paused
			{
				const auto& featureShortName = feat->GetShortName();
				auto* sceneMgr = globals::sceneSettingsManager;
				bool scenePaused = sceneMgr->IsFeaturePaused(featureShortName);
				if (sceneControlled || scenePaused) {
					bool active = !scenePaused;
					if (Util::FeatureToggle("##PauseSceneSettings", &active))
						sceneMgr->SetFeaturePaused(featureShortName, !active);
					ImGui::SameLine();
					ImGui::Text("%s", T("menu.features.scene_specific_settings", "Scene Specific Settings"));
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(scenePaused ? "menu.features.scene_paused_tooltip" : "menu.features.scene_active_tooltip",
											  scenePaused ? "Paused - click to resume" : "Active - click to pause"));
					}
					ImGui::Separator();
				}
			}

			// Disable feature settings while scene overrides are actively applied (not paused)
			if (sceneControlled)
				ImGui::BeginDisabled();

			ImVec2 cursorPosBefore = ImGui::GetCursorPos();
			feat->DrawSettings();

			if (feat != &globals::features::csEditor && ProfilingRenderer::HasFeatureTimers(feat->GetShortName())) {
				ImGui::SeparatorText(T("menu.features.profiling", "Profiling"));
				ProfilingRenderer::RenderFeatureTimers(feat->GetShortName());
			}

			ImVec2 cursorPosAfter = ImGui::GetCursorPos();

			if (sceneControlled)
				ImGui::EndDisabled();

			// --- Reactive constraint detection ---
			// Compare the current full constraint set against g_knownConstraintKeys.
			// On the very first frame we just seed the set (no popup); after that
			// any key that wasn't previously known triggers the warning.
			// This catches both same-frame changes (e.g. TerrainBlending toggle)
			// and next-frame changes (e.g. Upscaling, whose resolutionScale is
			// updated in the render loop, not in DrawSettings).
			if (!g_reactiveWarningShow) {  // don't overwrite a pending popup
				auto currentConstraints = FeatureConstraints::GetAllActiveConstraints();

				if (!g_knownConstraintKeysInitialised) {
					// First time: seed known set, no popup
					for (const auto& [settingId, result] : currentConstraints) {
						g_knownConstraintKeys.insert(settingId.featureShortName + "|" + settingId.settingPath);
					}
					g_knownConstraintKeysInitialised = true;
				} else {
					// Diff: find keys present now but not previously known
					std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> newConstraints;
					std::unordered_set<std::string> currentKeys;
					for (const auto& [settingId, result] : currentConstraints) {
						std::string key = settingId.featureShortName + "|" + settingId.settingPath;
						currentKeys.insert(key);
						if (g_knownConstraintKeys.find(key) == g_knownConstraintKeys.end()) {
							newConstraints.emplace_back(settingId, result);
						}
					}
					// Update known set to current (removes keys for constraints that went away)
					g_knownConstraintKeys = std::move(currentKeys);

					if (!newConstraints.empty() && !globals::menu->GetSettings().SkipConstraintWarning) {
						logger::info("Reactive constraint detection: {} new constraints", newConstraints.size());
						for (const auto& [settingId, result] : newConstraints) {
							logger::info("  - {}.{} forced to {} by {}", settingId.featureShortName, settingId.settingPath, FeatureConstraints::FormatConstraintValue(result.forcedValue), result.sources.empty() ? "?" : result.sources[0].featureName);
						}
						g_reactiveWarningShow = true;
						g_reactiveWarningConstraints = std::move(newConstraints);
						g_dontShowAgainCheckbox = false;
					}
				}
			}

			const float cursorEpsilon = 0.1f;
			bool cursorMoved = (std::abs(cursorPosAfter.x - cursorPosBefore.x) > cursorEpsilon ||
								std::abs(cursorPosAfter.y - cursorPosBefore.y) > cursorEpsilon);
			if (!cursorMoved) {
				ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.no_settings_available", "There are no settings available for this feature."));
			}
		} else {
			if (FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
				feat->DrawUnloadedUI();
			} else if (feat->installed) {
				ImGui::Text("%s", T("menu.features.available_after_restart", "This feature will be available after restart."));
			} else {
				feat->DrawUnloadedUI();
				if (!feat->GetFeatureModLink().empty()) {
					ImGui::Spacing();
					auto featureModLink = feat->GetFeatureModLink();
					const auto downloadText = std::vformat(
						T("menu.features.download_link", "Click here to download this feature ({})"), std::make_format_args(featureModLink));
					if (ImGui::Selectable(downloadText.c_str())) {
						ShellExecuteA(NULL, "open", featureModLink.c_str(), NULL, NULL, SW_SHOWNORMAL);
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T("menu.features.download_tooltip", "Download the feature from the mod page."));
					}
				}
			}
		}
	}

	if (hasFailedMessage && feat->DrawFailLoadMessage() && !FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
		ImGui::Spacing();
		SeparatorTextWithFont(T("menu.features.error_header", "Error"), Menu::FontRole::Subheading);
		ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", feat->failedLoadedMessage.c_str());
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderReactiveConstraintWarningDialog()
{
	if (!g_reactiveWarningShow) {
		return;
	}

	constexpr const char* popupId = "###SettingChangeWarning";
	const std::string popupTitle = fmt::format("{}{}", T("menu.features.setting_change_warning_title", "Setting Change Warning"), popupId);

	// OpenPopup is idempotent while the popup is already open, so calling it
	// every frame while the flag is set is safe and ensures we don't miss the
	// one-frame window where ImGui expects it.
	ImGui::OpenPopup(popupId);

	// Center the popup (ImGuiCond_Always matches the Clear Cache dialog pattern)
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (Util::BeginPopupModalWithRoundedClose(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", T("menu.features.settings_adjusted_warning", "Some of your settings have been automatically adjusted due to feature incompatibilities."));
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		bool navigated = false;
		// Table columns: Impacted Feature | Setting | Constrained By | Forced To
		if (ImGui::BeginTable("##ReactiveConstraintTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T("menu.features.col_impacted_feature", "Impacted Feature"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_setting", "Setting"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_constrained_by", "Constrained By"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_forced_to", "Forced To"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			size_t rowIndex = 0;
			for (const auto& [settingId, result] : g_reactiveWarningConstraints) {
				ImGui::TableNextRow();

				// --- Column 0: Impacted Feature (clickable -> navigate to that feature) ---
				ImGui::TableSetColumnIndex(0);
				{
					// Look up the display name of the target feature from its short name
					std::string targetDisplayName = settingId.featureShortName;
					for (auto* f : Feature::GetFeatureList()) {
						if (f->GetShortName() == settingId.featureShortName) {
							targetDisplayName = f->GetDisplayName();
							break;
						}
					}
					if (ImGui::Selectable(fmt::format("{}##imp{}", targetDisplayName, rowIndex).c_str())) {
						pendingFeatureSelection = settingId.featureShortName;
						navigated = true;
						break;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), targetDisplayName.c_str());
					}
				}

				// --- Column 1: Setting name ---
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", settingId.settingPath.c_str());

				// --- Column 2: Constrained By (source features, clickable) ---
				ImGui::TableSetColumnIndex(2);
				if (!result.sources.empty()) {
					if (ImGui::Selectable(fmt::format("{}##src{}", result.sources[0].featureName, rowIndex).c_str())) {
						pendingFeatureSelection = result.sources[0].featureShortName;
						navigated = true;
						break;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), result.sources[0].featureName.c_str());
						if (result.sources.size() > 1) {
							ImGui::Separator();
							for (size_t i = 1; i < result.sources.size(); ++i) {
								ImGui::Text(T("menu.features.also_feature", "Also: %s"), result.sources[i].featureName.c_str());
							}
						}
						ImGui::Separator();
						ImGui::Text("%s", result.sources[0].reason.c_str());
					}
				}

				// --- Column 3: Forced value ---
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%s", FeatureConstraints::FormatConstraintValue(result.forcedValue).c_str());

				rowIndex++;
			}

			ImGui::EndTable();
		}

		const bool escapePressed = !ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		if (escapePressed)
			Util::NotifyEscapeHandledByPopup();
		if (navigated || escapePressed) {
			ImGui::CloseCurrentPopup();
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::EndPopup();
			return;
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"%s",
			T("menu.features.constraints_explanation",
				"These settings are disabled in their respective feature menus while the constraints are active. "
				"Adjust the constraining features to remove them."));

		ImGui::Spacing();

		// "Don't show again" checkbox -- same pattern as Clear Cache dialog
		ImGui::Checkbox(T("menu.features.dont_show_warning", "Don't show this warning again"), &g_dontShowAgainCheckbox);

		ImGui::Spacing();

		// Centered OK button
		constexpr float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH;
		const float windowWidth = ImGui::GetWindowWidth();
		const float offset = (windowWidth - buttonWidth) * 0.5f;
		if (offset > 0)
			ImGui::SetCursorPosX(offset);

		if (ImGui::Button(T("menu.features.ok_button", "OK"), ImVec2(buttonWidth, 0))) {
			if (g_dontShowAgainCheckbox) {
				if (auto* menu = globals::menu) {
					menu->GetSettings().SkipConstraintWarning = true;
				}
			}
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Popup was closed externally (e.g. clicked outside), reset state
		g_reactiveWarningShow = false;
		g_reactiveWarningConstraints.clear();
	}
}
