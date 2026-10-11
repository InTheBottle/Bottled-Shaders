#include "ScreenSpaceShadows.h"
#include "Features/ReverseZ.h"

#include "Features/TerrainBlending.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.screen_space_shadows."

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	AdaptiveReach,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable screen-space contact shadows from the sun/moon direction."));

		ImGui::SliderInt(T(TKEY("sample_count"), "Sample Count Multiplier"), (int*)&bendSettings.SampleCount, 1, 4, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sample_count_tooltip"), "Multiplier for shadow ray sample count. Higher values increase shadow reach at the cost of performance. Adapts to render resolution."));

		ImGui::Checkbox(T(TKEY("adaptive_reach"), "Adaptive Reach"), (bool*)&bendSettings.AdaptiveReach);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("adaptive_reach_tooltip"), "Scales the ray length with the sun's angle to the view direction so shadows keep a similar reach as the camera turns: longer when the sun is to the side and shadows stretch across the screen, shorter when the sun is ahead or behind. Ray length is a per-pixel sweep, so the cost barely changes."));
		ImGui::Text(T(TKEY("ray_length"), "Ray length: %u samples (reach x%.2f)"), lastCompiledSampleCount, lastReachScale);
		if (lastRequestedSampleCount != 0 && lastCompiledSampleCount != lastRequestedSampleCount && IsRaymarchPending(lastRequestedSampleCount))
			ImGui::Text(T(TKEY("ray_length_compiling"), "Compiling %u samples in the background"), lastRequestedSampleCount);

		ImGui::SliderFloat(T(TKEY("surface_thickness"), "Surface Thickness"), &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("surface_thickness_tooltip"), "Assumed thickness of surfaces for shadow detection. Lower values produce thinner, more precise shadows."));

		ImGui::SliderFloat(T(TKEY("bilinear_threshold"), "Bilinear Threshold"), &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bilinear_threshold_tooltip"), "Depth threshold for edge detection during bilinear interpolation. Higher values smooth more aggressively across edges."));

		ImGui::SliderFloat(T(TKEY("shadow_contrast"), "Shadow Contrast"), &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shadow_contrast_tooltip"), "Contrast boost for the shadow transition. Higher values produce harder shadow edges."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::InvalidateRaymarchShaders()
{
	std::lock_guard lock(raymarchMutex);
	++raymarchGeneration;
	raymarchVariants.clear();
	lastCompiledSampleCount = 0;
	lastRequestedSampleCount = 0;
}

void ScreenSpaceShadows::ClearShaderCache()
{
	InvalidateRaymarchShaders();
}

uint ScreenSpaceShadows::GetScaledSampleCount(float a_reachScale)
{
	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(std::clamp(bendSettings.SampleCount, 1u, 4u) * 60 * areaScale * a_reachScale));

	// Quantize to steps of 8 to prevent frequent recompilation from small DRS oscillations
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	scaledSampleCount = std::clamp(scaledSampleCount, 8u, 1024u);

	return scaledSampleCount;
}

float ScreenSpaceShadows::GetAdaptiveReachScale(const float3& a_lightDirection)
{
	if (!bendSettings.AdaptiveReach) {
		reachBand = 0;
		return 1.0f;
	}

	// Screen-space shadow length scales with the light direction's component across the view axis.
	// The reference of 0.5 keeps the unmodified ray length at a 30 degree sun-to-view angle.
	auto viewMat = globals::game::frameBufferCached.GetCameraView().Transpose();
	auto lightVS = DirectX::SimpleMath::Vector4::Transform(float4(a_lightDirection.x, a_lightDirection.y, a_lightDirection.z, 0.0f), viewMat);
	const float length = std::sqrt(lightVS.x * lightVS.x + lightVS.y * lightVS.y + lightVS.z * lightVS.z);
	if (length <= 1e-6f)
		return 1.0f;
	const float across = std::sqrt(lightVS.x * lightVS.x + lightVS.y * lightVS.y) / length;
	constexpr float kReferenceAcross = 0.5f;
	return std::clamp(across / kReferenceAcross, 0.5f, 2.0f);
}

uint ScreenSpaceShadows::SelectSampleCount(float a_reachScale)
{
	// Bands are powers of sqrt(2) between 0.5x and 2x: five variants per base count. A band only changes once
	// the continuous value has moved 15 percent past the boundary, so a sun drifting along a boundary holds still.
	constexpr int kMinBand = -2;
	constexpr int kMaxBand = 2;
	constexpr float kHysteresis = 0.15f;
	const float continuous = std::clamp(std::log2(std::max(a_reachScale, 0.5f)) * 2.0f, (float)kMinBand, (float)kMaxBand);
	if (lastRequestedSampleCount == 0)
		reachBand = std::clamp((int)std::lround(continuous), kMinBand, kMaxBand);
	else if (continuous > (float)reachBand + 0.5f + kHysteresis)
		reachBand = std::min(reachBand + 1, kMaxBand);
	else if (continuous < (float)reachBand - 0.5f - kHysteresis)
		reachBand = std::max(reachBand - 1, kMinBand);
	lastReachScale = std::exp2((float)reachBand * 0.5f);

	const uint target = GetScaledSampleCount(lastReachScale);
	// Two quantisation steps of hysteresis so DRS jitter does not flip variants every frame.
	if (lastRequestedSampleCount != 0) {
		const uint delta = target > lastRequestedSampleCount ? target - lastRequestedSampleCount : lastRequestedSampleCount - target;
		if (delta < 16)
			return lastRequestedSampleCount;
	}
	lastRequestedSampleCount = target;
	return target;
}

void ScreenSpaceShadows::RequestRaymarch(uint a_sampleCount)
{
	if (!globals::shaderCache)
		return;

	std::vector<std::pair<const char*, const char*>> defines;
	uint generation = 0;
	{
		std::lock_guard lock(raymarchMutex);
		auto [it, inserted] = raymarchVariants.try_emplace(a_sampleCount);
		if (!inserted)
			return;
		it->second.pending = true;
		generation = raymarchGeneration;

		// Define strings must outlive the compile, so sample count texts are kept for the whole session.
		const std::string& text = raymarchSampleCountTexts.try_emplace(a_sampleCount, std::format("{}", a_sampleCount)).first->second;
		defines.push_back({ "SAMPLE_COUNT", text.c_str() });
	}
	// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
	// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });

	globals::shaderCache->EnqueueStandaloneShaderCompile(
		L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", "main", std::move(defines),
		SIE::ShaderCache::StandaloneShaderClass::Compute,
		[this, a_sampleCount, generation](ID3D11DeviceChild* a_shader) {
			std::lock_guard lock(raymarchMutex);
			auto found = raymarchVariants.find(a_sampleCount);
			if (generation != raymarchGeneration || found == raymarchVariants.end() || !found->second.pending) {
				if (a_shader)
					a_shader->Release();
				return;
			}
			found->second.pending = false;
			found->second.failed = a_shader == nullptr;
			if (a_shader)
				found->second.shader.attach(static_cast<ID3D11ComputeShader*>(a_shader));
		});
}

bool ScreenSpaceShadows::IsRaymarchPending(uint a_sampleCount)
{
	std::lock_guard lock(raymarchMutex);
	auto it = raymarchVariants.find(a_sampleCount);
	return it != raymarchVariants.end() && it->second.pending;
}

void ScreenSpaceShadows::PrefetchRaymarchVariants()
{
	if (!bendSettings.AdaptiveReach) {
		RequestRaymarch(GetScaledSampleCount(1.0f));
		return;
	}
	for (int band = -2; band <= 2; ++band)
		RequestRaymarch(GetScaledSampleCount(std::exp2((float)band * 0.5f)));
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch(uint a_sampleCount, uint& a_usedSampleCount)
{
	RequestRaymarch(a_sampleCount);

	std::lock_guard lock(raymarchMutex);
	if (auto it = raymarchVariants.find(a_sampleCount); it != raymarchVariants.end() && it->second.shader) {
		a_usedSampleCount = a_sampleCount;
		return it->second.shader.get();
	}

	ID3D11ComputeShader* nearest = nullptr;
	uint nearestDistance = UINT32_MAX;
	for (auto& [sampleCount, variant] : raymarchVariants) {
		if (!variant.shader)
			continue;
		const uint distance = sampleCount > a_sampleCount ? sampleCount - a_sampleCount : a_sampleCount - sampleCount;
		if (distance < nearestDistance) {
			nearestDistance = distance;
			nearest = variant.shader.get();
			a_usedSampleCount = sampleCount;
		}
	}
	if (nearest)
		return nearest;

	// Nothing has finished yet: the background pool serves the main shader queue first, which can take minutes on
	// a fresh cache, so the very first variant is compiled here once. Later variants always arrive in the background.
	auto& variant = raymarchVariants[a_sampleCount];
	if (variant.failed)
		return nullptr;
	const std::string& text = raymarchSampleCountTexts.try_emplace(a_sampleCount, std::format("{}", a_sampleCount)).first->second;
	std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", text.c_str() } };
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });
	auto* shader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0"));
	variant.pending = false;
	variant.failed = shader == nullptr;
	if (!shader)
		return nullptr;
	variant.shader.attach(shader);
	a_usedSampleCount = a_sampleCount;
	return shader;
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScopedS(8);
	TracyD3D11Zone(globals::state->tracyCtx, "Screen Space Shadows");

	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection
	auto CalculateLightProjection = [&]() -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection();

	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	// Setup common render state.
	// SSS always uses 24/32-bit depth, never the R16_UNORM half-precision path.
	// With TerrainBlending loaded the SRV is R32_FLOAT (blendedDepthTexture);
	// without it, the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS).
	// The shader's DepthTexture declaration is conditional on TERRAIN_BLENDING:
	// `<float>` for the R32_FLOAT path, `<unorm float>` for the R24_UNORM path.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	// Shared dispatch logic
	auto Dispatch = [&](ID3D11ComputeShader* shader, const float* lightProj,
						float invTexSizeX, float invTexSizeY) {
		globals::profiler->BeginPass("ScreenSpaceShadows::RayMarch");

		if (globals::state->frameAnnotations) {
			globals::state->BeginPerfEvent("SSS - Ray March");
		}

		context->CSSetShader(shader, nullptr, 0);

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			auto dispatchData = dispatchList.Dispatch[i];

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch CB");

				RaymarchCB data{};
				data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
				data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
				data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
				data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

				data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
				data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

				const bool reverseZ = globals::features::reverseZ.IsActive();
				data.FarDepthValue = reverseZ ? 0.0f : 1.0f;
				data.NearDepthValue = reverseZ ? 1.0f : 0.0f;

				data.DynamicRes = dynamicRes;

				data.InvDepthTextureSize[0] = invTexSizeX;
				data.InvDepthTextureSize[1] = invTexSizeY;

				data.settings = bendSettings;

				raymarchCB->Update(data);
			}

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch Sweep");
				context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
			}
		}

		if (globals::state->frameAnnotations) {
			globals::state->EndPerfEvent();
		}

		globals::profiler->EndPass();
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	const uint requestedSampleCount = SelectSampleCount(GetAdaptiveReachScale(light));
	RequestRaymarch(requestedSampleCount);
	PrefetchRaymarchVariants();
	uint usedSampleCount = 0;
	if (auto* raymarchCS = GetComputeRaymarch(requestedSampleCount, usedSampleCount)) {
		lastCompiledSampleCount = usedSampleCount;
		Dispatch(raymarchCS, lightProjectionF.data(), InvTexSizeX, InvTexSizeY);
	}

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky)
		if (bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull) {
			DrawShadows();
		}

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>(), "SSS::RaymarchCB");

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		const float farDepth = globals::features::reverseZ.IsActive() ? 0.0f : 1.0f;
		samplerDesc.BorderColor[0] = farDepth;
		samplerDesc.BorderColor[1] = farDepth;
		samplerDesc.BorderColor[2] = farDepth;
		samplerDesc.BorderColor[3] = farDepth;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
		Util::SetResourceName(pointBorderSampler, "SSS::PointBorderSampler");
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		// Store normal front-facing visibility and the reversed depth-offset
		// visibility used by back-facing SSS/transmission receivers.
		texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = new Texture2D(texDesc, "SSS::ShadowTexture");
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);
	}
}
#undef I18N_KEY_PREFIX
