#include "ExtendedEffect.h"

#ifdef ENABLE_ENB_EXTENDER

#include <sstream>

#include "../EffectManager.h"
#include "../PresetManager.h"
#include "../WeatherManager.h"
#include "Globals.h"

void ExtendedEffect::Unload()
{
	weatherData.clear();
	editedWeatherKeys.clear();
	weatherValuesApplied = false;
	bindingCache.clear();
	Effect::Unload();
}

// Technique evaluation

int ExtendedEffect::ResolveTechniqueBinding(const std::string& variableName)
{
	auto cacheIt = bindingCache.find(variableName);
	if (cacheIt != bindingCache.end())
		return cacheIt->second;

	for (int i = 0; i < static_cast<int>(uiVariables.size()); ++i) {
		auto& uiVar = uiVariables[i];
		const std::string& uname = !uiVar.uniqueName.empty() ? uiVar.uniqueName
		                           : !uiVar.group.empty()    ? uiVar.group + "." + uiVar.displayName
		                                                     : uiVar.displayName;
		if (uname == variableName) {
			bindingCache[variableName] = i;
			return i;
		}
	}

	bindingCache[variableName] = -2;
	return -2;
}

bool ExtendedEffect::IsTechniqueEnabled(TechniqueInfo& info)
{
	for (auto& binding : info.bindings) {
		int idx = ResolveTechniqueBinding(binding.variableName);
		if (idx < 0)
			continue;

		auto& uiVar = uiVariables[idx];
		bool val = false;
		switch (uiVar.type) {
		case UIVariableType::Bool: val = uiVar.boolValue; break;
		case UIVariableType::Int: val = uiVar.intValue != 0; break;
		case UIVariableType::Float: val = uiVar.floatValue != 0.0f; break;
		default: val = true; break;
		}

		if (binding.inverted ? !val : val)
			continue;
		return false;
	}
	return true;
}

// Time-of-day interpolation

float ExtendedEffect::GetPeriodWeight(const std::string& period)
{
	auto& cd = EffectManager::GetSingleton().commonData;
	if (period == "Dawn") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Dawn)];
	if (period == "Sunrise") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunrise)];
	if (period == "Day") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Day)];
	if (period == "Sunset") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunset)];
	if (period == "Dusk") return cd.timeOfDay2[static_cast<int>(TimeOfDay2Index::Dusk)];
	if (period == "Night") return cd.timeOfDay2[static_cast<int>(TimeOfDay2Index::Night)];
	if (period == "Interior") return cd.eInteriorFactor;
	return 0.0f;
}

void ExtendedEffect::ApplyTimeOfDayInterpolation()
{
	struct PeriodVar
	{
		size_t index;
		float weight;
	};
	std::unordered_map<std::string, std::vector<PeriodVar>> baseGroups;

	for (size_t i = 0; i < uiVariables.size(); ++i) {
		auto& uiVar = uiVariables[i];
		if (uiVar.timePeriod.empty() || !uiVar.effectVariable)
			continue;
		auto& name = uiVar.name;
		auto& period = uiVar.timePeriod;
		if (name.size() <= period.size() || name.compare(name.size() - period.size(), period.size(), period) != 0)
			continue;
		baseGroups[name.substr(0, name.size() - period.size())].push_back({ i, GetPeriodWeight(period) });
	}

	auto& cd = EffectManager::GetSingleton().commonData;

	for (auto& [baseName, entries] : baseGroups) {
		auto baseVarIt = variables.find(baseName);
		if (baseVarIt == variables.end())
			continue;
		auto* baseVar = baseVarIt->second.get();
		if (!baseVar || !baseVar->IsValid())
			continue;

		auto& sep = uiVariables[entries[0].index].separation;
		if (sep == "ExteriorWeather" && cd.eInteriorFactor > 0.0f)
			continue;

		float totalWeight = 0.0f;
		for (auto& e : entries)
			totalWeight += e.weight;
		if (totalWeight <= 0.0f)
			continue;

		auto& firstVar = uiVariables[entries[0].index];

		if (firstVar.type == UIVariableType::Float) {
			float result = 0.0f;
			for (auto& e : entries)
				result += uiVariables[e.index].floatValue * (e.weight / totalWeight);
			baseVar->AsScalar()->SetFloat(result);
		} else {
			int comps = (firstVar.type == UIVariableType::Float2) ? 2 : (firstVar.type == UIVariableType::Float3) ? 3 : 4;
			float result[4] = {};
			for (auto& e : entries) {
				float w = e.weight / totalWeight;
				for (int c = 0; c < comps; ++c)
					result[c] += uiVariables[e.index].vectorValue[c] * w;
			}
			baseVar->AsVector()->SetFloatVector(result);
		}
	}
}

// Weather blending

void ExtendedEffect::LoadWeatherData()
{
	weatherData.clear();
	editedWeatherKeys.clear();

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	auto& weatherManager = WeatherManager::GetSingleton();
	const auto& weatherEntries = weatherManager.GetWeatherEntries();

	for (const auto& [key, entry] : weatherEntries) {
		std::filesystem::path filePath = PresetManager::GetSingleton().GetENBSeriesPath() / entry.fileName;
		if (!std::filesystem::exists(filePath))
			continue;

		std::string filePathStr = filePath.string();

		WeatherValues values;
		for (const auto& uiVar : uiVariables) {
			if (uiVar.isLabel)
				continue;
			if (!uiVar.effectVariable && !uiVar.isDefine)
				continue;
			if (uiVar.separation.empty() || uiVar.separation == "None")
				continue;

			std::string iniKey = GetVariableIniKey(uiVar);
			if (iniKey.empty())
				continue;

			if (IsPerComponentVector(uiVar)) {
				static const char* suffixes[] = { "X", "Y", "Z", "W" };
				int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 : 4;
				for (int c = 0; c < comps; ++c) {
					std::string compKey = iniKey + suffixes[c];
					char buffer[256];
					DWORD result = GetPrivateProfileStringA(section.c_str(), compKey.c_str(), "", buffer, sizeof(buffer), filePathStr.c_str());
					if (result > 0)
						values[compKey] = buffer;
				}
			} else {
				char buffer[1024];
				DWORD result = GetPrivateProfileStringA(section.c_str(), iniKey.c_str(), "", buffer, sizeof(buffer), filePathStr.c_str());
				if (result > 0)
					values[iniKey] = buffer;
			}
		}

		if (!values.empty()) {
			for (uint32_t weatherID : entry.weatherIDs)
				weatherData[weatherID] = values;
		}
	}

	if (!weatherData.empty())
		logger::info("[ExtendedEffect] Loaded weather data for '{}' ({} weathers)", GetName(), weatherData.size());
}

void ExtendedEffect::SaveWeatherData()
{
	if (editedWeatherKeys.empty() || !IsCompiled())
		return;

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	for (const auto& [key, entry] : WeatherManager::GetSingleton().GetWeatherEntries()) {
		auto editedIt = editedWeatherKeys.find(entry.fileName);
		if (editedIt == editedWeatherKeys.end() || entry.weatherIDs.empty())
			continue;
		// Every ID of an entry holds the same values (CommitUIEdit keeps them in sync), so any one is enough
		auto weatherIt = weatherData.find(entry.weatherIDs.front());
		if (weatherIt == weatherData.end())
			continue;

		std::string filePathStr = (PresetManager::GetSingleton().GetENBSeriesPath() / entry.fileName).string();
		for (const auto& iniKey : editedIt->second) {
			auto valueIt = weatherIt->second.find(iniKey);
			if (valueIt == weatherIt->second.end())
				continue;
			if (!WritePrivateProfileStringA(section.c_str(), iniKey.c_str(), valueIt->second.c_str(), filePathStr.c_str()))
				logger::warn("[ExtendedEffect] Failed to write key '{}' to weather file '{}'", iniKey, filePathStr);
		}
		WritePrivateProfileStringA(nullptr, nullptr, nullptr, filePathStr.c_str());
	}

	editedWeatherKeys.clear();
}

void ExtendedEffect::ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID)
{
	if (weatherData.empty())
		return;

	auto currentIt = weatherData.find(currentWeatherID);
	auto lastIt = weatherData.find(lastWeatherID);
	const WeatherValues* currentValues = currentIt != weatherData.end() ? &currentIt->second : nullptr;
	const WeatherValues* lastValues = lastIt != weatherData.end() ? &lastIt->second : nullptr;

	// With no weather values in play the live values must go back to the preset values once, then stay put
	if (!currentValues && !lastValues) {
		if (!weatherValuesApplied)
			return;
		weatherValuesApplied = false;
	} else {
		weatherValuesApplied = true;
	}

	auto safeStof = [](const std::string& s, float fallback) -> float {
		try { return std::stof(s); } catch (...) { return fallback; }
	};

	for (auto& uiVar : uiVariables) {
		if (uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable && !uiVar.isDefine)
			continue;
		if (!HasWeatherSeparation(uiVar))
			continue;

		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty())
			continue;

		// A weather that does not supply a value uses the preset value, never the previous blend result
		switch (uiVar.type) {
		case UIVariableType::Float:
			{
				auto getVal = [&](const WeatherValues* vals) -> float {
					if (!vals)
						return uiVar.baseFloatValue;
					auto it = vals->find(iniKey);
					if (it == vals->end())
						return uiVar.baseFloatValue;
					return safeStof(it->second, uiVar.baseFloatValue);
				};

				float currentVal = getVal(currentValues);
				float lastVal = getVal(lastValues);
				uiVar.floatValue = lastVal + blendFactor * (currentVal - lastVal);
				if (uiVar.effectVariable)
					uiVar.effectVariable->AsScalar()->SetFloat(uiVar.floatValue);
				break;
			}
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			{
				int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 : 4;
				bool perComp = IsPerComponentVector(uiVar);

				auto parseVec = [&](const WeatherValues* vals, float* out) {
					memcpy(out, uiVar.baseVectorValue, sizeof(float) * comps);
					if (!vals)
						return;
					if (perComp) {
						static const char* suffixes[] = { "X", "Y", "Z", "W" };
						for (int c = 0; c < comps; ++c) {
							auto it = vals->find(iniKey + suffixes[c]);
							if (it != vals->end())
								out[c] = safeStof(it->second, uiVar.baseVectorValue[c]);
						}
					} else {
						auto it = vals->find(iniKey);
						if (it != vals->end()) {
							std::stringstream ss(it->second);
							std::string item;
							for (int c = 0; c < comps && std::getline(ss, item, ','); ++c)
								out[c] = safeStof(item, uiVar.baseVectorValue[c]);
						}
					}
				};

				float currentVals[4] = {}, lastVals[4] = {};
				parseVec(currentValues, currentVals);
				parseVec(lastValues, lastVals);

				for (int c = 0; c < comps; ++c)
					uiVar.vectorValue[c] = lastVals[c] + blendFactor * (currentVals[c] - lastVals[c]);

				if (uiVar.effectVariable)
					uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
				break;
			}
		default:
			break;
		}
	}
}

void ExtendedEffect::CommitUIEdit(size_t index, uint32_t weatherID)
{
	if (index >= uiVariables.size())
		return;

	auto& uiVar = uiVariables[index];
	std::string iniKey = GetVariableIniKey(uiVar);

	// All IDs sharing a weather file hold one copy of its values each; update every copy
	std::vector<WeatherValues*> weatherCopies;
	const WeatherManager::WeatherEntry* entry = nullptr;
	if (HasWeatherSeparation(uiVar) && !iniKey.empty()) {
		entry = WeatherManager::GetSingleton().FindWeatherEntry(weatherID);
		if (entry) {
			for (uint32_t linkedID : entry->weatherIDs) {
				auto it = weatherData.find(linkedID);
				if (it != weatherData.end())
					weatherCopies.push_back(&it->second);
			}
		}
	}

	// Stores value under key in the active weather if that weather supplies the key
	auto storeInWeather = [&](const std::string& key, const std::string& value) {
		if (weatherCopies.empty() || !weatherCopies.front()->contains(key))
			return false;
		for (auto* values : weatherCopies)
			(*values)[key] = value;
		editedWeatherKeys[entry->fileName].insert(key);
		return true;
	};

	switch (uiVar.type) {
	case UIVariableType::Float:
		if (!storeInWeather(iniKey, std::to_string(uiVar.floatValue)))
			uiVar.baseFloatValue = uiVar.floatValue;
		break;
	case UIVariableType::Float2:
	case UIVariableType::Float3:
	case UIVariableType::Float4:
		{
			int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
			                                                                                                  4;
			if (IsPerComponentVector(uiVar)) {
				static const char* suffixes[] = { "X", "Y", "Z", "W" };
				for (int c = 0; c < comps; ++c) {
					if (!storeInWeather(iniKey + suffixes[c], std::to_string(uiVar.vectorValue[c])))
						uiVar.baseVectorValue[c] = uiVar.vectorValue[c];
				}
			} else {
				std::string val;
				for (int c = 0; c < comps; ++c) {
					if (c > 0)
						val += ", ";
					val += std::to_string(uiVar.vectorValue[c]);
				}
				if (!storeInWeather(iniKey, val))
					std::copy(uiVar.vectorValue, uiVar.vectorValue + comps, uiVar.baseVectorValue);
			}
			break;
		}
	default:
		break;
	}
}

// Rendering

#include "../ENBExtender.h"
#include "../UITree.h"

namespace
{
	float SafeStofLocal(const std::string& s, float fallback = 0.0f)
	{
		return ENBExtender::SafeStof(s, fallback);
	}

	bool EvaluateCondition(const std::string& condStr, float boundValue)
	{
		if (condStr.empty())
			return boundValue != 0.0f;
		size_t valueStart = 0;
		if (condStr.size() >= 2 && !std::isdigit(static_cast<unsigned char>(condStr[1])) && condStr[1] != '-')
			valueStart = 2;
		else if (condStr[0] == '<' || condStr[0] == '>')
			valueStart = 1;
		else
			return boundValue != 0.0f;
		float cmp = SafeStofLocal(condStr.substr(valueStart));
		char c0 = condStr[0], c1 = (condStr.size() >= 2) ? condStr[1] : '\0';
		if (c0 == '=' && c1 == '=') return boundValue == cmp;
		if (c0 == '!' && c1 == '=') return boundValue != cmp;
		if (c0 == '<' && c1 == '=') return boundValue <= cmp;
		if (c0 == '>' && c1 == '=') return boundValue >= cmp;
		if (c0 == '=' && c1 == '<') return boundValue <= cmp;
		if (c0 == '=' && c1 == '>') return boundValue >= cmp;
		if (c0 == '<') return boundValue < cmp;
		if (c0 == '>') return boundValue > cmp;
		return false;
	}

	using FileUniqueNameMap = std::unordered_map<std::string, std::unordered_map<std::string, UITree::VarRef>>;

	std::pair<bool, bool> EvaluateBinding(const Effect::UIVariable& var,
		const std::unordered_map<std::string, UITree::VarRef>& uniqueNameMap,
		const FileUniqueNameMap& fileUniqueNameMap)
	{
		bool visible = true, readOnly = var.isReadOnly;
		if (var.uiBindings.empty())
			return { visible, readOnly };

		for (const auto& binding : var.uiBindings) {
			const UITree::VarRef* boundRef = nullptr;
			if (!binding.file.empty()) {
				auto fileIt = fileUniqueNameMap.find(binding.file);
				if (fileIt != fileUniqueNameMap.end()) {
					auto varIt = fileIt->second.find(binding.target);
					if (varIt != fileIt->second.end())
						boundRef = &varIt->second;
				}
			} else {
				auto it = uniqueNameMap.find(binding.target);
				if (it != uniqueNameMap.end())
					boundRef = &it->second;
			}

			if (!boundRef)
				continue;

			const auto& bv = boundRef->effect->uiVariables[boundRef->index];
			float val = 0.0f;
			switch (bv.type) {
			case Effect::UIVariableType::Float: val = bv.floatValue; break;
			case Effect::UIVariableType::Int: val = static_cast<float>(bv.intValue); break;
			case Effect::UIVariableType::Bool: val = bv.boolValue ? 1.0f : 0.0f; break;
			default: break;
			}

			bool cond = EvaluateCondition(binding.condition, val);
			if (binding.inverted)
				cond = !cond;

			std::string prop = binding.property;
			std::transform(prop.begin(), prop.end(), prop.begin(), ::tolower);
			if (prop == "hidden") { if (cond) visible = false; }
			else if (prop == "visible") { if (!cond) visible = false; }
			else if (prop == "readonly") { if (cond) readOnly = true; }
			else if (prop == "readwrite") { if (!cond) readOnly = true; }
			else { if (!cond) visible = false; }
		}

		return { visible, readOnly };
	}

	bool IsVarVisible(const Effect::UIVariable& uiVar)
	{
		return !uiVar.displayName.empty() && !uiVar.isHidden;
	}

	struct RenderContext
	{
		std::unordered_map<std::string, UITree::VarRef>& uniqueNameMap;
		FileUniqueNameMap& fileUniqueNameMap;
		std::unordered_set<Effect*>& changedEffects;
		std::vector<UITree::VarRef>& changedVars;
		UITree::MetaMap& meta;
		bool performanceMode = false;
		int tableCounter = 0;

		bool BeginVarTable()
		{
			std::string tableId = "##ut_" + std::to_string(tableCounter++);
			if (ImGui::BeginTable(tableId.c_str(), 2, ImGuiTableFlags_SizingFixedFit)) {
				float w = ImGui::GetContentRegionAvail().x;
				ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, w * 0.45f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, w * 0.55f);
				return true;
			}
			return false;
		}
	};

	void RenderWidget(const std::string& label, const std::string& id,
		Effect::UIVariable& uiVar, bool readOnly, const UITree::VarRef& ref,
		std::unordered_set<Effect*>& changedEffects, std::vector<UITree::VarRef>& changedVars)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (readOnly)
			ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Disable);
		ImGui::Text("%s", label.c_str());

		ImGui::TableSetColumnIndex(1);
		if (readOnly)
			ImGui::BeginDisabled();
		bool changed = false;
		float floatStep = (uiVar.floatMax - uiVar.floatMin) / 100.0f;
		switch (uiVar.type) {
		case Effect::UIVariableType::Float:
			changed = ImGui::InputFloat(id.c_str(), &uiVar.floatValue, floatStep, floatStep * 10.0f, "%.3f");
			if (changed)
				uiVar.floatValue = std::clamp(uiVar.floatValue, uiVar.floatMin, uiVar.floatMax);
			break;
		case Effect::UIVariableType::Int:
			if ((uiVar.widgetType == Effect::UIWidgetType::Dropdown || uiVar.widgetType == Effect::UIWidgetType::Quality) && !uiVar.dropdownItems.empty()) {
				int di = (uiVar.widgetType == Effect::UIWidgetType::Quality) ? uiVar.intValue + 1 : uiVar.intValue;
				const char* cur = (di >= 0 && di < static_cast<int>(uiVar.dropdownItems.size())) ? uiVar.dropdownItems[di].c_str() : "";
				if (ImGui::BeginCombo(id.c_str(), cur)) {
					for (int j = 0; j < static_cast<int>(uiVar.dropdownItems.size()); ++j) {
						int iv = (uiVar.widgetType == Effect::UIWidgetType::Quality) ? (j - 1) : j;
						if (ImGui::Selectable(uiVar.dropdownItems[j].c_str(), uiVar.intValue == iv)) {
							uiVar.intValue = iv;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
			} else {
				changed = ImGui::InputInt(id.c_str(), &uiVar.intValue, 1, 10);
				if (changed)
					uiVar.intValue = std::clamp(uiVar.intValue, uiVar.intMin, uiVar.intMax);
			}
			break;
		case Effect::UIVariableType::Bool:
			changed = ImGui::Checkbox(id.c_str(), &uiVar.boolValue);
			break;
		case Effect::UIVariableType::Float2:
			changed = ImGui::InputScalarN(id.c_str(), ImGuiDataType_Float, uiVar.vectorValue, 2, &floatStep, nullptr, "%.3f");
			if (changed)
				for (int i = 0; i < 2; ++i)
					uiVar.vectorValue[i] = std::clamp(uiVar.vectorValue[i], uiVar.floatMin, uiVar.floatMax);
			break;
		case Effect::UIVariableType::Float3:
			if (uiVar.widgetType == Effect::UIWidgetType::Color) {
				changed = ImGui::ColorEdit3(id.c_str(), uiVar.vectorValue);
			} else {
				float min3 = (uiVar.widgetType == Effect::UIWidgetType::Vector) ? -1.0f : uiVar.floatMin;
				float max3 = (uiVar.widgetType == Effect::UIWidgetType::Vector) ? 1.0f : uiVar.floatMax;
				float step3 = (max3 - min3) / 100.0f;
				changed = ImGui::InputScalarN(id.c_str(), ImGuiDataType_Float, uiVar.vectorValue, 3, &step3, nullptr, "%.3f");
				if (changed)
					for (int i = 0; i < 3; ++i)
						uiVar.vectorValue[i] = std::clamp(uiVar.vectorValue[i], min3, max3);
			}
			break;
		case Effect::UIVariableType::Float4:
			if (uiVar.widgetType == Effect::UIWidgetType::Color) {
				changed = ImGui::ColorEdit4(id.c_str(), uiVar.vectorValue);
			} else {
				changed = ImGui::InputScalarN(id.c_str(), ImGuiDataType_Float, uiVar.vectorValue, 4, &floatStep, nullptr, "%.3f");
				if (changed)
					for (int i = 0; i < 4; ++i)
						uiVar.vectorValue[i] = std::clamp(uiVar.vectorValue[i], uiVar.floatMin, uiVar.floatMax);
			}
			break;
		}
		if (changed) {
			changedEffects.insert(ref.effect);
			changedVars.push_back(ref);
		}
		if (readOnly)
			ImGui::EndDisabled();

		if (!uiVar.separation.empty() && uiVar.separation != "None") {
			ImGui::SameLine();
			ImGui::Text("W");
		}

		if (readOnly)
			ImGui::PopStyleColor();
	}

	bool RenderVar(UITree::VarRef& ref, bool& inTable, RenderContext& ctx)
	{
		auto& uiVar = ref.effect->uiVariables[ref.index];

		if (!IsVarVisible(uiVar))
			return false;
		if (ctx.performanceMode && !uiVar.ignorePerfMode)
			return false;

		auto [bindVisible, bindReadOnly] = EvaluateBinding(uiVar, ctx.uniqueNameMap, ctx.fileUniqueNameMap);
		if (!bindVisible)
			return false;

		if (uiVar.isLabel) {
			if (inTable) { ImGui::EndTable(); inTable = false; }
			if (uiVar.isReadOnly)
				ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Disable);
			ImGui::TextWrapped("%s", uiVar.displayName.c_str());
			if (uiVar.isReadOnly)
				ImGui::PopStyleColor();
		} else {
			if (!inTable) {
				if (!ctx.BeginVarTable())
					return false;
				inTable = true;
			}
			RenderWidget(uiVar.displayName, "##uv_" + std::to_string(ref.index) + "_" + ref.effect->GetName(),
				uiVar, bindReadOnly, ref, ctx.changedEffects, ctx.changedVars);
		}
		return true;
	}

	void RenderTechniqueDropdown(Effect* effect, std::unordered_set<Effect*>& changedEffects)
	{
		ImGui::Text("%s", effect->techniqueDropdown.name.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		const char* current = effect->uiTechniques[effect->selectedTechniqueIndex].displayName.c_str();
		if (ImGui::BeginCombo(("##TECHNIQUE_" + effect->GetName()).c_str(), current)) {
			for (uint32_t i = 0; i < effect->uiTechniques.size(); ++i) {
				if (ImGui::Selectable(effect->uiTechniques[i].displayName.c_str(), effect->selectedTechniqueIndex == i)) {
					effect->selectedTechniqueIndex = i;
					changedEffects.insert(effect);
				}
				if (effect->selectedTechniqueIndex == i)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}

	bool HasVisibleContent(const UITree::GroupNode& node)
	{
		for (auto& item : node.items) {
			if (item.type == UITree::Item::Type::Variable) {
				auto& uiVar = item.var.effect->uiVariables[item.var.index];
				if (IsVarVisible(uiVar))
					return true;
			} else if (item.type == UITree::Item::Type::Group && item.group) {
				if (HasVisibleContent(*item.group))
					return true;
			}
		}
		return false;
	}

	void RenderGroupNode(UITree::GroupNode& node, RenderContext& ctx,
		const std::vector<std::pair<Effect*, std::string>>& techDropdowns)
	{
		for (auto& [effect, group] : techDropdowns)
			if (!group.empty() && group == node.fullPath && !effect->techniqueDropdown.topLevel)
				RenderTechniqueDropdown(effect, ctx.changedEffects);

		bool inTable = false;
		bool lastWasSeparator = false;

		for (auto& item : node.items) {
			switch (item.type) {
			case UITree::Item::Type::Variable:
				if (RenderVar(item.var, inTable, ctx))
					lastWasSeparator = false;
				break;

			case UITree::Item::Type::Separator:
				if (!lastWasSeparator) {
					if (inTable) { ImGui::EndTable(); inTable = false; }
					ImGui::Separator();
					lastWasSeparator = true;
				}
				break;

			case UITree::Item::Type::Group:
				if (!item.group || !HasVisibleContent(*item.group))
					break;
				if (inTable) { ImGui::EndTable(); inTable = false; }
				lastWasSeparator = false;

				{
					std::string displayName = item.group->name;
					ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
					auto metaIt = ctx.meta.find(item.group->fullPath);
					if (metaIt != ctx.meta.end()) {
						if (!metaIt->second.displayName.empty())
							displayName = metaIt->second.displayName;
						if (metaIt->second.defaultOpen)
							flags = ImGuiTreeNodeFlags_DefaultOpen;
					}
					if (ImGui::TreeNodeEx((displayName + "###ugrp_" + item.group->fullPath).c_str(), flags)) {
						RenderGroupNode(*item.group, ctx, techDropdowns);
						ImGui::TreePop();
					}
				}
				break;
			}
		}

		if (inTable)
			ImGui::EndTable();
	}
}

void ExtendedEffect::RenderImGui()
{
	Effect* self = this;
	RenderMergedUI({ &self, 1 });
}

void ExtendedEffect::RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter)
{
	UITree::Tree tree;
	tree.Build(effects, filter);

	std::vector<std::pair<Effect*, std::string>> techDropdowns;
	for (auto* effect : effects) {
		if (!effect->IsCompiled() || effect->uiTechniques.size() <= 1 || !effect->techniqueDropdown.visible)
			continue;
		techDropdowns.push_back({ effect, effect->techniqueDropdown.group });
		if (!effect->techniqueDropdown.group.empty()) {
			auto [it, inserted] = tree.meta.try_emplace(effect->techniqueDropdown.group);
			if (inserted) {
				it->second.displayName = effect->techniqueDropdown.groupName;
				it->second.defaultOpen = effect->techniqueDropdown.groupOpen;
				it->second.ordering = effect->techniqueDropdown.ordering;
				it->second.hasOrdering = true;
			}
			UITree::TraverseGroupPath(tree.root, effect->techniqueDropdown.group, tree.meta);
		}
	}

	tree.Sort();

	std::unordered_set<Effect*> changedEffects;
	std::vector<UITree::VarRef> changedVars;
	RenderContext ctx{ tree.uniqueNameMap, tree.fileUniqueNameMap, changedEffects, changedVars, tree.meta,
		EffectManager::GetSingleton().performanceMode };

	if (filter != UITree::FilterMode::TopLevelOnly) {
		for (auto& [effect, group] : techDropdowns)
			if (effect->techniqueDropdown.topLevel || group.empty())
				RenderTechniqueDropdown(effect, changedEffects);
	}

	RenderGroupNode(tree.root, ctx, techDropdowns);

	if (!changedEffects.empty()) {
		auto& cd = EffectManager::GetSingleton().commonData;
		uint32_t activeWeatherID = static_cast<uint32_t>(cd.weather[2] > 0.5f ? cd.weather[0] : cd.weather[1]);
		for (auto& ref : changedVars) {
			if (auto* ext = dynamic_cast<ExtendedEffect*>(ref.effect))
				ext->CommitUIEdit(static_cast<size_t>(ref.index), activeWeatherID);
		}
		for (auto* effect : changedEffects)
			effect->UpdateUIVariables();
	}

	for (auto* effect : effects) {
		if (!effect->GetErrors().empty()) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Error, "%s:", effect->GetName().c_str());
			for (const auto& err : effect->GetErrors())
				ImGui::TextWrapped("%s", err.c_str());
		}
	}
}

#endif
