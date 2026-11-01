#include "ProceduralLightning.h"

#include "Deferred.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.procedural_lightning."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralLightning::Settings,
	Enabled,
	UseWeatherColor,
	Color,
	Intensity,
	Width,
	CloudGlow,
	FogAbsorption,
	Duration,
	MaxBoltsPerFlash,
	MaxRestrikes,
	MinDistance,
	MaxDistance,
	CloudHeight,
	ViewBias,
	Alpha,
	Beta,
	Gamma,
	MaxDepth,
	MaxBranchDepth)

namespace
{
	constexpr size_t kMaxSegments = 16384;
	constexpr size_t kMaxSegmentsPerBolt = 8192;
	constexpr size_t kMinSegmentBudget = 256;

	constexpr float kMaxDeltaTime = 0.1f;
	constexpr float kPausedPreviewStep = 1.0f / 60.0f;
	constexpr float kBaseWidth = 60.0f;
	constexpr float kBranchWidthFalloff = 0.7f;
	constexpr float kCloudFadeArrival = 0.12f;
	constexpr float kGlowRadiusScale = 0.55f;
	constexpr float kGlowDrop = 0.12f;
	constexpr float kCloudGlowScale = 0.05f;
	constexpr float kBoltLean = 0.08f;
	constexpr float kViewSpread = 0.7f;
	constexpr float kWidthToGlow = 4.0f;
	constexpr float kMinSigma = 0.75f;
	constexpr float kMaxSigmaAt1080p = 5.0f;
	constexpr float kReferenceHeight = 1080.0f;

	constexpr float kLeaderTime = 0.045f;
	constexpr float kLeaderBrightness = 0.16f;
	constexpr float kLeaderBranchScale = 0.6f;
	constexpr float kStrokeDecay = 0.045f;
	constexpr float kGlowDecay = 0.09f;
	constexpr float kRestrikeGapMin = 0.045f;
	constexpr float kRestrikeGapMax = 0.13f;
	constexpr float kRestrikeBranch = 0.15f;
	constexpr float kBoltStagger = 0.14f;
	constexpr float kFullyRevealed = 8.0f;

	constexpr uint32_t kBranchDepthShift = 8;

	struct ShapePreset
	{
		float Alpha;
		float Beta;
		float Gamma;
		uint MaxDepth;
		uint MaxBranchDepth;
	};

	constexpr std::array<ShapePreset, 6> kShapePresets{ {
		{ 0.5f, 0.4f, 0.3f, 8, 3 },
		{ 0.3f, 0.3f, 0.5f, 10, 4 },
		{ 0.7f, 0.5f, 0.2f, 6, 2 },
		{ 0.4f, 0.6f, 0.6f, 12, 5 },
		{ 0.6f, 0.2f, 0.15f, 7, 2 },
		{ 0.35f, 0.45f, 0.55f, 9, 4 },
	} };

	float Smoothstep(float a_edge, float a_value)
	{
		const float t = std::clamp(a_value / a_edge, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			globals::features::proceduralLightning.Render(a_accumulator);
			func(a_accumulator, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void ProceduralLightning::SetupResources()
{
	auto device = globals::d3d::device;

	segmentBuffer = eastl::make_unique<Buffer>(StructuredBufferDesc<Segment>(kMaxSegments, false, true), nullptr, "ProceduralLightning::Segments");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = static_cast<UINT>(kMaxSegments);
		segmentBuffer->CreateSRV(srvDesc);
	}

	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "ProceduralLightning::FrameData");

	{
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;

		auto& color = blendDesc.RenderTarget[0];
		color.BlendEnable = TRUE;
		color.SrcBlend = D3D11_BLEND_ONE;
		color.DestBlend = D3D11_BLEND_ONE;
		color.BlendOp = D3D11_BLEND_OP_ADD;
		color.SrcBlendAlpha = D3D11_BLEND_ZERO;
		color.DestBlendAlpha = D3D11_BLEND_ONE;
		color.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		color.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

		auto& mask = blendDesc.RenderTarget[1];
		mask.BlendEnable = TRUE;
		mask.SrcBlend = D3D11_BLEND_ONE;
		mask.DestBlend = D3D11_BLEND_ONE;
		mask.BlendOp = D3D11_BLEND_OP_MAX;
		mask.SrcBlendAlpha = D3D11_BLEND_ONE;
		mask.DestBlendAlpha = D3D11_BLEND_ONE;
		mask.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		mask.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "ProceduralLightning::BlendState");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "ProceduralLightning::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "ProceduralLightning::DepthStencilState");
	}

	GetBoltVS();
	GetBoltPS();
}

void ProceduralLightning::ClearShaderCache()
{
	boltVS = nullptr;
	boltPS = nullptr;
	shaderCompileFailed = false;

	GetBoltVS();
	GetBoltPS();
}

ID3D11VertexShader* ProceduralLightning::GetBoltVS()
{
	if (!boltVS && !shaderCompileFailed) {
		boltVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralLightning\\BoltVS.hlsl", {}, "vs_5_0")));
		shaderCompileFailed = !boltVS;
	}
	return boltVS.get();
}

ID3D11PixelShader* ProceduralLightning::GetBoltPS()
{
	if (!boltPS && !shaderCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		boltPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralLightning\\BoltPS.hlsl", defines, "ps_5_0")));
		shaderCompileFailed = !boltPS;
	}
	return boltPS.get();
}

void ProceduralLightning::PostPostLoad()
{
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));
	logger::info("[ProceduralLightning] Installed hooks");
}

float ProceduralLightning::Uniform(float a_min, float a_max)
{
	return std::uniform_real_distribution<float>(a_min, a_max)(rng);
}

float ProceduralLightning::GetViewYaw()
{
	if (auto camera = RE::Main::WorldRootCamera()) {
		const auto& worldToCam = camera->GetRuntimeData().worldToCam;
		return std::atan2(worldToCam[3][1], worldToCam[3][0]);
	}
	return Uniform(0.0f, Lightning::kTau);
}

float ProceduralLightning::GetStrikeFloor(float a_cameraZ) const
{
	constexpr float kFallbackDrop = 40000.0f;
	constexpr float kMaxDrop = 200000.0f;

	auto player = globals::game::player;
	auto world = player ? player->GetWorldspace() : nullptr;
	if (world) {
		const float waterHeight = world->GetDefaultWaterHeight();
		if (std::isfinite(waterHeight) && waterHeight < a_cameraZ && waterHeight > a_cameraZ - kMaxDrop)
			return waterHeight;
	}
	return a_cameraZ - kFallbackDrop;
}

float4 ProceduralLightning::GetBoltColor(const RE::Sky* a_sky) const
{
	if (settings.UseWeatherColor) {
		const auto* weather = a_sky->lastWeather && a_sky->currentWeatherPct < 0.5f ? a_sky->lastWeather : a_sky->currentWeather;
		if (weather) {
			const auto& color = weather->data.lightningColor;
			if (color.red || color.green || color.blue)
				return { color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, 1.0f };
		}
	}
	return { settings.Color[0], settings.Color[1], settings.Color[2], 1.0f };
}

bool ProceduralLightning::SpawnStrike(const RE::Sky* a_sky, bool a_preview)
{
	const auto slot = std::find_if(strikes.begin(), strikes.end(), [](const Strike& a_strike) { return !a_strike.Active; });
	if (slot == strikes.end() || segments.size() + kMinSegmentBudget > kMaxSegments)
		return false;

	const RE::NiPoint3 camera = Util::GetEyePosition();
	const float yaw = Uniform(0.0f, 1.0f) < settings.ViewBias ? GetViewYaw() + Uniform(-kViewSpread, kViewSpread) : Uniform(0.0f, Lightning::kTau);
	const float distance = Uniform(settings.MinDistance, settings.MaxDistance);
	const float floorZ = GetStrikeFloor(camera.z);
	const float topZ = camera.z + settings.CloudHeight;
	const float height = topZ - floorZ;

	const float3 end{ camera.x + std::cos(yaw) * distance, camera.y + std::sin(yaw) * distance, floorZ };
	const float3 start{ end.x + Uniform(-kBoltLean, kBoltLean) * height, end.y + Uniform(-kBoltLean, kBoltLean) * height, topZ };

	const Lightning::BoltConfig config{ settings.Alpha, settings.Beta, settings.Gamma, settings.MaxDepth, settings.MaxBranchDepth };
	generated.clear();
	Lightning::GenerateBolt(start, end, config, std::min(kMaxSegmentsPerBolt, kMaxSegments - segments.size()), rng, generated);
	if (generated.empty())
		return false;

	const uint32_t index = static_cast<uint32_t>(std::distance(strikes.begin(), slot));
	const float widthScale = kBaseWidth * settings.Width;
	segments.reserve(segments.size() + generated.size());
	for (const auto& bolt : generated) {
		const float width = widthScale * (0.35f + 0.65f * std::max(bolt.Start.Energy, bolt.End.Energy)) * std::pow(kBranchWidthFalloff, static_cast<float>(bolt.BranchDepth));
		segments.push_back({ bolt.Start.Position,
			bolt.Start.Energy * Smoothstep(kCloudFadeArrival, bolt.Start.Arrival),
			bolt.End.Position,
			bolt.End.Energy * Smoothstep(kCloudFadeArrival, bolt.End.Arrival),
			bolt.Start.Arrival,
			bolt.End.Arrival,
			width,
			index | (bolt.BranchDepth << kBranchDepthShift) });
	}
	segmentsDirty = true;

	Strike& strike = *slot;
	strike = {};
	strike.Active = true;
	strike.Preview = a_preview;
	strike.Age = a_preview ? 0.0f : -Uniform(0.0f, kBoltStagger);
	strike.Color = GetBoltColor(a_sky);
	strike.Glow = { start.x, start.y, start.z - height * kGlowDrop, height * kGlowRadiusScale };

	const uint32_t restrikes = std::uniform_int_distribution<uint32_t>(0, settings.MaxRestrikes)(rng);
	float time = kLeaderTime * settings.Duration;
	strike.Strokes[0] = { time, 1.0f };
	strike.StrokeCount = 1;
	for (uint32_t i = 0; i < restrikes && strike.StrokeCount < kMaxStrokes; ++i) {
		time += Uniform(kRestrikeGapMin, kRestrikeGapMax) * settings.Duration;
		strike.Strokes[strike.StrokeCount++] = { time, Uniform(0.45f, 0.85f) * std::pow(0.9f, static_cast<float>(i)) };
	}
	strike.Lifetime = time + kGlowDecay * 4.0f * settings.Duration;

	debug.strikes++;
	debug.lastSegments = static_cast<uint32_t>(generated.size());
	return true;
}

void ProceduralLightning::SpawnBolts(const RE::Sky* a_sky, bool a_preview)
{
	const uint32_t bolts = std::uniform_int_distribution<uint32_t>(1, std::max(settings.MaxBoltsPerFlash, 1u))(rng);
	for (uint32_t i = 0; i < bolts; ++i) {
		if (!SpawnStrike(a_sky, a_preview))
			return;
	}
}

void ProceduralLightning::PollEngineFlash(const RE::Sky* a_sky)
{
	const uint32_t flashStartTime = a_sky->pad1CC;
	if (flashStartTime == lastFlashStartTime)
		return;
	lastFlashStartTime = flashStartTime;
	if (a_sky->flash > 0.0f)
		SpawnBolts(a_sky, false);
}

void ProceduralLightning::UpdateStrikes(float a_deltaTime)
{
	frameStrikes = {};
	for (uint32_t i = 0; i < kMaxStrikes; ++i) {
		Strike& strike = strikes[i];
		if (!strike.Active)
			continue;

		const float previousAge = strike.Age;
		strike.Age += a_deltaTime;
		if (strike.Age >= strike.Lifetime) {
			strike.Active = false;
			std::erase_if(segments, [i](const Segment& a_segment) { return (a_segment.Packed & ((1u << kBranchDepthShift) - 1u)) == i; });
			segmentsDirty = true;
			continue;
		}
		if (strike.Age < 0.0f)
			continue;

		StrikeData& data = frameStrikes[i];
		data.Color = strike.Color;
		data.Glow = strike.Glow;

		const float returnStroke = strike.Strokes[0].Time;
		if (strike.Age < returnStroke) {
			data.Main = kLeaderBrightness;
			data.Branch = kLeaderBrightness * kLeaderBranchScale;
			data.Front = strike.Age / returnStroke;
			continue;
		}

		data.Front = kFullyRevealed;
		float glow = 0.0f;
		for (uint32_t stroke = 0; stroke < strike.StrokeCount; ++stroke) {
			const auto& current = strike.Strokes[stroke];
			if (strike.Age < current.Time)
				break;

			const float since = previousAge < current.Time ? 0.0f : strike.Age - current.Time;
			const float channel = current.Amplitude * std::exp(-since / (kStrokeDecay * settings.Duration));
			data.Main = std::max(data.Main, channel);
			data.Branch = std::max(data.Branch, stroke == 0 ? channel : channel * kRestrikeBranch);
			glow = std::max(glow, current.Amplitude * std::exp(-since / (kGlowDecay * settings.Duration)));
		}
		data.CloudGlow = glow * settings.CloudGlow * kCloudGlowScale;
	}
}

void ProceduralLightning::ClearStrikes()
{
	strikes = {};
	segments.clear();
	segmentsDirty = false;
}

void ProceduralLightning::ClearTemporalMask()
{
	temporalMaskDirty = false;
	auto renderer = globals::game::renderer;
	if (!renderer)
		return;
	auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
	if (mask.RTV) {
		const FLOAT clearColor[4] = {};
		globals::d3d::context->ClearRenderTargetView(mask.RTV, clearColor);
	}
}

void ProceduralLightning::Render(RE::BSShaderAccumulator* a_accumulator)
{
	if (!loaded || !a_accumulator)
		return;

	auto state = globals::state;
	if (!state || !state->inWorld || !globals::shaderCache->IsEnabled())
		return;
	if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
		return;

	auto smState = globals::game::smState;
	const auto* accumulatorData = a_accumulator->GetRuntimeData();
	if (!accumulatorData || accumulatorData->firstPerson || !smState || accumulatorData->activeShadowSceneNode != smState->shadowSceneNode[0])
		return;

	if (lastRenderedFrame == state->frameCount)
		return;
	lastRenderedFrame = state->frameCount;

	auto sky = globals::game::sky;
	const bool exterior = sky && sky->mode.get() == RE::Sky::Mode::kFull;
	if (!settings.Enabled || !exterior)
		ClearStrikes();

	if (settings.Enabled && exterior) {
		PollEngineFlash(sky);
		if (testStrikeQueued)
			SpawnBolts(sky, true);
	}
	testStrikeQueued = false;

	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const bool previewing = std::any_of(strikes.begin(), strikes.end(), [](const Strike& a_strike) { return a_strike.Active && a_strike.Preview; });
	UpdateStrikes(paused ? (previewing ? kPausedPreviewStep : 0.0f) : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxDeltaTime));

	const bool active = std::any_of(strikes.begin(), strikes.end(), [](const Strike& a_strike) { return a_strike.Active; });
	if (active)
		Draw();
	else if (temporalMaskDirty)
		ClearTemporalMask();
}

void ProceduralLightning::Draw()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!context || !renderer || !segmentBuffer || !frameConstants)
		return;

	auto vertexShader = GetBoltVS();
	auto pixelShader = GetBoltPS();
	if (!vertexShader || !pixelShader)
		return;

	auto& runtimeData = renderer->GetRuntimeData();
	auto mainTarget = runtimeData.renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto temporalMask = runtimeData.renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
	auto sceneDepth = Util::GetCurrentSceneDepthSRV(false);
	if (!mainTarget.RTV || !mainTarget.texture || !sceneDepth)
		return;

	D3D11_TEXTURE2D_DESC textureDesc{};
	mainTarget.texture->GetDesc(&textureDesc);

	bool useTemporalMask = temporalMask.RTV && temporalMask.texture;
	if (useTemporalMask) {
		D3D11_TEXTURE2D_DESC maskDesc{};
		temporalMask.texture->GetDesc(&maskDesc);
		useTemporalMask = maskDesc.Width == textureDesc.Width && maskDesc.Height == textureDesc.Height;
	}

	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(context);

	auto profiler = globals::profiler;
	profiler->BeginPass("ProceduralLightning");

	if (temporalMaskDirty) {
		const FLOAT clearColor[4] = {};
		context->ClearRenderTargetView(temporalMask.RTV, clearColor);
	}

	if (segmentsDirty) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(context->Map(segmentBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		if (!segments.empty())
			memcpy_s(mapped.pData, sizeof(Segment) * kMaxSegments, segments.data(), sizeof(Segment) * segments.size());
		context->Unmap(segmentBuffer->resource.get(), 0);
		segmentsDirty = false;
	}

	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height) });

	FrameData frameData{};
	std::copy(frameStrikes.begin(), frameStrikes.end(), frameData.Strikes);
	if (auto smState = globals::game::smState) {
		if (auto sceneNode = smState->shadowSceneNode[0]) {
			if (auto& fog = sceneNode->GetRuntimeData().fogProperty) {
				const float range = fog->farDistance - fog->nearDistance;
				if (range > 1.0f && std::isfinite(range))
					frameData.FogParams = { fog->nearDistance / range, 1.0f / range, std::max(fog->power, 0.0f), std::clamp(fog->clamp, 0.0f, 1.0f) };
			}
		}
	}
	frameData.RenderSize = resolution;
	frameData.Intensity = settings.Intensity;
	frameData.FogAbsorption = settings.FogAbsorption;
	frameData.MinSigma = kMinSigma;
	frameData.MaxSigma = std::max(kMaxSigmaAt1080p * resolution.y / kReferenceHeight, kMinSigma);
	frameData.SegmentCount = static_cast<uint32_t>(segments.size());
	frameData.WidthToGlow = kWidthToGlow;
	frameConstants->Update(frameData);

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* renderTargets[2] = { mainTarget.RTV, useTemporalMask ? temporalMask.RTV : nullptr };
	context->OMSetRenderTargets(useTemporalMask ? 2 : 1, renderTargets, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11Buffer* perFrame = *globals::game::perFrame.get();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };

	ID3D11ShaderResourceView* segmentSRV = segmentBuffer->srv.get();
	context->VSSetConstantBuffers(0, 1, &frameBuffer);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->VSSetShaderResources(0, 1, &segmentSRV);
	context->VSSetShader(vertexShader, nullptr, 0);

	context->PSSetConstantBuffers(0, 1, &frameBuffer);
	context->PSSetConstantBuffers(5, 2, sharedBuffers);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetShaderResources(0, 1, &sceneDepth);
	context->PSSetShader(pixelShader, nullptr, 0);

	context->DrawInstanced(4, static_cast<UINT>(segments.size()) + kMaxStrikes, 0, 0);

	profiler->EndPass();
	backup.Restore(context);

	temporalMaskDirty = temporalMaskDirty || useTemporalMask;
	debug.renderedFrames++;
}

int ProceduralLightning::GetActiveShapePreset() const
{
	for (size_t i = 0; i < kShapePresets.size(); ++i) {
		const auto& preset = kShapePresets[i];
		if (preset.Alpha == settings.Alpha && preset.Beta == settings.Beta && preset.Gamma == settings.Gamma && preset.MaxDepth == settings.MaxDepth && preset.MaxBranchDepth == settings.MaxBranchDepth)
			return static_cast<int>(i);
	}
	return -1;
}

void ProceduralLightning::ApplyShapePreset(int a_preset)
{
	const auto& preset = kShapePresets[static_cast<size_t>(a_preset)];
	settings.Alpha = preset.Alpha;
	settings.Beta = preset.Beta;
	settings.Gamma = preset.Gamma;
	settings.MaxDepth = preset.MaxDepth;
	settings.MaxBranchDepth = preset.MaxBranchDepth;
}

void ProceduralLightning::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Bolts appear with the game's lightning flashes, so only in thunderstorm weathers and outdoors."));

	if (ImGui::TreeNodeEx(T(TKEY("appearance"), "Appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("intensity"), "Brightness"), &settings.Intensity, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("width"), "Width"), &settings.Width, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("cloud_glow"), "Cloud Glow"), &settings.CloudGlow, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("cloud_glow_tooltip"), "Soft light in the clouds around the top of each bolt. 0 turns it off."));
		ImGui::SliderFloat(T(TKEY("fog_absorption"), "Fog Absorption"), &settings.FogAbsorption, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("fog_absorption_tooltip"), "How much weather fog dims distant bolts."));
		ImGui::SliderFloat(T(TKEY("duration"), "Duration"), &settings.Duration, 0.5f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

		ImGui::Checkbox(T(TKEY("use_weather_color"), "Use Weather Lightning Color"), &settings.UseWeatherColor);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("use_weather_color_tooltip"), "Tints bolts with the current weather's lightning color. Falls back to the color below if the weather has none."));
		ImGui::ColorEdit3(T(TKEY("color"), "Bolt Color"), settings.Color.data());
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("strikes"), "Strikes"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderInt(T(TKEY("max_bolts"), "Max Bolts per Flash"), reinterpret_cast<int*>(&settings.MaxBoltsPerFlash), 1, static_cast<int>(kMaxStrikes), "%d", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderInt(T(TKEY("max_restrikes"), "Max Re-strikes"), reinterpret_cast<int*>(&settings.MaxRestrikes), 0, static_cast<int>(kMaxStrokes) - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("max_restrikes_tooltip"), "Extra pulses down the same channel, which makes the bolt flicker."));

		ImGui::SliderFloat(T(TKEY("min_distance"), "Min Distance"), &settings.MinDistance, 10000.0f, 300000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("max_distance"), "Max Distance"), &settings.MaxDistance, 10000.0f, 300000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		settings.MaxDistance = std::max(settings.MaxDistance, settings.MinDistance);
		ImGui::SliderFloat(T(TKEY("cloud_height"), "Cloud Height"), &settings.CloudHeight, 20000.0f, 150000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("cloud_height_tooltip"), "How far above the camera bolts start, in game units."));
		ImGui::SliderFloat(T(TKEY("view_bias"), "Strike in View"), &settings.ViewBias, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("view_bias_tooltip"), "Chance a bolt lands in the direction you are looking instead of anywhere around you."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("shape"), "Shape"))) {
		const char* presetNames[] = {
			T(TKEY("preset_classic"), "Classic"),
			T(TKEY("preset_dense"), "Dense"),
			T(TKEY("preset_sparse"), "Sparse"),
			T(TKEY("preset_chaotic"), "Chaotic"),
			T(TKEY("preset_smooth"), "Smooth"),
			T(TKEY("preset_branchy"), "Branchy")
		};
		static_assert(sizeof(presetNames) / sizeof(presetNames[0]) == kShapePresets.size());

		const int activePreset = GetActiveShapePreset();
		if (ImGui::BeginCombo(T(TKEY("preset"), "Preset"), activePreset >= 0 ? presetNames[activePreset] : T(TKEY("preset_custom"), "Custom"))) {
			for (int i = 0; i < IM_ARRAYSIZE(presetNames); ++i) {
				if (ImGui::Selectable(presetNames[i], i == activePreset))
					ApplyShapePreset(i);
			}
			ImGui::EndCombo();
		}

		ImGui::SliderFloat(T(TKEY("beta"), "Jaggedness"), &settings.Beta, 0.05f, 0.8f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("gamma"), "Branching"), &settings.Gamma, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("alpha"), "Branch Decay"), &settings.Alpha, 0.1f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("alpha_tooltip"), "Higher values make forks rarer and shorter further down the bolt."));
		ImGui::SliderInt(T(TKEY("max_depth"), "Detail"), reinterpret_cast<int*>(&settings.MaxDepth), 4, 12, "%d", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderInt(T(TKEY("max_branch_depth"), "Fork Levels"), reinterpret_cast<int*>(&settings.MaxBranchDepth), 0, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"))) {
		if (Util::Button(T(TKEY("test_strike"), "Trigger Test Strike")))
			testStrikeQueued = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("test_strike_tooltip"), "Fires a bolt now, in any weather. Outdoors only."));

		ImGui::Text("%s: %llu", T(TKEY("total_strikes"), "Bolts drawn"), debug.strikes);
		ImGui::Text("%s: %u", T(TKEY("last_segments"), "Last bolt segments"), debug.lastSegments);
		ImGui::Text("%s: %llu", T(TKEY("rendered_frames"), "Rendered frames"), debug.renderedFrames);
		ImGui::TreePop();
	}
}

void ProceduralLightning::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.Intensity, defaults.Intensity, 1.0f, 64.0f);
	sanitize(settings.Width, defaults.Width, 0.25f, 4.0f);
	sanitize(settings.CloudGlow, defaults.CloudGlow, 0.0f, 3.0f);
	sanitize(settings.FogAbsorption, defaults.FogAbsorption, 0.0f, 1.0f);
	sanitize(settings.Duration, defaults.Duration, 0.5f, 3.0f);
	sanitize(settings.MinDistance, defaults.MinDistance, 10000.0f, 300000.0f);
	sanitize(settings.MaxDistance, defaults.MaxDistance, 10000.0f, 300000.0f);
	sanitize(settings.CloudHeight, defaults.CloudHeight, 20000.0f, 150000.0f);
	sanitize(settings.ViewBias, defaults.ViewBias, 0.0f, 1.0f);
	sanitize(settings.Alpha, defaults.Alpha, 0.1f, 1.0f);
	sanitize(settings.Beta, defaults.Beta, 0.05f, 0.8f);
	sanitize(settings.Gamma, defaults.Gamma, 0.0f, 1.0f);
	for (size_t i = 0; i < settings.Color.size(); ++i)
		sanitize(settings.Color[i], defaults.Color[i], 0.0f, 1.0f);
	settings.MaxDistance = std::max(settings.MaxDistance, settings.MinDistance);
	settings.MaxBoltsPerFlash = std::clamp<uint>(settings.MaxBoltsPerFlash, 1, kMaxStrikes);
	settings.MaxRestrikes = std::clamp<uint>(settings.MaxRestrikes, 0, kMaxStrokes - 1);
	settings.MaxDepth = std::clamp<uint>(settings.MaxDepth, 4, 12);
	settings.MaxBranchDepth = std::clamp<uint>(settings.MaxBranchDepth, 0, 5);
}

void ProceduralLightning::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ProceduralLightning::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
