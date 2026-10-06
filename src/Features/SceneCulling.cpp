#include "SceneCulling.h"

#include "Features/Skylighting.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#include <cmath>
#include <numbers>

#define I18N_KEY_PREFIX "feature.scene_culling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SceneCulling::Settings,
	EnableSunShadowCulling,
	SunMinCascade,
	SunMinDistance,
	SunMaxRadius,
	SunMinAngularSize,
	SunMinElevation,
	EnablePointShadowCulling,
	PointShadowCullingInteriors,
	PointMinDistance,
	PointMaxRadius,
	PointMinAngularSize,
	EnableActorShadowCulling,
	ActorShadowDistance,
	ActorShadowCullingPointLights,
	EnableSubtreePruning,
	EnableDecalCulling,
	DecalMaxDistance,
	DecalMaxRadius,
	EnableLightGatherThrottle,
	LightGatherMinMove,
	LightGatherMinRadiusChange,
	LightGatherMaxAgeMs)

namespace
{
	thread_local int tl_insideActor = 0;

	void DistanceTooltip(const char* a_text, float a_distance)
	{
		if (auto _tt = Util::HoverTooltipWrapper())
			Util::DrawMultiLineTooltip({ a_text, Util::Units::FormatDistance(a_distance) });
	}
}

void SceneCulling::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	settings.SunMinCascade = std::clamp(settings.SunMinCascade, 0, static_cast<int>(kMaxCascades) - 1);
	settings.SunMinDistance = Util::ClampFinite(settings.SunMinDistance, defaults.SunMinDistance, 0.f, 20000.f);
	settings.SunMaxRadius = Util::ClampFinite(settings.SunMaxRadius, defaults.SunMaxRadius, 10.f, 1000.f);
	settings.SunMinAngularSize = Util::ClampFinite(settings.SunMinAngularSize, defaults.SunMinAngularSize, 0.001f, 0.2f);
	settings.SunMinElevation = Util::ClampFinite(settings.SunMinElevation, defaults.SunMinElevation, 0.f, 90.f);
	settings.PointMinDistance = Util::ClampFinite(settings.PointMinDistance, defaults.PointMinDistance, 0.f, 20000.f);
	settings.PointMaxRadius = Util::ClampFinite(settings.PointMaxRadius, defaults.PointMaxRadius, 10.f, 1000.f);
	settings.PointMinAngularSize = Util::ClampFinite(settings.PointMinAngularSize, defaults.PointMinAngularSize, 0.001f, 0.2f);
	settings.ActorShadowDistance = Util::ClampFinite(settings.ActorShadowDistance, defaults.ActorShadowDistance, kMinActorShadowDistance, 20000.f);
	settings.DecalMaxDistance = Util::ClampFinite(settings.DecalMaxDistance, defaults.DecalMaxDistance, 0.f, 20000.f);
	settings.DecalMaxRadius = Util::ClampFinite(settings.DecalMaxRadius, defaults.DecalMaxRadius, 1.f, 500.f);
	settings.LightGatherMinMove = Util::ClampFinite(settings.LightGatherMinMove, defaults.LightGatherMinMove, 0.f, 256.f);
	settings.LightGatherMinRadiusChange = Util::ClampFinite(settings.LightGatherMinRadiusChange, defaults.LightGatherMinRadiusChange, 0.f, 1024.f);
	settings.LightGatherMaxAgeMs = Util::ClampFinite(settings.LightGatherMaxAgeMs, defaults.LightGatherMaxAgeMs, 0.f, 5000.f);
}

void SceneCulling::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SceneCulling::RestoreDefaultSettings()
{
	settings = {};
}

void SceneCulling::DrawSettings()
{
	ImGui::SeparatorText(T(TKEY("sun_shadows"), "Sun Shadows"));

	ImGui::Checkbox(T(TKEY("enable_sun_shadow_culling"), "Cull Small Distant Objects"), &settings.EnableSunShadowCulling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_sun_shadow_culling_tooltip"), "Objects that are far away and small in relation to their distance no longer cast sun shadows. An object is only dropped when every rule below applies."));

	{
		auto _ = Util::DisableGuard(!settings.EnableSunShadowCulling);
		ImGui::Indent();

		ImGui::SliderInt(T(TKEY("sun_min_cascade"), "First Culled Cascade"), &settings.SunMinCascade, 0, static_cast<int>(kMaxCascades) - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("sun_min_cascade_tooltip"), "Cascades closer than this are never touched. 0 is the nearest cascade, 1 starts with the second."));

		ImGui::SliderFloat(T(TKEY("sun_min_distance"), "Min Distance"), &settings.SunMinDistance, 0.f, 10000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("sun_min_distance_tooltip"), "Objects closer to the camera than this always keep their shadow."), settings.SunMinDistance);

		ImGui::SliderFloat(T(TKEY("sun_max_radius"), "Max Radius"), &settings.SunMaxRadius, 10.f, 1000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("sun_max_radius_tooltip"), "Objects with a larger bounding radius always keep their shadow. At low sun the radius is scaled by the shadow length."), settings.SunMaxRadius);

		ImGui::SliderFloat(T(TKEY("sun_min_angular_size"), "Min Angular Size"), &settings.SunMinAngularSize, 0.001f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("sun_min_angular_size_tooltip"), "Radius divided by distance. Below this the object appears too small for its shadow to matter. Higher values cull more."));

		ImGui::SliderFloat(T(TKEY("sun_min_elevation"), "Min Sun Elevation"), &settings.SunMinElevation, 0.f, 90.f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("sun_min_elevation_tooltip"), "Below this sun angle no sun shadow is dropped at all, since long morning and evening shadows of small objects are easy to notice. 0 always culls."));

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("point_shadows"), "Torch and Point Light Shadows"));

	ImGui::Checkbox(T(TKEY("enable_point_shadow_culling"), "Cull Small Distant Objects##Point"), &settings.EnablePointShadowCulling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_point_shadow_culling_tooltip"), "Same rule for shadows cast by torches, fires and lanterns. Objects right next to the light always keep their shadow."));

	{
		auto _ = Util::DisableGuard(!settings.EnablePointShadowCulling);
		ImGui::Indent();

		ImGui::Checkbox(T(TKEY("point_shadow_culling_interiors"), "Also In Interiors"), &settings.PointShadowCullingInteriors);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("point_shadow_culling_interiors_tooltip"), "Interiors are lit almost only by point lights, so missing shadows stand out there. Off by default."));

		ImGui::SliderFloat(T(TKEY("point_min_distance"), "Min Distance##Point"), &settings.PointMinDistance, 0.f, 10000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("point_min_distance_tooltip"), "Objects closer to the camera than this always keep their shadow."), settings.PointMinDistance);

		ImGui::SliderFloat(T(TKEY("point_max_radius"), "Max Radius##Point"), &settings.PointMaxRadius, 10.f, 1000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("point_max_radius_tooltip"), "Objects with a larger bounding radius always keep their shadow."), settings.PointMaxRadius);

		ImGui::SliderFloat(T(TKEY("point_min_angular_size"), "Min Angular Size##Point"), &settings.PointMinAngularSize, 0.001f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("point_min_angular_size_tooltip"), "Radius divided by distance to the camera. Higher values cull more."));

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("actor_shadows"), "Character Shadows"));

	ImGui::Checkbox(T(TKEY("enable_actor_shadow_culling"), "Cull Distant Character Shadows"), &settings.EnableActorShadowCulling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_actor_shadow_culling_tooltip"), "Characters and creatures beyond the distance below do not cast shadows. Not applied while the sun is below Min Sun Elevation."));

	{
		auto _ = Util::DisableGuard(!settings.EnableActorShadowCulling);
		ImGui::Indent();

		ImGui::SliderFloat(T(TKEY("actor_shadow_distance"), "Character Shadow Distance"), &settings.ActorShadowDistance, kMinActorShadowDistance, 12000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("actor_shadow_distance_tooltip"), "Distance from the camera beyond which character shadows are dropped."), settings.ActorShadowDistance);

		ImGui::Checkbox(T(TKEY("actor_shadow_culling_point_lights"), "Also For Torch Shadows"), &settings.ActorShadowCullingPointLights);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("actor_shadow_culling_point_lights_tooltip"), "Apply the same distance to shadows cast by torches and fires."));

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("traversal"), "Scene Traversal"));

	ImGui::Checkbox(T(TKEY("enable_subtree_pruning"), "Skip Whole Branches"), &settings.EnableSubtreePruning);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_subtree_pruning_tooltip"), "In the sun shadow and skylighting passes, skip a whole scene-graph branch when the branch as a whole already meets the culling rule. Same result, fewer nodes visited."));

	ImGui::SeparatorText(T(TKEY("decals"), "Decals"));

	ImGui::Checkbox(T(TKEY("enable_decal_culling"), "Cull Small Distant Decals"), &settings.EnableDecalCulling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_decal_culling_tooltip"), "Footprint-sized decals (blood, footprints, dirt) beyond the distance below are not drawn. Larger decals such as plaster patches on walls stay."));

	{
		auto _ = Util::DisableGuard(!settings.EnableDecalCulling);
		ImGui::Indent();

		ImGui::SliderFloat(T(TKEY("decal_max_distance"), "Max Distance##Decal"), &settings.DecalMaxDistance, 0.f, 10000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("decal_max_distance_tooltip"), "Decals farther from the camera than this are dropped."), settings.DecalMaxDistance);

		ImGui::SliderFloat(T(TKEY("decal_max_radius"), "Max Radius##Decal"), &settings.DecalMaxRadius, 1.f, 500.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("decal_max_radius_tooltip"), "Only decals with a smaller bounding radius are affected. Footprints are about 22; above 50 wall stains start to disappear."), settings.DecalMaxRadius);

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("dynamic_lights"), "Dynamic Lights"));

	ImGui::Checkbox(T(TKEY("enable_light_gather_throttle"), "Throttle Light Assignment"), &settings.EnableLightGatherThrottle);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enable_light_gather_throttle_tooltip"), "The engine re-searches which objects each dynamic light touches every frame. Keep the previous result while the light has barely moved."));
	if (!lightGatherHooked)
		ImGui::TextDisabled("%s", T(TKEY("light_gather_unavailable"), "Inactive: the engine call site did not match this game version."));

	{
		auto _ = Util::DisableGuard(!settings.EnableLightGatherThrottle || !lightGatherHooked);
		ImGui::Indent();

		ImGui::SliderFloat(T(TKEY("light_gather_min_move"), "Min Movement"), &settings.LightGatherMinMove, 0.f, 64.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("light_gather_min_move_tooltip"), "Re-search once the light has moved at least this far. Flicker moves a light only a few units."), settings.LightGatherMinMove);

		ImGui::SliderFloat(T(TKEY("light_gather_min_radius_change"), "Min Radius Change"), &settings.LightGatherMinRadiusChange, 0.f, 256.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		DistanceTooltip(T(TKEY("light_gather_min_radius_change_tooltip"), "Re-search once the light radius has changed by at least this much."), settings.LightGatherMinRadiusChange);

		ImGui::SliderFloat(T(TKEY("light_gather_max_age"), "Max Age"), &settings.LightGatherMaxAgeMs, 0.f, 2000.f, "%.0f ms", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("light_gather_max_age_tooltip"), "Re-search at least this often so characters walking past a static light still get lit."));

		ImGui::Unindent();
	}

	ImGui::SeparatorText(T(TKEY("statistics"), "Statistics"));

	ImGui::Checkbox(T(TKEY("show_counters"), "Show Per-Frame Counters"), &showCounters);
	if (showCounters && ImGui::BeginTable("SceneCullingCounters", 2, ImGuiTableFlags_SizingStretchProp)) {
		const auto row = [](const char* a_label, uint32_t a_value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(a_label);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%u", a_value);
		};
		row(T(TKEY("counter_sun"), "Sun shadow meshes culled / kept"), lastCounters.sunCulled);
		ImGui::SameLine();
		ImGui::Text("/ %u", lastCounters.sunKept);
		row(T(TKEY("counter_point"), "Point light shadow meshes culled / kept"), lastCounters.pointCulled);
		ImGui::SameLine();
		ImGui::Text("/ %u", lastCounters.pointKept);
		row(T(TKEY("counter_actor"), "Character shadow meshes culled"), lastCounters.actorCulled);
		row(T(TKEY("counter_pruned_sun"), "Sun shadow branches skipped"), lastCounters.prunedSun);
		row(T(TKEY("counter_pruned_precip"), "Skylighting branches skipped"), lastCounters.prunedPrecip);
		row(T(TKEY("counter_decal"), "Decals culled"), lastCounters.decalCulled);
		row(T(TKEY("counter_light_gather"), "Light searches run / skipped"), lastCounters.lightGatherCalls);
		ImGui::SameLine();
		ImGui::Text("/ %u", lastCounters.lightGatherSkipped);
		ImGui::EndTable();
	}
}

void SceneCulling::PostPostLoad()
{
	Hooks::Install();
}

void SceneCulling::DataLoaded()
{
	Hooks::InstallLate();
}

void SceneCulling::EarlyPrepass()
{
	if (collectCounters.load(std::memory_order_relaxed)) {
		lastCounters.sunCulled = counters.sunCulled.exchange(0, std::memory_order_relaxed);
		lastCounters.sunKept = counters.sunKept.exchange(0, std::memory_order_relaxed);
		lastCounters.pointCulled = counters.pointCulled.exchange(0, std::memory_order_relaxed);
		lastCounters.pointKept = counters.pointKept.exchange(0, std::memory_order_relaxed);
		lastCounters.actorCulled = counters.actorCulled.exchange(0, std::memory_order_relaxed);
		lastCounters.prunedSun = counters.prunedSun.exchange(0, std::memory_order_relaxed);
		lastCounters.prunedPrecip = counters.prunedPrecip.exchange(0, std::memory_order_relaxed);
		lastCounters.decalCulled = counters.decalCulled.exchange(0, std::memory_order_relaxed);
		lastCounters.lightGatherCalls = counters.lightGatherCalls.exchange(0, std::memory_order_relaxed);
		lastCounters.lightGatherSkipped = counters.lightGatherSkipped.exchange(0, std::memory_order_relaxed);
	}
	collectCounters.store(showCounters, std::memory_order_relaxed);

	const auto pos = Util::GetCameraWorldPosition();
	camX.store(pos.x, std::memory_order_relaxed);
	camY.store(pos.y, std::memory_order_relaxed);
	camZ.store(pos.z, std::memory_order_relaxed);

	const auto player = globals::game::player;
	const auto cell = player ? player->GetParentCell() : nullptr;
	pointCullAllowed.store(!cell || !cell->IsInteriorCell() || settings.PointShadowCullingInteriors, std::memory_order_relaxed);

	const auto sky = globals::game::sky;
	precipCamera.store(sky && sky->precip ? sky->precip->occlusionData.camera.get() : nullptr, std::memory_order_relaxed);

	std::array<const RE::NiCamera*, kMaxCascades> cameras{};
	uint32_t pointCount = 0;
	const auto smState = globals::game::smState;
	const auto shadowSceneNode = smState ? smState->shadowSceneNode[0] : nullptr;
	if (shadowSceneNode) {
		auto& ssnData = shadowSceneNode->GetRuntimeData();
		if (const auto sun = ssnData.sunShadowDirLight) {
			const auto& v = sun->GetShadowDirectionalLightRuntimeData().sunVector;
			const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
			const float s = len > 0.f ? std::clamp(std::abs(v.z) / len, kMinSunSin, 1.f) : 1.f;
			sunSin.store(s, std::memory_order_relaxed);
			sunCullAllowed.store(s >= std::sin(settings.SunMinElevation * (std::numbers::pi_v<float> / 180.f)), std::memory_order_relaxed);
			const auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
			for (uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i)
				cameras[i] = descriptors[i].camera.get();
		}
		for (const auto& light : ssnData.activeShadowLights) {
			if (!light || light.get() == ssnData.sunShadowDirLight)
				continue;
			for (const auto& descriptor : light->GetRuntimeData().shadowmapDescriptors) {
				if (pointCount < kMaxPointCameras && descriptor.camera)
					pointCameras[pointCount++].store(descriptor.camera.get(), std::memory_order_relaxed);
			}
		}
	}
	for (std::size_t i = 0; i < kMaxCascades; ++i)
		sunCameras[i].store(cameras[i], std::memory_order_relaxed);
	pointCameraCount.store(pointCount, std::memory_order_relaxed);
}

SceneCulling::CullerKind SceneCulling::Classify(const RE::BSCullingProcess* a_culler, uint32_t& a_cascade) const
{
	const auto camera = a_culler->camera;
	if (!camera)
		return CullerKind::Other;
	for (uint32_t i = 0; i < kMaxCascades; ++i) {
		if (sunCameras[i].load(std::memory_order_relaxed) == camera) {
			a_cascade = i;
			return CullerKind::Sun;
		}
	}
	const auto count = std::min<uint32_t>(pointCameraCount.load(std::memory_order_relaxed), kMaxPointCameras);
	for (uint32_t i = 0; i < count; ++i) {
		if (pointCameras[i].load(std::memory_order_relaxed) == camera)
			return CullerKind::Point;
	}
	return CullerKind::Other;
}

float SceneCulling::DistanceToCamera(const RE::NiBound& a_bound) const
{
	const float dx = a_bound.center.x - camX.load(std::memory_order_relaxed);
	const float dy = a_bound.center.y - camY.load(std::memory_order_relaxed);
	const float dz = a_bound.center.z - camZ.load(std::memory_order_relaxed);
	return std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius;
}

bool SceneCulling::ShouldCullBound(const RE::NiBound& a_bound, float a_minDistance, float a_maxRadius, float a_minAngularSize, float a_radiusScale) const
{
	if (a_bound.radius <= 0.f)
		return false;
	const float radius = a_bound.radius * a_radiusScale;
	if (radius >= a_maxRadius)
		return false;
	const float distance = DistanceToCamera(a_bound);
	if (distance <= a_minDistance)
		return false;
	return radius / distance < a_minAngularSize;
}

bool SceneCulling::ShouldCullSun(RE::BSGeometry& a_geometry, uint32_t a_cascade) const
{
	if (!settings.EnableSunShadowCulling || a_cascade < static_cast<uint32_t>(settings.SunMinCascade))
		return false;
	if (a_geometry.GetGeometryRuntimeData().skinInstance && BelongsToActor(a_geometry))
		return false;
	return ShouldCullBound(a_geometry.worldBound, settings.SunMinDistance, settings.SunMaxRadius, settings.SunMinAngularSize, 1.f / sunSin.load(std::memory_order_relaxed));
}

bool SceneCulling::ShouldCullPoint(const RE::NiCamera* a_lightCamera, RE::BSGeometry& a_geometry) const
{
	if (!settings.EnablePointShadowCulling || !pointCullAllowed.load(std::memory_order_relaxed))
		return false;
	if (a_geometry.GetGeometryRuntimeData().skinInstance && BelongsToActor(a_geometry))
		return false;
	const auto& bound = a_geometry.worldBound;
	if (a_lightCamera) {
		const auto& p = a_lightCamera->world.translate;
		const float dx = bound.center.x - p.x, dy = bound.center.y - p.y, dz = bound.center.z - p.z;
		const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - bound.radius;
		if (d <= 1.f || bound.radius / d >= kNearPointLightAngular)
			return false;
	}
	return ShouldCullBound(bound, settings.PointMinDistance, settings.PointMaxRadius, settings.PointMinAngularSize, 1.f);
}

bool SceneCulling::BelongsToActor(const RE::BSGeometry& a_geometry)
{
	const RE::NiAVObject* object = &a_geometry;
	for (int depth = 0; object && depth < 32; ++depth, object = object->parent) {
		if (const auto ref = object->GetUserData())
			return ref->GetFormType() == RE::FormType::ActorCharacter;
	}
	return false;
}

bool SceneCulling::IsActorRoot(const RE::NiAVObject* a_object)
{
	const auto ref = a_object->GetUserData();
	return ref && ref->GetFormType() == RE::FormType::ActorCharacter;
}

bool SceneCulling::ShouldCullActorShadow(RE::BSGeometry& a_geometry, bool a_pointLight)
{
	if (!settings.EnableActorShadowCulling || (a_pointLight && !settings.ActorShadowCullingPointLights))
		return false;
	if (!a_geometry.GetGeometryRuntimeData().skinInstance)
		return false;
	if (DistanceToCamera(a_geometry.worldBound) <= settings.ActorShadowDistance)
		return false;
	if (!BelongsToActor(a_geometry))
		return false;
	Count(counters.actorCulled);
	return true;
}

bool SceneCulling::ShouldCullDecal(RE::BSGeometry& a_geometry)
{
	if (!settings.EnableDecalCulling)
		return false;
	const auto property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	if (!property || !property->flags.any(kDecal, kDynamicDecal))
		return false;
	if (a_geometry.worldBound.radius >= settings.DecalMaxRadius)
		return false;
	if (DistanceToCamera(a_geometry.worldBound) <= settings.DecalMaxDistance)
		return false;
	Count(counters.decalCulled);
	return true;
}

bool SceneCulling::ShouldPrune(RE::BSCullingProcess* a_culler, RE::NiAVObject* a_object, bool a_parabolic)
{
	if (!settings.EnableSubtreePruning || tl_insideActor > 0 || a_parabolic)
		return false;
	const auto node = a_object ? a_object->AsNode() : nullptr;
	if (!node)
		return false;
	const auto& bound = node->worldBound;
	if (bound.radius <= 0.f)
		return false;

	if (a_culler->camera && a_culler->camera == precipCamera.load(std::memory_order_relaxed)) {
		auto& skylighting = globals::features::skylighting;
		if (!skylighting.loaded)
			return false;
		const float minRadius = skylighting.inOcclusion ? skylighting.settings.OcclusionMinRadius : Skylighting::kEngineMinOccluderRadius;
		if (bound.radius <= minRadius) {
			Count(counters.prunedPrecip);
			return true;
		}
		return false;
	}

	uint32_t cascade = 0;
	if (Classify(a_culler, cascade) != CullerKind::Sun)
		return false;
	if (!settings.EnableSunShadowCulling || !sunCullAllowed.load(std::memory_order_relaxed) || cascade < static_cast<uint32_t>(settings.SunMinCascade))
		return false;
	if (ShouldCullBound(bound, settings.SunMinDistance, settings.SunMaxRadius, settings.SunMinAngularSize, 1.f / sunSin.load(std::memory_order_relaxed))) {
		Count(counters.prunedSun);
		return true;
	}
	return false;
}

bool SceneCulling::ShouldSkipLightGather(RE::BSLight* a_light)
{
	if (!settings.EnableLightGatherThrottle || !a_light || !a_light->light)
		return false;

	const auto now = std::chrono::steady_clock::now();
	const auto& pos = a_light->worldTranslate;
	const float radius = a_light->light->GetLightRuntimeData().radius.x;

	std::scoped_lock lock(lightGatherLock);
	if (const auto it = lightGatherState.find(a_light); it != lightGatherState.end()) {
		const auto& e = it->second;
		const float moved = pos.GetDistance(e.position);
		const float radiusChange = std::abs(radius - e.radius);
		const float ageMs = std::chrono::duration<float, std::milli>(now - e.time).count();
		if (e.niLight == a_light->light.get() && moved < settings.LightGatherMinMove && radiusChange < settings.LightGatherMinRadiusChange && ageMs < settings.LightGatherMaxAgeMs) {
			Count(counters.lightGatherSkipped);
			return true;
		}
	}
	lightGatherState[a_light] = { a_light->light.get(), pos, radius, now };

	if (now - lightGatherCleanup > std::chrono::seconds(10)) {
		lightGatherCleanup = now;
		std::erase_if(lightGatherState, [&](const auto& a_entry) { return now - a_entry.second.time > std::chrono::seconds(10); });
	}
	return false;
}

void SceneCulling::Count(std::atomic<uint32_t>& a_counter)
{
	if (collectCounters.load(std::memory_order_relaxed))
		a_counter.fetch_add(1, std::memory_order_relaxed);
}

void SceneCulling::Hooks::BSCullingProcess_AppendVirtual::thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
{
	auto& sceneCulling = globals::features::sceneCulling;
	uint32_t cascade = 0;
	switch (sceneCulling.Classify(a_this, cascade)) {
	case CullerKind::Sun:
		if (sceneCulling.sunCullAllowed.load(std::memory_order_relaxed)) {
			if (sceneCulling.ShouldCullActorShadow(a_visible, false))
				return;
			if (sceneCulling.ShouldCullSun(a_visible, cascade)) {
				sceneCulling.Count(sceneCulling.counters.sunCulled);
				return;
			}
		}
		sceneCulling.Count(sceneCulling.counters.sunKept);
		break;
	case CullerKind::Point:
		if (sceneCulling.ShouldCullActorShadow(a_visible, true))
			return;
		if (sceneCulling.ShouldCullPoint(a_this->camera, a_visible)) {
			sceneCulling.Count(sceneCulling.counters.pointCulled);
			return;
		}
		sceneCulling.Count(sceneCulling.counters.pointKept);
		break;
	default:
		break;
	}
	func(a_this, a_visible, a_alphaGroupIndex);
}

void SceneCulling::Hooks::BSParabolicCullingProcess_AppendVirtual::thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
{
	auto& sceneCulling = globals::features::sceneCulling;
	if (sceneCulling.ShouldCullActorShadow(a_visible, true))
		return;
	if (sceneCulling.ShouldCullPoint(a_this->camera, a_visible)) {
		sceneCulling.Count(sceneCulling.counters.pointCulled);
		return;
	}
	sceneCulling.Count(sceneCulling.counters.pointKept);
	func(a_this, a_visible, a_alphaGroupIndex);
}

template <int N, bool Parabolic>
void SceneCulling::Hooks::CullingProcess_Process<N, Parabolic>::thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_alphaGroupIndex)
{
	auto& sceneCulling = globals::features::sceneCulling;
	if (a_object && IsActorRoot(a_object)) {
		++tl_insideActor;
		func(a_this, a_object, a_alphaGroupIndex);
		--tl_insideActor;
		return;
	}
	if (sceneCulling.ShouldPrune(a_this, a_object, Parabolic))
		return;
	func(a_this, a_object, a_alphaGroupIndex);
}

RE::BSShaderProperty::RenderPassArray* SceneCulling::Hooks::BSLightingShaderProperty_GetRenderPasses::thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags, RE::BSShaderAccumulator* a_accumulator)
{
	auto passes = func(a_property, a_geometry, a_renderFlags, a_accumulator);
	if (passes && passes->head && a_geometry && globals::features::sceneCulling.ShouldCullDecal(*a_geometry))
		passes->Clear();
	return passes;
}

void SceneCulling::Hooks::ShadowSceneNode_UpdateLightGeometry::thunk(RE::ShadowSceneNode* a_sceneNode, RE::BSLight* a_light)
{
	auto& sceneCulling = globals::features::sceneCulling;
	if (sceneCulling.ShouldSkipLightGather(a_light))
		return;
	sceneCulling.Count(sceneCulling.counters.lightGatherCalls);
	func(a_sceneNode, a_light);
}

void SceneCulling::Hooks::Install()
{
	stl::write_vfunc<0x18, BSCullingProcess_AppendVirtual>(RE::VTABLE_BSCullingProcess[0]);
	stl::write_vfunc<0x18, BSParabolicCullingProcess_AppendVirtual>(RE::VTABLE_BSParabolicCullingProcess[0]);
	stl::write_vfunc<0x16, CullingProcess_Process<0, false>>(RE::VTABLE_BSCullingProcess[0]);
	stl::write_vfunc<0x16, CullingProcess_Process<1, true>>(RE::VTABLE_BSParabolicCullingProcess[0]);
	stl::write_vfunc<0x16, CullingProcess_Process<2, false>>(RE::VTABLE_BSGeometryListCullingProcess[0]);

	const auto site = REL::RelocationID(99746, 106335).address() + REL::Relocate(0xB3, 0x6E2);
	const auto target = REL::RelocationID(99708, 106342).address();
	const auto* code = reinterpret_cast<const std::uint8_t*>(site);
	const bool isCall = code[0] == 0xE8 && site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1) == target;
	if (isCall) {
		stl::write_thunk_call<ShadowSceneNode_UpdateLightGeometry>(site);
		globals::features::sceneCulling.lightGatherHooked = true;
	} else {
		logger::warn("[Scene Culling] Light assignment call site at 0x{:X} does not call 0x{:X}; light gather throttle disabled", site, target);
	}

	logger::info("[Scene Culling] Installed culling hooks (light gather throttle {})", isCall ? "active" : "inactive");
}

void SceneCulling::Hooks::InstallLate()
{
	stl::write_vfunc<0x2A, BSLightingShaderProperty_GetRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
	logger::info("[Scene Culling] Installed decal culling hook");
}
