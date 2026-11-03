#include "CapsuleShadows.h"

#include "Deferred.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.capsule_shadows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CapsuleShadows::Settings,
	Enabled,
	AmbientStrength,
	ShadowStrength,
	MaxDistance)

namespace
{
	constexpr float kMinDistance = 512.0f;
	constexpr float kMaxDistance = 4096.0f;
	constexpr float kShadowReach = 160.0f;
	constexpr float kReferenceHeight = 128.0f;
	constexpr float kMinBoundRadius = 8.0f;
	constexpr float kMaxBoundRadius = 200.0f;
	constexpr float kMinLightWeight = 0.02f;
	constexpr float kLightSmoothing = 0.25f;
	constexpr uint32_t kForgetFrames = 120;

	struct BoneSegment
	{
		const char* start;
		const char* end;
		float radius;
	};

	constexpr std::array<BoneSegment, 12> kHumanoidSegments{ {
		{ "NPC Pelvis [Pelv]", "NPC Spine2 [Spn2]", 12.0f },
		{ "NPC Spine2 [Spn2]", "NPC Neck [Neck]", 13.0f },
		{ "NPC Head [Head]", nullptr, 9.0f },
		{ "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", 4.5f },
		{ "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", 3.5f },
		{ "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", 4.5f },
		{ "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", 3.5f },
		{ "NPC L Thigh [LThg]", "NPC L Calf [LClf]", 6.5f },
		{ "NPC L Calf [LClf]", "NPC L Foot [Lft ]", 4.5f },
		{ "NPC R Thigh [RThg]", "NPC R Calf [RClf]", 6.5f },
		{ "NPC R Calf [RClf]", "NPC R Foot [Rft ]", 4.5f },
		{ "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", 12.0f },
	} };

	float3 ToFloat3(const RE::NiPoint3& a_point)
	{
		return { a_point.x, a_point.y, a_point.z };
	}
}

void CapsuleShadows::SetupResources()
{
	auto createStructured = [](eastl::unique_ptr<Buffer>& a_buffer, D3D11_BUFFER_DESC a_desc, uint32_t a_count, const char* a_name) {
		a_buffer = eastl::make_unique<Buffer>(a_desc, nullptr, a_name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = a_count;
		a_buffer->CreateSRV(srvDesc);
	};

	createStructured(capsuleBuffer, StructuredBufferDesc<Capsule>(kMaxCapsules, false, true), kMaxCapsules, "CapsuleShadows::Capsules");
	createStructured(actorBuffer, StructuredBufferDesc<ActorData>(kMaxActors, false, true), kMaxActors, "CapsuleShadows::Actors");
	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "CapsuleShadows::FrameData");
}

void CapsuleShadows::ClearShaderCache()
{
	computeShader = nullptr;
	shaderCompileFailed = false;
}

ID3D11ComputeShader* CapsuleShadows::GetShader()
{
	if (!computeShader && !shaderCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		computeShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CapsuleShadows\\CapsuleShadowsCS.hlsl", defines, "cs_5_0")));
		shaderCompileFailed = !computeShader;
	}
	return computeShader.get();
}

void CapsuleShadows::CollectLights()
{
	lights.clear();
	sunLuminance = 0.0f;

	auto sky = globals::game::sky;
	if (sky && !Util::IsInterior()) {
		const auto& sun = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
		sunLuminance = (sun.red + sun.green + sun.blue) / 3.0f;
	}

	auto smState = globals::game::smState;
	auto sceneNode = smState ? smState->shadowSceneNode[0] : nullptr;
	if (!sceneNode)
		return;

	for (const auto& entry : sceneNode->GetRuntimeData().activeLights) {
		auto* bsLight = entry.get();
		if (!bsLight || bsLight->IsShadowLight())
			continue;
		auto* niLight = bsLight->light.get();
		if (!niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			continue;
		const auto& data = niLight->GetLightRuntimeData();
		const float luminance = (data.diffuse.red + data.diffuse.green + data.diffuse.blue) / 3.0f * data.fade * bsLight->lodDimmer;
		if (luminance > 0.0f && data.radius.x > 1.0f)
			lights.push_back({ niLight->world.translate, data.radius.x, luminance });
	}
}

bool CapsuleShadows::FindDominantLight(const RE::NiPoint3& a_position, RE::NiPoint3& a_light, float& a_dominance) const
{
	float best = 0.0f;
	float total = sunLuminance;
	for (const auto& light : lights) {
		const float distanceSq = a_position.GetSquaredDistance(light.position);
		const float radiusSq = light.radius * light.radius;
		if (distanceSq >= radiusSq)
			continue;
		const float falloff = 1.0f - distanceSq / radiusSq;
		const float weight = light.luminance * falloff * falloff;
		total += weight;
		if (weight > best) {
			best = weight;
			a_light = light.position;
		}
	}
	a_dominance = total > 0.0f ? best / total : 0.0f;
	return best > kMinLightWeight;
}

uint32_t CapsuleShadows::AddSkeletonCapsules(RE::NiAVObject* a_root, float a_scale)
{
	static const auto names = [] {
		std::array<std::pair<RE::BSFixedString, RE::BSFixedString>, kHumanoidSegments.size()> result;
		for (size_t i = 0; i < kHumanoidSegments.size(); ++i) {
			result[i].first = kHumanoidSegments[i].start;
			if (kHumanoidSegments[i].end)
				result[i].second = kHumanoidSegments[i].end;
		}
		return result;
	}();

	uint32_t added = 0;
	for (size_t i = 0; i < kHumanoidSegments.size() && added < kMaxCapsulesPerActor && capsuleCount < kMaxCapsules; ++i) {
		auto* start = a_root->GetObjectByName(names[i].first);
		if (!start)
			continue;

		RE::NiPoint3 startPoint = start->world.translate;
		RE::NiPoint3 endPoint = startPoint;
		if (kHumanoidSegments[i].end) {
			auto* end = a_root->GetObjectByName(names[i].second);
			if (!end)
				continue;
			endPoint = end->world.translate;
		} else {
			endPoint.z += 8.0f * a_scale;
		}

		auto& capsule = capsules[capsuleCount++];
		capsule.A = ToFloat3(startPoint);
		capsule.B = ToFloat3(endPoint);
		capsule.Radius = kHumanoidSegments[i].radius * a_scale;
		++added;
	}
	return added;
}

void CapsuleShadows::AddBoundCapsule(RE::Actor* a_actor, RE::NiAVObject* a_root)
{
	if (capsuleCount >= kMaxCapsules)
		return;
	const float radius = a_root->worldBound.radius;
	if (radius < kMinBoundRadius)
		return;

	const float clamped = std::min(radius, kMaxBoundRadius);
	const float heading = a_actor->GetAngleZ();
	const RE::NiPoint3 forward{ std::sin(heading), std::cos(heading), 0.0f };
	const RE::NiPoint3& center = a_root->worldBound.center;

	auto& capsule = capsules[capsuleCount++];
	capsule.A = ToFloat3(center - forward * (clamped * 0.55f));
	capsule.B = ToFloat3(center + forward * (clamped * 0.55f));
	capsule.Radius = clamped * 0.4f;
}

void CapsuleShadows::AddActor(RE::Actor* a_actor, float a_deltaTime)
{
	if (actorCount >= kMaxActors)
		return;
	auto* root = a_actor->Get3D(false);
	if (!root)
		return;

	const float height = a_actor->GetHeight();
	const float scale = std::clamp(std::isfinite(height) && height > 0.0f ? height / kReferenceHeight : 1.0f, 0.35f, 4.0f);

	const uint32_t firstCapsule = capsuleCount;
	if (AddSkeletonCapsules(root, scale) == 0)
		AddBoundCapsule(a_actor, root);
	if (capsuleCount == firstCapsule)
		return;

	const RE::NiPoint3 center = a_actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, kReferenceHeight * 0.5f * scale };

	RE::NiPoint3 lightPosition = center;
	float dominance = 0.0f;
	const bool lit = FindDominantLight(center, lightPosition, dominance);

	auto& state = actorLights[a_actor->GetFormID()];
	const float blend = 1.0f - std::exp(-a_deltaTime / kLightSmoothing);
	if (state.frame == 0 || state.strength <= 0.0f)
		state.position = lightPosition;
	else if (lit)
		state.position += (lightPosition - state.position) * blend;
	state.strength += ((lit ? dominance : 0.0f) - state.strength) * blend;
	state.frame = globals::state->frameCount;

	auto& actor = actors[actorCount++];
	actor.Center = ToFloat3(center);
	actor.BoundRadius = kShadowReach + kReferenceHeight * 0.75f * scale;
	actor.LightPosition = ToFloat3(state.position);
	actor.LightStrength = state.strength;
	actor.FirstCapsule = firstCapsule;
	actor.CapsuleCount = capsuleCount - firstCapsule;
}

void CapsuleShadows::Prepass()
{
	actorCount = 0;
	capsuleCount = 0;

	if (!settings.Enabled || (settings.AmbientStrength <= 0.0f && settings.ShadowStrength <= 0.0f) || globals::state->isLoadingMenuOpen) {
		actorLights.clear();
		return;
	}

	const RE::NiPoint3 eye = Util::GetEyePosition();
	const float maxDistanceSq = settings.MaxDistance * settings.MaxDistance;

	candidates.clear();
	auto consider = [&](RE::ActorPtr a_actor) {
		if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDisabled())
			return;
		const float distanceSq = eye.GetSquaredDistance(a_actor->GetPosition());
		if (distanceSq <= maxDistanceSq)
			candidates.emplace_back(distanceSq, std::move(a_actor));
	};

	auto camera = RE::PlayerCamera::GetSingleton();
	if (auto player = globals::game::player; player && !(camera && camera->IsInFirstPerson()))
		consider(RE::ActorPtr(player));
	if (auto lists = RE::ProcessLists::GetSingleton()) {
		for (auto& handle : lists->highActorHandles)
			consider(handle.get());
	}

	if (candidates.empty()) {
		actorLights.clear();
		return;
	}

	const size_t count = std::min<size_t>(candidates.size(), kMaxActors);
	std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(), [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.first < a_rhs.first; });

	const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 1e-4f, 0.1f);
	CollectLights();
	for (size_t i = 0; i < count; ++i)
		AddActor(candidates[i].second.get(), deltaTime);
	candidates.clear();

	const uint32_t frame = globals::state->frameCount;
	for (auto it = actorLights.begin(); it != actorLights.end();)
		it = it->second.frame + kForgetFrames < frame ? actorLights.erase(it) : std::next(it);

	if (actorCount == 0 || !capsuleBuffer || !actorBuffer)
		return;

	auto context = globals::d3d::context;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	DX::ThrowIfFailed(context->Map(capsuleBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	memcpy_s(mapped.pData, sizeof(Capsule) * kMaxCapsules, capsules.data(), sizeof(Capsule) * capsuleCount);
	context->Unmap(capsuleBuffer->resource.get(), 0);

	DX::ThrowIfFailed(context->Map(actorBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	memcpy_s(mapped.pData, sizeof(ActorData) * kMaxActors, actors.data(), sizeof(ActorData) * actorCount);
	context->Unmap(actorBuffer->resource.get(), 0);
}

bool CapsuleShadows::EnsureTexture(uint32_t a_width, uint32_t a_height)
{
	if (occlusionTexture && occlusionTexture->desc.Width == a_width && occlusionTexture->desc.Height == a_height)
		return true;

	occlusionTexture.reset();

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_width;
	desc.Height = a_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8_UNORM;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

	try {
		occlusionTexture = eastl::make_unique<Texture2D>(desc, "CapsuleShadows::Occlusion");
		occlusionTexture->CreateSRV(srvDesc);
		occlusionTexture->CreateUAV(uavDesc);
	} catch (const std::exception& e) {
		logger::error("[CapsuleShadows] Failed to create the occlusion texture ({}), turning Capsule Shadows off", e.what());
		occlusionTexture.reset();
		settings.Enabled = false;
		return false;
	}
	return true;
}

void CapsuleShadows::Draw()
{
	occlusionValid = false;
	if (actorCount == 0 || !frameConstants)
		return;

	auto shader = GetShader();
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!shader || !context || !renderer)
		return;

	auto main = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto sceneDepth = Util::GetCurrentSceneDepthSRV(false);
	if (!main.texture || !normalRoughness.SRV || !sceneDepth)
		return;

	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	if (!EnsureTexture(std::max((mainDesc.Width + 1) / 2, 1u), std::max((mainDesc.Height + 1) / 2, 1u)))
		return;

	FrameData frame{};
	frame.ActorCount = actorCount;
	frame.AmbientStrength = settings.AmbientStrength;
	frame.ShadowStrength = settings.ShadowStrength;
	frameConstants->Update(frame);

	TracyD3D11Zone(globals::state->tracyCtx, "Capsule Shadows");
	globals::profiler->BeginPass("CapsuleShadows");

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ID3D11ShaderResourceView* resources[4] = { sceneDepth, normalRoughness.SRV, capsuleBuffer->srv.get(), actorBuffer->srv.get() };
	ID3D11UnorderedAccessView* output = occlusionTexture->uav.get();

	context->CSSetConstantBuffers(0, 1, &frameBuffer);
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	context->CSSetShaderResources(0, 4, resources);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetShader(shader, nullptr, 0);

	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height) });
	const uint32_t width = (static_cast<uint32_t>(resolution.x) + 1) / 2;
	const uint32_t height = (static_cast<uint32_t>(resolution.y) + 1) / 2;
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	ID3D11ShaderResourceView* nullResources[4] = {};
	ID3D11UnorderedAccessView* nullOutput = nullptr;
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetShaderResources(0, 4, nullResources);
	context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);

	globals::profiler->EndPass();
	occlusionValid = true;
}

ID3D11ShaderResourceView* CapsuleShadows::GetOcclusionSRV() const
{
	return occlusionValid && occlusionTexture ? occlusionTexture->srv.get() : nullptr;
}

void CapsuleShadows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	ImGui::SliderFloat(T(TKEY("ambient_strength"), "Contact Strength"), &settings.AmbientStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("ambient_strength_tooltip"), "How much characters darken the ambient light around them. This is what grounds them where nothing casts a real shadow."));

	ImGui::SliderFloat(T(TKEY("shadow_strength"), "Shadow Strength"), &settings.ShadowStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shadow_strength_tooltip"), "Strength of the soft shadow cast away from the strongest nearby light that has no shadows of its own."));

	ImGui::SliderFloat(T(TKEY("max_distance"), "Max Distance"), &settings.MaxDistance, kMinDistance, kMaxDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("max_distance_tooltip"), "Characters further than this from the camera (in game units) get no capsule shadows. Only the nearest eight are ever shadowed."));
}

void CapsuleShadows::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.AmbientStrength, defaults.AmbientStrength, 0.0f, 1.0f);
	sanitize(settings.ShadowStrength, defaults.ShadowStrength, 0.0f, 1.0f);
	sanitize(settings.MaxDistance, defaults.MaxDistance, kMinDistance, kMaxDistance);
}

void CapsuleShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CapsuleShadows::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
