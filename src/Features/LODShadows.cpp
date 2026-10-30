#include "LODShadows.h"

#include "LODShadows/TerrainRE.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/PerfUtils.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LODShadows::Settings, Enabled, UpdateBudgetMs, DepthBias)

namespace
{
	constexpr uint32_t kIndicesPerTriangle = 3;
	constexpr float kMinimumUpdateBudgetMs = 0.01f;
	constexpr float kMaximumUpdateBudgetMs = 0.4f;
	constexpr uint32_t kMaximumCacheFrames = 600;
	constexpr float kCenterMargin = 0.125f;
	constexpr float kDirectionThreshold = 0.001f;
	constexpr uint32_t kMaskGroupSize = 8;
	constexpr float kMinimumDirectionLength = 1e-6f;
	constexpr float kCellSize = 4096.0f;
	constexpr float kMinimumDepthPadding = 1.0f;
	constexpr int kRasterDepthBias = 1;
	constexpr float kTerrainSlopeScaledDepthBias = 1.0f;
	constexpr uint32_t kCollectIntervalFrames = 4;
	constexpr float kDefaultTimeScale = 20.0f;
	constexpr uint64_t kHashSeed = 14695981039346656037ull;
	constexpr uint64_t kHashPrime = 1099511628211ull;

	bool IsMapView()
	{
		const auto* manager = LODShadowsRE::GetActiveTerrainManager();
		return globals::state->isMapMenuOpen || (manager && LODShadowsRE::IsMapMode(manager));
	}

	float ShadowClockRate()
	{
		const auto* calendar = globals::game::calendar;
		const float timeScale = calendar ? calendar->GetTimescale() : kDefaultTimeScale;
		return std::isfinite(timeScale) ? std::max(1.0f, timeScale / kDefaultTimeScale) : 1.0f;
	}

	uint32_t BatchTriangleBudget(uint32_t a_triangleBudget)
	{
		return uint32_t(std::min(double(a_triangleBudget) * ShadowClockRate(), double(std::numeric_limits<uint32_t>::max() / kIndicesPerTriangle)));
	}

	void HashBytes(uint64_t& a_hash, const void* a_data, size_t a_size)
	{
		for (size_t i = 0; i < a_size; ++i)
			a_hash = (a_hash ^ static_cast<const uint8_t*>(a_data)[i]) * kHashPrime;
	}

	DirectX::XMMATRIX WorldMatrix(const RE::NiTransform& a_transform)
	{
		const auto& r = a_transform.rotate.entry;
		const auto& t = a_transform.translate;
		const DirectX::XMFLOAT3X4 transform(r[0][0], r[0][1], r[0][2], t.x, r[1][0], r[1][1], r[1][2], t.y, r[2][0], r[2][1], r[2][2], t.z);
		return DirectX::XMMatrixScaling(a_transform.scale, a_transform.scale, a_transform.scale) * DirectX::XMLoadFloat3x4(&transform);
	}

	RE::NiPoint3 GetSunDirection()
	{
		const auto* scene = globals::game::smState->shadowSceneNode[0];
		const auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
		const auto* light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
		return light ? light->GetWorldDirection() : RE::NiPoint3{};
	}

	float GetNativeShadowDistance()
	{
		const auto* scene = globals::game::smState->shadowSceneNode[0];
		const auto* nativeLight = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
		return nativeLight ? nativeLight->GetShadowDirectionalLightRuntimeData().endSplitDistances[1] : 0.0f;
	}

	template <class F>
	void ForEachCasterBound(RE::BSGeometry* a_geometry, F&& a_callback)
	{
		if (a_geometry->GetType() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
			const auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry)->GetMultiStreamTrishapeRuntimeData();
			for (auto* group : instances.instanceGroups) {
				if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
					continue;
				RE::NiBound bound;
				group->GetNiBound(&bound);
				a_callback(bound);
			}
		} else {
			a_callback(a_geometry->worldBound);
		}
	}

	struct PassStateScope
	{
		explicit PassStateScope(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.data(), depth.put());
			context->RSGetViewports(&viewportCount, viewports.data());
			context->OMGetBlendState(blend.put(), blendFactor.data(), &sampleMask);
			context->OMGetDepthStencilState(depthStencil.put(), &stencilRef);
			context->VSGetShader(vertexShader.put(), nullptr, nullptr);
			context->PSGetShader(pixelShader.put(), nullptr, nullptr);
			context->GSGetShader(geometryShader.put(), nullptr, nullptr);
			context->HSGetShader(hullShader.put(), nullptr, nullptr);
			context->DSGetShader(domainShader.put(), nullptr, nullptr);
			context->RSGetState(rasterizer.put());
			context->PSGetSamplers(0, 1, sampler.put());
			context->PSGetShaderResources(0, 1, pixelResource.put());
			context->VSGetConstantBuffers(0, 1, vertexConstants.put());
			context->PSGetConstantBuffers(0, 1, pixelConstants.put());
			context->IAGetInputLayout(inputLayout.put());
			context->IAGetVertexBuffers(0, 2, vertexBuffers.data(), strides.data(), offsets.data());
			context->IAGetIndexBuffer(indexBuffer.put(), &indexFormat, &indexOffset);
			context->IAGetPrimitiveTopology(&topology);
		}

		~PassStateScope()
		{
			ID3D11ShaderResourceView* nullView = nullptr;
			context->PSSetShaderResources(0, 1, &nullView);
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.data(), depth.get());
			auto* resource = pixelResource.get();
			context->PSSetShaderResources(0, 1, &resource);
			context->RSSetViewports(viewportCount, viewports.data());
			context->OMSetBlendState(blend.get(), blendFactor.data(), sampleMask);
			context->OMSetDepthStencilState(depthStencil.get(), stencilRef);
			context->VSSetShader(vertexShader.get(), nullptr, 0);
			context->PSSetShader(pixelShader.get(), nullptr, 0);
			context->GSSetShader(geometryShader.get(), nullptr, 0);
			context->HSSetShader(hullShader.get(), nullptr, 0);
			context->DSSetShader(domainShader.get(), nullptr, 0);
			context->RSSetState(rasterizer.get());
			auto* samplerState = sampler.get();
			context->PSSetSamplers(0, 1, &samplerState);
			auto* buffer = vertexConstants.get();
			context->VSSetConstantBuffers(0, 1, &buffer);
			buffer = pixelConstants.get();
			context->PSSetConstantBuffers(0, 1, &buffer);
			context->IASetInputLayout(inputLayout.get());
			context->IASetVertexBuffers(0, 2, vertexBuffers.data(), strides.data(), offsets.data());
			context->IASetIndexBuffer(indexBuffer.get(), indexFormat, indexOffset);
			context->IASetPrimitiveTopology(topology);
			for (auto* target : targets)
				if (target)
					target->Release();
			for (auto* vertexBuffer : vertexBuffers)
				if (vertexBuffer)
					vertexBuffer->Release();
		}

		PassStateScope(const PassStateScope&) = delete;
		PassStateScope& operator=(const PassStateScope&) = delete;

		ID3D11DeviceContext* context;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		winrt::com_ptr<ID3D11DepthStencilView> depth;
		UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		winrt::com_ptr<ID3D11BlendState> blend;
		std::array<FLOAT, 4> blendFactor{};
		UINT sampleMask = 0;
		winrt::com_ptr<ID3D11DepthStencilState> depthStencil;
		UINT stencilRef = 0;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11GeometryShader> geometryShader;
		winrt::com_ptr<ID3D11HullShader> hullShader;
		winrt::com_ptr<ID3D11DomainShader> domainShader;
		winrt::com_ptr<ID3D11RasterizerState> rasterizer;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11ShaderResourceView> pixelResource;
		winrt::com_ptr<ID3D11Buffer> vertexConstants;
		winrt::com_ptr<ID3D11Buffer> pixelConstants;
		winrt::com_ptr<ID3D11InputLayout> inputLayout;
		std::array<ID3D11Buffer*, 2> vertexBuffers{};
		std::array<UINT, 2> strides{};
		std::array<UINT, 2> offsets{};
		winrt::com_ptr<ID3D11Buffer> indexBuffer;
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};
}

std::pair<std::string, std::vector<std::string>> LODShadows::GetFeatureSummary()
{
	return { T("feature.lod_shadows.description", "Casts cached sun and moon shadows from terrain, distant objects and trees, without an underside mesh or a heightmap."),
		{ T("feature.lod_shadows.key_feature_1", "Shadows from LOD terrain, objects and trees beyond the native shadow range"),
			T("feature.lod_shadows.key_feature_2", "Replaces heightmap terrain shadows once every map covers the terrain"),
			T("feature.lod_shadows.key_feature_3", "Spreads rendering over frames within a GPU time budget") } };
}

void LODShadows::LoadSettings(json& a_json)
{
	const bool wasSettingsEnabled = settings.Enabled;
	settings = a_json;
	if (!std::isfinite(settings.UpdateBudgetMs))
		settings.UpdateBudgetMs = Settings{}.UpdateBudgetMs;
	if (!std::isfinite(settings.DepthBias))
		settings.DepthBias = Settings{}.DepthBias;
	settings.UpdateBudgetMs = std::clamp(settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs);
	settings.DepthBias = std::clamp(settings.DepthBias, 0.0f, 64.0f);
	if (wasSettingsEnabled != settings.Enabled)
		resetRequested = true;
}

void LODShadows::SaveSettings(json& a_json) { a_json = settings; }

void LODShadows::RestoreDefaultSettings()
{
	settings = {};
	resetRequested = true;
}

void LODShadows::GameLoaded() { resetRequested = true; }

void LODShadows::DrawSettings()
{
	ImGui::Checkbox(T("feature.lod_shadows.enabled", "Enable LOD Shadows"), &settings.Enabled);
	ImGui::SliderFloat(T("feature.lod_shadows.update_budget", "GPU Update Budget (ms)"), &settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs, "%.3f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.lod_shadows.update_budget_tooltip", "GPU time spent per frame rebuilding the cached maps. Higher values refresh shadows sooner after the sun moves."));
	ImGui::SliderFloat(T("feature.lod_shadows.depth_bias", "Depth Bias"), &settings.DepthBias, 0.0f, 64.0f, "%.1f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.lod_shadows.depth_bias_tooltip", "Raise to remove shadow acne on distant terrain, lower to keep small casters attached."));
	ImGui::TextDisabled("%s %.3f ms", T("feature.lod_shadows.last_update", "Last update:"), lastUpdateMs);
	ImGui::TextDisabled("%s %s", T("feature.lod_shadows.terrain_replacement", "Replaces terrain shadows:"), ReplacesTerrainShadows() ? T("common.yes", "Yes") : T("common.no", "No"));
}

void LODShadows::CompileShaders()
{
	vertexShader = nullptr;
	treeVertexShader = nullptr;
	alphaShader = nullptr;
	for (auto& cache : layouts)
		cache.clear();
	vertexBytecode = Util::CompileShaderBlob(L"Data\\Shaders\\LODShadows\\Caster.hlsl", {}, "vs_5_0");
	treeBytecode = Util::CompileShaderBlob(L"Data\\Shaders\\LODShadows\\Caster.hlsl", { { "INSTANCED_TREE", "1" } }, "vs_5_0");
	alphaShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\LODShadows\\Caster.hlsl", {}, "ps_5_0")));
	if (!vertexBytecode || !treeBytecode || !alphaShader)
		return;
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), nullptr, vertexShader.put()));
	DX::ThrowIfFailed(device->CreateVertexShader(treeBytecode->GetBufferPointer(), treeBytecode->GetBufferSize(), nullptr, treeVertexShader.put()));
	Util::SetResourceName(vertexShader.get(), "LODShadows::Caster VS");
	Util::SetResourceName(treeVertexShader.get(), "LODShadows::TreeCaster VS");
	Util::SetResourceName(alphaShader.get(), "LODShadows::AlphaTest PS");
}

ID3D11ComputeShader* LODShadows::GetComputeShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines)
{
	if (!a_shader)
		a_shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_path, a_defines, "cs_5_0")));
	return a_shader.get();
}

void LODShadows::SetupResources()
{
	ready = false;
	try {
		auto device = globals::d3d::device;
		D3D11_TEXTURE2D_DESC textureDesc{};
		textureDesc.Width = textureDesc.Height = kResolution;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = kDepthSliceCount * 2;
		textureDesc.Format = DXGI_FORMAT_R16_TYPELESS;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		DX::ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, depthTexture.put()));
		Util::SetResourceName(depthTexture.get(), "LODShadows::DepthMaps");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = textureDesc.ArraySize;
		DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture.get(), &srvDesc, depthSRV.put()));
		Util::SetResourceName(depthSRV.get(), "LODShadows::DepthMaps SRV");
		D3D11_TEXTURE2D_DESC boundsDesc{};
		boundsDesc.Width = boundsDesc.Height = kRayBoundsResolution;
		boundsDesc.MipLevels = kRayBoundsMipCount;
		boundsDesc.ArraySize = kDepthSliceCount;
		boundsDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		boundsDesc.SampleDesc.Count = 1;
		boundsDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		DX::ThrowIfFailed(device->CreateTexture2D(&boundsDesc, nullptr, rayBoundsTexture.put()));
		Util::SetResourceName(rayBoundsTexture.get(), "LODShadows::RayBounds");
		D3D11_SHADER_RESOURCE_VIEW_DESC boundsView{};
		boundsView.Format = boundsDesc.Format;
		boundsView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		boundsView.Texture2DArray.MipLevels = kRayBoundsMipCount;
		boundsView.Texture2DArray.ArraySize = kDepthSliceCount;
		DX::ThrowIfFailed(device->CreateShaderResourceView(rayBoundsTexture.get(), &boundsView, rayBoundsSRV.put()));
		Util::SetResourceName(rayBoundsSRV.get(), "LODShadows::RayBounds SRV");
		for (uint32_t mip = 0; mip < kRayBoundsMipCount; ++mip) {
			boundsView.Texture2DArray.MostDetailedMip = mip;
			boundsView.Texture2DArray.MipLevels = 1;
			DX::ThrowIfFailed(device->CreateShaderResourceView(rayBoundsTexture.get(), &boundsView, rayBoundsMipSRVs[mip].put()));
			Util::SetResourceName(rayBoundsMipSRVs[mip].get(), "LODShadows::RayBounds Mip %u SRV", mip);
			for (uint32_t slice = 0; slice < kDepthSliceCount; ++slice) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = boundsDesc.Format;
				uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				uav.Texture2DArray.MipSlice = mip;
				uav.Texture2DArray.FirstArraySlice = slice;
				uav.Texture2DArray.ArraySize = 1;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(rayBoundsTexture.get(), &uav, rayBoundsUAVs[slice][mip].put()));
				Util::SetResourceName(rayBoundsUAVs[slice][mip].get(), "LODShadows::RayBounds Slice %u Mip %u UAV", slice, mip);
			}
		}
		for (uint32_t i = 0; i < depthViews.size(); ++i) {
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_D16_UNORM;
			desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.FirstArraySlice = i;
			desc.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(device->CreateDepthStencilView(depthTexture.get(), &desc, depthViews[i].put()));
			Util::SetResourceName(depthViews[i].get(), "LODShadows::DepthMap %u DSV", i);
		}
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = true;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
		Util::SetResourceName(depthState.get(), "LODShadows::DepthState");
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = true;
		rasterDesc.DepthBias = kRasterDepthBias;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, rasterizer.put()));
		Util::SetResourceName(rasterizer.get(), "LODShadows::Rasterizer");
		rasterDesc.SlopeScaledDepthBias = kTerrainSlopeScaledDepthBias;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, terrainRasterizer.put()));
		Util::SetResourceName(terrainRasterizer.get(), "LODShadows::TerrainRasterizer");
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, alphaSampler.put()));
		Util::SetResourceName(alphaSampler.get(), "LODShadows::AlphaSampler");
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
		Util::SetResourceName(comparisonSampler.get(), "LODShadows::ComparisonSampler");
		drawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawData>(), "LODShadows::CasterData");
		mapBuffer = std::make_unique<Buffer>(StructuredBufferDesc<MapData>(uint64_t(kMapCount), false, false), nullptr, "LODShadows::MapData");
		D3D11_SHADER_RESOURCE_VIEW_DESC mapView{};
		mapView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		mapView.Buffer.NumElements = kMapCount;
		mapBuffer->CreateSRV(mapView);
		effectMapBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<std::array<MapData, kMapCount>>(), "LODShadows::EffectMapData");
		maskBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<std::array<uint32_t, 4>>(), "LODShadows::MaskData");
		for (auto& query : queries) {
			D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			DX::ThrowIfFailed(device->CreateQuery(&queryDesc, query.disjoint.put()));
			queryDesc.Query = D3D11_QUERY_TIMESTAMP;
			DX::ThrowIfFailed(device->CreateQuery(&queryDesc, query.begin.put()));
			DX::ThrowIfFailed(device->CreateQuery(&queryDesc, query.end.put()));
			Util::SetResourceName(query.disjoint.get(), "LODShadows::UpdateDisjoint");
			Util::SetResourceName(query.begin.get(), "LODShadows::UpdateBegin");
			Util::SetResourceName(query.end.get(), "LODShadows::UpdateEnd");
		}
		CompileShaders();
		ready = vertexShader && treeVertexShader && alphaShader && comparisonSampler;
		Invalidate();
		Publish();
	} catch (const std::exception& error) {
		logger::error("LOD Shadows resource creation failed: {}", error.what());
	}
}

void LODShadows::ClearShaderCache()
{
	ready = false;
	maskShader = nullptr;
	filterShader = nullptr;
	rayBoundsShader = nullptr;
	rayBoundsReduceShader = nullptr;
	maskFailed = false;
	Invalidate();
	try {
		CompileShaders();
		ready = depthTexture && vertexShader && treeVertexShader && alphaShader && comparisonSampler;
	} catch (const std::exception& error) {
		logger::error("LOD Shadows shader creation failed: {}", error.what());
	}
}

void LODShadows::Invalidate()
{
	for (auto& map : maps)
		map.valid = false;
	for (auto& map : previousMaps)
		map.valid = false;
	for (auto& map : completedDepthMaps)
		map.valid = false;
	casters.clear();
	visibleGeometry.clear();
	terrainGeometryCount = 0;
	terrainAvailable = false;
	pendingIndex = -1;
	nextMap = 0;
	clearPending = false;
	maskReady = false;
}

void LODShadows::CreateMaskResources()
{
	const auto* graphicsState = globals::game::graphicsState;
	const uint32_t width = (graphicsState->screenWidth + kMaskDownsample - 1) / kMaskDownsample;
	const uint32_t height = (graphicsState->screenHeight + kMaskDownsample - 1) / kMaskDownsample;
	if (maskTextures[0] && maskTextures[0]->desc.Width == width && maskTextures[0]->desc.Height == height)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = desc.Format;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	for (uint32_t i = 0; i < maskTextures.size(); ++i) {
		maskTextures[i] = std::make_unique<Texture2D>(desc, i == 0 ? "LODShadows::ReceiverMask" : "LODShadows::FilteredMask");
		maskTextures[i]->CreateSRV(srv);
		maskTextures[i]->CreateUAV(uav);
	}
}

void LODShadows::DrawMask()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "LODShadows::FilterReceivers");
	auto* depth = Util::GetCurrentSceneDepthSRV(false);
	auto* generate = GetComputeShader(maskShader, L"Data\\Shaders\\LODShadows\\FilterMask.cs.hlsl", {});
	auto* filter = GetComputeShader(filterShader, L"Data\\Shaders\\LODShadows\\FilterMask.cs.hlsl", { { "FILTER", "1" } });
	if (!depth || !generate || !filter)
		return;
	CreateMaskResources();
	const auto* graphicsState = globals::game::graphicsState;
	const auto renderSize = Util::ConvertToDynamic(float2{ float(graphicsState->screenWidth), float(graphicsState->screenHeight) });
	maskDimensions = { (uint32_t(renderSize.x) + kMaskDownsample - 1) / kMaskDownsample,
		(uint32_t(renderSize.y) + kMaskDownsample - 1) / kMaskDownsample };
	if (!maskDimensions[0] || !maskDimensions[1])
		return;
	maskBuffer->Update(std::array<uint32_t, 4>{ maskDimensions[0], maskDimensions[1], 0, 0 });
	auto* context = globals::d3d::context;
	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	winrt::com_ptr<ID3D11Buffer> savedBuffer;
	winrt::com_ptr<ID3D11SamplerState> savedSampler;
	winrt::com_ptr<ID3D11UnorderedAccessView> savedUAV;
	std::array<ID3D11ShaderResourceView*, 2> savedViews{};
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(0, 1, savedBuffer.put());
	context->CSGetSamplers(0, 1, savedSampler.put());
	context->CSGetUnorderedAccessViews(0, 1, savedUAV.put());
	context->CSGetShaderResources(0, uint32_t(savedViews.size()), savedViews.data());
	const SKSE::stl::scope_exit restore([&]() noexcept {
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(0, uint32_t(savedViews.size()), savedViews.data());
		for (auto* view : savedViews)
			if (view)
				view->Release();
		auto* buffer = savedBuffer.get();
		auto* sampler = savedSampler.get();
		auto* uav = savedUAV.get();
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(savedShader.get(), nullptr, 0);
	});
	ID3D11ShaderResourceView* nullView = nullptr;
	context->PSSetShaderResources(kMaskSlot, 1, &nullView);
	auto* buffer = maskBuffer->CB();
	auto* sampler = alphaSampler.get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetSamplers(0, 1, &sampler);
	const uint32_t groupsX = (maskDimensions[0] + kMaskGroupSize - 1) / kMaskGroupSize;
	const uint32_t groupsY = (maskDimensions[1] + kMaskGroupSize - 1) / kMaskGroupSize;
	std::array<ID3D11ShaderResourceView*, 2> views{ depth, nullptr };
	auto* uav = maskTextures[0]->uav.get();
	context->CSSetShaderResources(0, uint32_t(views.size()), views.data());
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(generate, nullptr, 0);
	context->Dispatch(groupsX, groupsY, 1);
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	views = { nullptr, maskTextures[0]->srv.get() };
	context->CSSetShaderResources(0, uint32_t(views.size()), views.data());
	uav = maskTextures[1]->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(filter, nullptr, 0);
	context->Dispatch(groupsX, groupsY, 1);
	maskReady = true;
}

void LODShadows::Prepass()
{
	maskReady = false;
	if (ready && settings.Enabled && !maskFailed && !IsMapView() && globals::state->HasDirectionalShadows()) {
		try {
			DrawMask();
		} catch (const std::exception& error) {
			maskFailed = true;
			logger::error("LOD Shadows receiver filtering failed: {}", error.what());
		}
	}
	Publish();
	auto* view = maskReady ? maskTextures[1]->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(kMaskSlot, 1, &view);
}

void LODShadows::Collect(RE::NiAVObject* a_object, bool a_ignoreAppCulling, bool a_loadedObject)
{
	if (!a_object)
		return;
	if (!a_ignoreAppCulling && a_object->GetAppCulled())
		return;
	if (auto node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			Collect(child.get(), a_ignoreAppCulling, a_loadedObject);
	} else if (auto geometry = a_object->AsGeometry()) {
		if (a_loadedObject) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				!property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite) ||
				property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction, RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction) ||
				!collectedGeometry.insert(geometry).second)
				return;
		}
		visibleGeometry.emplace_back(geometry);
		HashBytes(sceneHash, &geometry, sizeof(geometry));
		HashBytes(sceneHash, &geometry->world, sizeof(geometry->world));
		const auto lodMode = LODShadowsRE::GetLODMode(geometry);
		HashBytes(sceneHash, &lodMode, sizeof(lodMode));
		auto& data = geometry->GetGeometryRuntimeData();
		HashBytes(sceneHash, &data.rendererData, sizeof(data.rendererData));
		if (auto segmented = geometry->AsSubIndexTriShape()) {
			auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
			if (segments.segmentData) {
				for (uint32_t i = 0; i < segments.numSegments; ++i) {
					const auto& range = segments.segmentData[i];
					HashBytes(sceneHash, &range.index, sizeof(range.index));
					HashBytes(sceneHash, &range.numTris, sizeof(range.numTris));
					HashBytes(sceneHash, &range.flags, sizeof(range.flags));
				}
			}
		} else if (geometry->GetType() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
			auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(geometry)->GetMultiStreamTrishapeRuntimeData();
			HashBytes(sceneHash, &instances.instanceCount, sizeof(instances.instanceCount));
			for (auto group : instances.instanceGroups) {
				if (group && group->vertexBuffer) {
					HashBytes(sceneHash, &group->vertexBuffer->buffer, sizeof(group->vertexBuffer->buffer));
					HashBytes(sceneHash, &group->instanceCount, sizeof(group->instanceCount));
					HashBytes(sceneHash, &group->center, sizeof(group->center));
					HashBytes(sceneHash, &group->size, sizeof(group->size));
				}
			}
		}
	}
}

void LODShadows::CollectTerrain()
{
	Collect(LODShadowsRE::GetLODLandRoot().get(), true);
	terrainAvailable = !visibleGeometry.empty();
	landBounds = float4{};
	const auto* tes = globals::game::tes;
	const auto* grid = tes ? tes->gridCells : nullptr;
	if (grid) {
		landBounds = float4{ (tes->currentGridX + 0.5f) * kCellSize, (tes->currentGridY + 0.5f) * kCellSize,
			grid->length * kCellSize * 0.5f, grid->length * kCellSize * 0.5f };
		for (uint32_t x = 0; x < grid->length; ++x) {
			for (uint32_t y = 0; y < grid->length; ++y) {
				auto* cell = grid->GetCell(x, y);
				if (!cell || !cell->IsAttached() || !cell->IsExteriorCell())
					continue;
				const auto* land = cell->GetRuntimeData().cellLand;
				if (land && land->loadedData)
					for (auto* mesh : land->loadedData->mesh)
						Collect(mesh);
			}
		}
	}
	terrainGeometryCount = uint32_t(visibleGeometry.size());
	HashBytes(sceneHash, &landBounds, sizeof(landBounds));
}

void LODShadows::CollectLoadedObjects()
{
	collectedGeometry.clear();
	for (const auto& geometry : visibleGeometry)
		collectedGeometry.insert(geometry.get());
	if (auto* tes = globals::game::tes) {
		tes->ForEachReference([&](RE::TESObjectREFR* reference) {
			const auto* base = reference ? reference->GetBaseObject() : nullptr;
			const auto* staticObject = base ? base->As<RE::TESObjectSTAT>() : nullptr;
			if (base && base->Is(RE::FormType::Static, RE::FormType::StaticCollection, RE::FormType::Tree) &&
				(!staticObject || !staticObject->IsSkyObject()) &&
				!reference->IsDisabled() && !reference->IsDeleted())
				Collect(reference->Get3D(), false, true);
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
}

void LODShadows::AddCaster(RE::BSGeometry* a_geometry)
{
	auto shape = a_geometry->AsTriShape();
	auto& data = a_geometry->GetGeometryRuntimeData();
	auto mesh = data.rendererData;
	auto property = data.shaderProperty.get();
	if (!shape || !mesh || !mesh->vertexBuffer || !mesh->indexBuffer || data.skinInstance || !property) {
		++unsupportedCasters;
		return;
	}
	const auto type = a_geometry->GetType().get();
	const bool instanced = type == RE::BSGeometry::Type::kMultiStreamInstanceTriShape;
	if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kSubIndexTriShape &&
		type != RE::BSGeometry::Type::kMeshLODTriShape && type != RE::BSGeometry::Type::kMultiIndexTriShape && !instanced) {
		++unsupportedCasters;
		return;
	}
	Caster caster;
	caster.lodLandscape = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape);
	caster.geometry.reset(a_geometry);
	caster.vertices.copy_from(reinterpret_cast<ID3D11Buffer*>(mesh->vertexBuffer));
	caster.indices.copy_from(reinterpret_cast<ID3D11Buffer*>(mesh->indexBuffer));
	caster.vertexDescriptor = std::bit_cast<uint64_t>(data.vertexDesc);
	constexpr uint64_t kStrideMask = 0xf;
	caster.stride = uint32_t(caster.vertexDescriptor & kStrideMask) * sizeof(float);
	caster.indexCount = shape->GetTrishapeRuntimeData().triangleCount * kIndicesPerTriangle;
	if (type == RE::BSGeometry::Type::kMeshLODTriShape) {
		constexpr uint8_t kSingleLevel = 0x80;
		const auto mode = LODShadowsRE::GetLODMode(a_geometry);
		const uint32_t level = mode & ~kSingleLevel;
		const auto* counts = LODShadowsRE::GetMeshLODTriangleCounts(a_geometry);
		if (level > LODShadowsRE::kMeshLODLevels || ((mode & kSingleLevel) && level == LODShadowsRE::kMeshLODLevels)) {
			++unsupportedCasters;
			return;
		}
		uint32_t prefix = 0;
		for (uint32_t i = 0; i < level; ++i)
			prefix += counts[i] * kIndicesPerTriangle;
		caster.firstIndex = mode & kSingleLevel ? prefix : 0;
		caster.indexCount = mode & kSingleLevel ? counts[level] * kIndicesPerTriangle : prefix;
	}
	caster.transform = a_geometry->world;
	caster.bound = a_geometry->worldBound;
	if (caster.stride < (data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? sizeof(float) * 3 : sizeof(uint16_t) * 4)) {
		++unsupportedCasters;
		return;
	}
	if (!caster.indexCount)
		return;
	if (auto material = property->material) {
		caster.uvTransform = float4{ material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
		if (material->GetType() == RE::BSShaderMaterial::Type::kLighting)
			caster.materialAlpha = static_cast<RE::BSLightingShaderMaterialBase*>(material)->materialAlpha;
	}
	if (auto alpha = data.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		caster.alphaThreshold = alpha->alphaThreshold / 255.0f;
	if (instanced)
		caster.alphaThreshold = std::max(caster.alphaThreshold, 0.5f);
	if (caster.alphaThreshold > 0) {
		auto texture = instanced ? skyrim_cast<RE::NiSourceTexture*>(LODShadowsRE::GetDistantTreeAtlas().get()) : property->GetBaseTexture();
		if (!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !texture || !texture->rendererTexture || !texture->rendererTexture->resourceView) {
			++unsupportedCasters;
			return;
		}
		caster.texture.copy_from(reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView));
	}
	D3D11_BUFFER_DESC indexDesc{};
	caster.indices->GetDesc(&indexDesc);
	if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
		++unsupportedCasters;
		return;
	}
	if (instanced) {
		auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry)->GetMultiStreamTrishapeRuntimeData();
		caster.instanceStride = uint32_t((caster.vertexDescriptor >> 4) & kStrideMask) * sizeof(float);
		if (caster.instanceStride < sizeof(uint16_t) * 8) {
			++unsupportedCasters;
			return;
		}
		for (auto group : instances.instanceGroups) {
			if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
				continue;
			caster.instances.copy_from(reinterpret_cast<ID3D11Buffer*>(group->vertexBuffer->buffer));
			caster.instanceCount = uint32_t(std::min(size_t(group->instanceCount), group->vertexBuffer->byteWidth / caster.instanceStride));
			group->GetNiBound(&caster.bound);
			if (caster.instanceCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (auto segmented = a_geometry->AsSubIndexTriShape()) {
		auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
		if (!segments.segmentData) {
			++unsupportedCasters;
			return;
		}
		for (uint32_t i = 0; i < (segments.nonSegmented ? 1u : segments.numSegments); ++i) {
			const auto& range = segments.segmentData[i];
			if (!range.flags)
				continue;
			caster.firstIndex = segments.nonSegmented ? 0 : range.index;
			caster.indexCount = (segments.nonSegmented ? shape->GetTrishapeRuntimeData().triangleCount : range.numTris) * kIndicesPerTriangle;
			if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
				++unsupportedCasters;
				continue;
			}
			if (caster.indexCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (Intersects(caster)) {
		casters.push_back(std::move(caster));
	}
}

bool LODShadows::Intersects(const Caster& a_caster) const
{
	using namespace DirectX;
	const auto& center = a_caster.bound.center;
	const auto point = XMVector3TransformCoord(XMVectorSet(center.x, center.y, center.z, 1), XMLoadFloat4x4(&pendingMap.data.worldToClip));
	const float radiusXY = a_caster.bound.radius / pendingMap.halfExtent;
	const float radiusZ = a_caster.bound.radius / pendingMap.depthRange;
	return std::abs(XMVectorGetX(point)) <= 1 + radiusXY && std::abs(XMVectorGetY(point)) <= 1 + radiusXY && XMVectorGetZ(point) >= -radiusZ && XMVectorGetZ(point) <= 1 + radiusZ;
}

void LODShadows::BeginMap(uint32_t a_index, const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction)
{
	using namespace DirectX;
	pendingIndex = a_index;
	pendingMap = {};
	pendingMap.center = a_center;
	pendingMap.direction = a_direction;
	for (uint32_t slice = a_index * kDepthSlicesPerMap; slice < (a_index + 1) * kDepthSlicesPerMap; ++slice) {
		if ((!maps[a_index].valid || slice != maps[a_index].slice) &&
			(!previousMaps[a_index].valid || slice != previousMaps[a_index].slice)) {
			pendingMap.slice = slice;
			break;
		}
	}
	pendingMap.scene = sceneHash;
	pendingMap.frame = globals::state->frameCount;
	pendingMap.transitionGeneration = transitionGeneration;
	pendingMap.terrainValid = terrainAvailable;
	pendingLandBounds = landBounds;
	const auto direction = XMVectorSet(a_direction.x, a_direction.y, a_direction.z, 0);
	const auto up = std::abs(a_direction.z) > 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
	const auto rotation = XMMatrixLookToLH(XMVectorZero(), direction, up);
	const auto center = XMVector3TransformCoord(XMVectorSet(a_center.x, a_center.y, a_center.z, 1), rotation);
	float extent = kMinimumHalfExtents[a_index];
	if (a_index == kMapCount - 1) {
		for (const auto& geometry : visibleGeometry) {
			ForEachCasterBound(geometry.get(), [&](const RE::NiBound& bound) {
				const auto offset = bound.center - a_center;
				const auto point = XMVector3TransformNormal(XMVectorSet(offset.x, offset.y, offset.z, 0), rotation);
				const float requiredExtent = std::max(std::abs(XMVectorGetX(point)), std::abs(XMVectorGetY(point))) + bound.radius;
				if (std::isfinite(requiredExtent))
					extent = std::max(extent, std::ceil(requiredExtent / ((1.0f - kCenterMargin) * kCellSize)) * kCellSize);
			});
		}
	}
	const float texel = extent * 2 / kResolution;
	const float centerX = std::round(XMVectorGetX(center) / texel) * texel;
	const float centerY = std::round(XMVectorGetY(center) / texel) * texel;
	float depthNear = -kCellSize;
	float depthFar = kCellSize;
	for (const auto& geometry : visibleGeometry) {
		ForEachCasterBound(geometry.get(), [&](const RE::NiBound& bound) {
			const auto offset = bound.center - a_center;
			const auto point = XMVector3TransformNormal(XMVectorSet(offset.x, offset.y, offset.z, 0), rotation);
			const float x = XMVectorGetX(point) + XMVectorGetX(center) - centerX;
			const float y = XMVectorGetY(point) + XMVectorGetY(center) - centerY;
			const float z = XMVectorGetZ(point);
			if (std::abs(x) <= extent + bound.radius && std::abs(y) <= extent + bound.radius &&
				std::isfinite(z) && std::isfinite(bound.radius)) {
				depthNear = std::min(depthNear, z - bound.radius);
				depthFar = std::max(depthFar, z + bound.radius);
			}
		});
	}
	const float padding = std::max(kMinimumDepthPadding, settings.DepthBias);
	depthNear -= padding;
	depthFar += padding;
	pendingMap.halfExtent = extent;
	pendingMap.depthRange = depthFar - depthNear;
	const auto translation = XMMatrixTranslation(-centerX, -centerY, -XMVectorGetZ(center) - depthNear);
	XMStoreFloat4x4(&pendingMap.data.worldToClip, rotation * translation * XMMatrixOrthographicLH(extent * 2, extent * 2, 0, pendingMap.depthRange));
	casters.clear();
	unsupportedCasters = 0;
	const float nativeDistance = GetNativeShadowDistance();
	const auto loadedObjectStart = visibleGeometry.size() - geometryCounts[3];
	for (uint32_t i = 0; i < visibleGeometry.size(); ++i) {
		const auto unsupported = unsupportedCasters;
		const auto firstCaster = casters.size();
		AddCaster(visibleGeometry[i].get());
		for (size_t caster = firstCaster; caster < casters.size(); ++caster) {
			const auto& bound = casters[caster].bound;
			casters[caster].nativeShadowCaster = i >= loadedObjectStart && nativeDistance > 0.0f &&
			                                     (bound.center - a_center).Length() - bound.radius < nativeDistance;
		}
		if (i < terrainGeometryCount && unsupportedCasters != unsupported)
			pendingMap.terrainValid = false;
	}
	std::ranges::sort(casters, {}, [](const Caster& caster) { return std::tuple{ caster.nativeShadowCaster, caster.alphaThreshold > 0 }; });
	casterIndex = 0;
	indexOffset = instanceOffset = 0;
	clearPending = true;
	distantDepthCaptured = false;
}

ID3D11InputLayout* LODShadows::GetLayout(const Caster& a_caster)
{
	const bool instanced = bool(a_caster.instances);
	auto& cache = layouts[instanced];
	if (auto it = cache.find(a_caster.vertexDescriptor); it != cache.end())
		return it->second.get();
	const auto desc = std::bit_cast<RE::BSGraphics::VertexDesc>(a_caster.vertexDescriptor);
	std::array<D3D11_INPUT_ELEMENT_DESC, 4> elements{
		D3D11_INPUT_ELEMENT_DESC{ "POSITION", 0, desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) : 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 5, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 }
	};
	auto blob = instanced ? treeBytecode.get() : vertexBytecode.get();
	winrt::com_ptr<ID3D11InputLayout> layout;
	DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(elements.data(), instanced ? 4 : 2, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put()));
	Util::SetResourceName(layout.get(), "LODShadows::CasterLayout");
	return cache.emplace(a_caster.vertexDescriptor, std::move(layout)).first->second.get();
}

void LODShadows::ResolveTimings()
{
	auto* context = globals::d3d::context;
	for (auto& query : queries) {
		if (!query.pending)
			continue;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		if (context->GetData(query.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;
		query.pending = false;
		uint64_t begin = 0;
		uint64_t end = 0;
		if (disjoint.Disjoint || !disjoint.Frequency ||
			context->GetData(query.begin.get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			context->GetData(query.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || end < begin)
			continue;
		const double elapsedMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
		if (query.immediateRefresh)
			continue;
		lastUpdateMs = elapsedMs;
		if (query.budgetLimited || lastUpdateMs > settings.UpdateBudgetMs)
			updateBudget.Record(query.triangles, lastUpdateMs, settings.UpdateBudgetMs);
	}
}

void LODShadows::UpdateMaps(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, bool a_refreshImmediately)
{
	if (a_refreshImmediately) {
		for (auto& map : completedDepthMaps)
			map.valid = false;
	} else {
		PublishCompletedMaps(false);
	}
	auto& query = queries[queryCursor];
	if (query.pending && !a_refreshImmediately)
		return;
	if (!a_refreshImmediately && pendingIndex < 0) {
		RE::BSSpinLockGuard lock(LODShadowsRE::GetCameraTerrainLock());
		std::array<uint32_t, kMapCount> candidates{};
		uint32_t candidateCount = 0;
		std::array<double, kMapCount> deadlines{};
		for (uint32_t i = 0; i < kMapCount; ++i) {
			const auto index = (nextMap + i) % kMapCount;
			if (completedDepthMaps[index].valid)
				continue;
			const auto& map = maps[index];
			const bool blending = previousMaps[index].valid;
			if (!NeedsUpdate(index, a_center, a_direction) && (!blending || map.direction == a_direction))
				continue;
			deadlines[index] = map.valid ? map.completedTimeMs + (blending ? map.blendDurationMs : 0.0) : 0.0;
			candidates[candidateCount++] = index;
		}
		std::sort(candidates.begin(), candidates.begin() + candidateCount, [&](uint32_t left, uint32_t right) {
			return deadlines[left] < deadlines[right];
		});
		double buildTimeMs = 0;
		const auto batchTriangles = BatchTriangleBudget(updateBudget.Triangles());
		for (uint32_t i = 0; i < candidateCount; ++i) {
			const auto index = candidates[i];
			const auto& map = maps[index];
			const double workloadTimeMs = std::ceil(double(map.triangleCount) / batchTriangles) * frameDurationMs;
			buildTimeMs += std::max(map.buildDurationMs, workloadTimeMs) + frameDurationMs;
			if (frameTimeMs + buildTimeMs >= deadlines[index]) {
				BeginMap(candidates[0], a_center, a_direction);
				break;
			}
		}
		if (pendingIndex < 0)
			return;
	}
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "LODShadows::Update");
	auto context = globals::d3d::context;
	const bool measured = !query.pending;
	if (measured) {
		context->Begin(query.disjoint.get());
		context->End(query.begin.get());
	}
	const SKSE::stl::scope_exit finishTiming([&]() noexcept {
		if (!measured)
			return;
		context->End(query.end.get());
		context->End(query.disjoint.get());
		query.pending = true;
		query.triangles = lastTriangles;
		query.budgetLimited = pendingIndex >= 0;
		query.immediateRefresh = a_refreshImmediately;
		queryCursor = (queryCursor + 1) % kQueryFrames;
	});
	if (a_refreshImmediately) {
		for (uint32_t i = 0; i < kMapCount; ++i) {
			{
				RE::BSSpinLockGuard lock(LODShadowsRE::GetCameraTerrainLock());
				BeginMap(i, a_center, a_direction);
			}
			RenderBatch(true);
		}
	} else {
		RenderBatch(false);
	}
}

void LODShadows::CaptureDistantDepth()
{
	if (distantDepthCaptured || (casterIndex < casters.size() && !casters[casterIndex].nativeShadowCaster))
		return;
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CopySubresourceRegion(depthTexture.get(), kDepthSliceCount + pendingMap.slice, 0, 0, 0,
		depthTexture.get(), pendingMap.slice, nullptr);
	auto* depth = depthViews[pendingMap.slice].get();
	context->OMSetRenderTargets(0, nullptr, depth);
	distantDepthCaptured = true;
}

bool LODShadows::BuildRayBounds(uint32_t a_slice)
{
	TracyD3D11Zone(globals::state->tracyCtx, "LODShadows::BuildRayBounds");
	auto* generate = GetComputeShader(rayBoundsShader, L"Data\\Shaders\\LODShadows\\RayBounds.cs.hlsl", {});
	auto* reduce = GetComputeShader(rayBoundsReduceShader, L"Data\\Shaders\\LODShadows\\RayBounds.cs.hlsl", { { "REDUCE", "1" } });
	if (!generate || !reduce)
		return false;
	auto* context = globals::d3d::context;
	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	winrt::com_ptr<ID3D11Buffer> savedBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> savedUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> savedInput;
	winrt::com_ptr<ID3D11ShaderResourceView> savedBounds;
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(0, 1, savedBuffer.put());
	context->CSGetUnorderedAccessViews(0, 1, savedUAV.put());
	context->CSGetShaderResources(0, 1, savedInput.put());
	context->PSGetShaderResources(kRayBoundsSlot, 1, savedBounds.put());
	const SKSE::stl::scope_exit restore([&]() noexcept {
		ID3D11UnorderedAccessView* uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		auto* view = savedInput.get();
		context->CSSetShaderResources(0, 1, &view);
		view = savedBounds.get();
		context->PSSetShaderResources(kRayBoundsSlot, 1, &view);
		auto* buffer = savedBuffer.get();
		context->CSSetConstantBuffers(0, 1, &buffer);
		uav = savedUAV.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(savedShader.get(), nullptr, 0);
	});
	ID3D11ShaderResourceView* nullView = nullptr;
	context->PSSetShaderResources(kRayBoundsSlot, 1, &nullView);
	auto* buffer = maskBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);
	for (uint32_t mip = 0; mip < kRayBoundsMipCount; ++mip) {
		const uint32_t dimension = kRayBoundsResolution >> mip;
		maskBuffer->Update(std::array<uint32_t, 4>{ dimension, a_slice, kRayBoundsTileSize, 0 });
		auto* input = mip ? rayBoundsMipSRVs[mip - 1].get() : depthSRV.get();
		context->CSSetShaderResources(0, 1, &input);
		auto* output = rayBoundsUAVs[a_slice][mip].get();
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShader(mip ? reduce : generate, nullptr, 0);
		const uint32_t groups = mip ? (dimension + kMaskGroupSize - 1) / kMaskGroupSize : dimension;
		context->Dispatch(groups, groups, 1);
		output = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShaderResources(0, 1, &nullView);
	}
	return true;
}

void LODShadows::RenderBatch(bool a_refreshImmediately)
{
	auto context = globals::d3d::context;
	const auto firstTriangle = lastTriangles;
	if (!a_refreshImmediately)
		pendingMap.buildDurationMs += frameDurationMs;
	const auto batchTriangles = BatchTriangleBudget(updateBudget.Triangles());
	{
		PassStateScope scope(context);
		ID3D11ShaderResourceView* nullView = nullptr;
		context->PSSetShaderResources(kDepthSlot, 1, &nullView);
		context->CSSetShaderResources(kDepthSlot, 1, &nullView);
		auto depth = depthViews[pendingMap.slice].get();
		context->OMSetRenderTargets(0, nullptr, depth);
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->OMSetBlendState(nullptr, nullptr, ~0u);
		ID3D11RasterizerState* activeRasterizer = nullptr;
		const D3D11_VIEWPORT viewport{ 0, 0, float(kResolution), float(kResolution), 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		auto buffer = drawBuffer->CB();
		context->VSSetConstantBuffers(0, 1, &buffer);
		context->PSSetConstantBuffers(0, 1, &buffer);
		auto sampler = alphaSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		if (clearPending) {
			context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1, 0);
			clearPending = false;
		}
		while (casterIndex < casters.size() && (a_refreshImmediately || lastTriangles < batchTriangles)) {
			CaptureDistantDepth();
			const auto& caster = casters[casterIndex];
			auto* casterRasterizer = caster.lodLandscape ? terrainRasterizer.get() : rasterizer.get();
			if (activeRasterizer != casterRasterizer) {
				context->RSSetState(casterRasterizer);
				activeRasterizer = casterRasterizer;
			}
			DrawData data;
			DirectX::XMStoreFloat4x4(&data.worldToClip, WorldMatrix(caster.transform) * DirectX::XMLoadFloat4x4(&pendingMap.data.worldToClip));
			DirectX::XMStoreFloat4x4(&data.world, DirectX::XMMatrixTranspose(WorldMatrix(caster.transform)));
			data.uvTransform = caster.uvTransform;
			data.alpha = float4{ caster.alphaThreshold, caster.materialAlpha, caster.lodLandscape ? 1.0f : 0.0f, 0 };
			data.landBounds = pendingLandBounds;
			drawBuffer->Update(data);
			context->VSSetShader(caster.instances ? treeVertexShader.get() : vertexShader.get(), nullptr, 0);
			context->PSSetShader(caster.alphaThreshold > 0 ? alphaShader.get() : nullptr, nullptr, 0);
			auto texture = caster.texture.get();
			context->PSSetShaderResources(0, 1, &texture);
			context->IASetInputLayout(GetLayout(caster));
			ID3D11Buffer* vertices[]{ caster.vertices.get(), caster.instances.get() };
			const UINT strides[]{ caster.stride, caster.instanceStride };
			const UINT offsets[]{ 0, 0 };
			context->IASetVertexBuffers(0, caster.instances ? 2 : 1, vertices, strides, offsets);
			context->IASetIndexBuffer(caster.indices.get(), DXGI_FORMAT_R16_UINT, 0);
			const uint32_t count = a_refreshImmediately ? caster.indexCount - indexOffset :
			                                              std::min(caster.indexCount - indexOffset, (batchTriangles - lastTriangles) * kIndicesPerTriangle);
			const uint32_t instances = caster.instances && indexOffset == 0 && count == caster.indexCount ?
			                               (a_refreshImmediately ? caster.instanceCount - instanceOffset :
			                                                       std::min(caster.instanceCount - instanceOffset, (batchTriangles - lastTriangles) / (count / kIndicesPerTriangle))) :
			                               1;
			if (caster.instances)
				context->DrawIndexedInstanced(count, instances, caster.firstIndex + indexOffset, 0, instanceOffset);
			else
				context->DrawIndexed(count, caster.firstIndex + indexOffset, 0);
			lastTriangles += count / kIndicesPerTriangle * instances;
			indexOffset += count;
			if (indexOffset == caster.indexCount) {
				indexOffset = 0;
				instanceOffset += instances;
				if (instanceOffset == caster.instanceCount) {
					instanceOffset = 0;
					++casterIndex;
				}
			}
		}
		CaptureDistantDepth();
	}
	pendingMap.triangleCount += lastTriangles - firstTriangle;
	if (casterIndex == casters.size()) {
		pendingMap.rayBoundsValid = BuildRayBounds(pendingMap.slice);
		pendingMap.valid = true;
		completedDepthMaps[pendingIndex] = pendingMap;
		nextMap = (pendingIndex + 1) % kMapCount;
		pendingIndex = -1;
		casters.clear();
		PublishCompletedMaps(a_refreshImmediately);
	}
}

void LODShadows::PublishCompletedMaps(bool a_refreshImmediately)
{
	for (uint32_t i = 0; i < kMapCount; ++i) {
		auto& completed = completedDepthMaps[i];
		if (!completed.valid || (!a_refreshImmediately && GetHistoryBlend(i) < 1.0f))
			continue;
		previousMaps[i] = maps[i];
		previousMaps[i].valid &= !a_refreshImmediately;
		completed.completedTimeMs = frameTimeMs;
		completed.blendDurationMs = kHistoryIntervalsMs[i] / ShadowClockRate();
		maps[i] = completed;
		completed.valid = false;
	}
}

float LODShadows::GetHistoryBlend(uint32_t a_index) const
{
	const auto& map = maps[a_index];
	if (!previousMaps[a_index].valid || map.blendDurationMs <= 0)
		return 1.0f;
	return float(std::clamp((frameTimeMs - map.completedTimeMs) / map.blendDurationMs, 0.0, 1.0));
}

bool LODShadows::NeedsUpdate(uint32_t a_index, const RE::NiPoint3& a_position, const RE::NiPoint3& a_direction) const
{
	const auto& map = maps[a_index];
	return !map.valid || map.scene != sceneHash || map.transitionGeneration != transitionGeneration ||
	       (map.center - a_position).Length() > map.halfExtent * kCenterMargin ||
	       (map.direction - a_direction).Length() > kDirectionThreshold || globals::state->frameCount - map.frame > kMaximumCacheFrames;
}

bool LODShadows::ReplacesTerrainShadows() const
{
	if (!loaded || !ready || !settings.Enabled || resetRequested || IsMapView() || !globals::state->HasDirectionalShadows())
		return false;
	const auto* manager = LODShadowsRE::GetActiveTerrainManager();
	return manager && manager->worldSpace && manager->worldSpace->GetFormID() == worldSpaceID &&
	       !LODShadowsRE::IsLoadScreenUp() && std::ranges::all_of(maps, [](const Map& map) { return map.valid && map.terrainValid; });
}

void LODShadows::Publish()
{
	if (!mapBuffer)
		return;
	std::array<MapData, kMapCount> data{};
	uint32_t validMaps = 0;
	const bool active = ready && settings.Enabled && !IsMapView();
	for (uint32_t i = 0; i < kMapCount; ++i) {
		data[i] = maps[i].data;
		const bool valid = active && maps[i].valid;
		const float bias = settings.DepthBias / std::max(kMinimumDepthPadding, maps[i].depthRange);
		const float historyBias = settings.DepthBias / std::max(kMinimumDepthPadding, previousMaps[i].depthRange);
		data[i].parameters = float4{ float(maps[i].slice), valid ? 1.0f : 0.0f, bias, 0 };
		data[i].filter = float4{ 1.0f / kResolution, maps[i].halfExtent * 2 / kResolution, historyBias, 0 };
		data[i].historyWorldToClip = previousMaps[i].data.worldToClip;
		const bool boundsValid = maps[i].rayBoundsValid && (!previousMaps[i].valid || previousMaps[i].rayBoundsValid);
		const uint32_t flags = kHardwareComparison | (boundsValid ? kRayBounds : 0);
		data[i].history = float4{ float(previousMaps[i].slice), GetHistoryBlend(i), previousMaps[i].valid ? 1.0f : 0.0f, float(flags) };
		validMaps += valid;
	}
	data[0].parameters.w = float(validMaps);
	data[1].parameters.w = ReplacesTerrainShadows() ? 1.0f : 0.0f;
	data[2].parameters.w = GetNativeShadowDistance();
	data[0].filter.w = maskReady ? 1.0f : 0.0f;
	data[1].filter.w = float(maskDimensions[0]);
	data[2].filter.w = float(maskDimensions[1]);
	auto context = globals::d3d::context;
	context->UpdateSubresource(mapBuffer->resource.get(), 0, nullptr, data.data(), 0, 0);
	if (effectMapBuffer)
		effectMapBuffer->Update(data);
	ID3D11ShaderResourceView* views[]{ depthSRV.get(), mapBuffer->srv.get() };
	context->PSSetShaderResources(kDepthSlot, 2, views);
	context->CSSetShaderResources(kDepthSlot, 2, views);
	auto* bounds = rayBoundsSRV.get();
	context->PSSetShaderResources(kRayBoundsSlot, 1, &bounds);
}

void LODShadows::OnDraw()
{
	const auto* shader = globals::state->currentShader;
	if (!shader)
		return;
	const auto type = shader->shaderType.get();
	if (type == RE::BSShader::Type::Effect) {
		auto* buffer = effectMapBuffer ? effectMapBuffer->CB() : nullptr;
		globals::d3d::context->PSSetConstantBuffers(kEffectMapBufferSlot, 1, &buffer);
	}
	if (!ready || !settings.Enabled || (type != RE::BSShader::Type::Effect && type != RE::BSShader::Type::Particle))
		return;
	auto* sampler = comparisonSampler.get();
	globals::d3d::context->PSSetSamplers(kComparisonSamplerSlot, 1, &sampler);
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= 1u << kComparisonSamplerSlot;
}

void LODShadows::ReflectionsPrepass()
{
	maskReady = false;
	Publish();
}

void LODShadows::CollectScene(bool a_refreshLoaded)
{
	std::vector<RE::NiPointer<RE::BSGeometry>> loadedGeometry;
	if (!a_refreshLoaded && geometryCounts[3] <= visibleGeometry.size())
		loadedGeometry.assign(std::make_move_iterator(visibleGeometry.end() - geometryCounts[3]), std::make_move_iterator(visibleGeometry.end()));
	sceneHash = kHashSeed;
	visibleGeometry.clear();
	{
		RE::BSSpinLockGuard lock(LODShadowsRE::GetCameraTerrainLock());
		CollectTerrain();
		sourceHashes[0] = sceneHash;
		geometryCounts[0] = uint32_t(visibleGeometry.size());
		sceneHash = kHashSeed;
		Collect(LODShadowsRE::GetLODObjectRoot().get(), true);
		sourceHashes[1] = sceneHash;
		geometryCounts[1] = uint32_t(visibleGeometry.size()) - geometryCounts[0];
		sceneHash = kHashSeed;
		Collect(LODShadowsRE::GetTreeNode().get(), true);
		sourceHashes[2] = sceneHash;
		geometryCounts[2] = uint32_t(visibleGeometry.size()) - geometryCounts[0] - geometryCounts[1];
	}
	if (a_refreshLoaded) {
		sceneHash = kHashSeed;
		const auto lodGeometryCount = visibleGeometry.size();
		CollectLoadedObjects();
		sourceHashes[3] = sceneHash;
		geometryCounts[3] = uint32_t(visibleGeometry.size() - lodGeometryCount);
	} else {
		visibleGeometry.insert(visibleGeometry.end(), std::make_move_iterator(loadedGeometry.begin()), std::make_move_iterator(loadedGeometry.end()));
	}
	sceneHash = kHashSeed;
	HashBytes(sceneHash, sourceHashes.data(), sizeof(sourceHashes));
}

void LODShadows::EarlyPrepass()
{
	ZoneScoped;
	const double nowMs = Util::GetNowSecs() * 1000.0;
	frameDurationMs = frameTimeMs > 0 ? nowMs - frameTimeMs : 0;
	frameTimeMs = nowMs;
	maskReady = false;
	lastTriangles = 0;
	ResolveTimings();
	if (resetRequested.exchange(false)) {
		Invalidate();
		lastUpdateMs = 0;
	}
	if (IsMapView()) {
		Publish();
		return;
	}
	if (!ready || !settings.Enabled || !globals::state->HasDirectionalShadows()) {
		if (wasEnabled)
			Invalidate();
		wasEnabled = false;
		Publish();
		return;
	}
	wasEnabled = true;
	try {
		auto manager = LODShadowsRE::GetActiveTerrainManager();
		auto direction = GetSunDirection();
		const auto directionLength = direction.Length();
		if (!manager || !manager->worldSpace || LODShadowsRE::IsLoadScreenUp() || !std::isfinite(directionLength) || directionLength < kMinimumDirectionLength) {
			Invalidate();
			Publish();
			return;
		}
		direction /= directionLength;
		const auto generation = Util::GetCompletedCelestialTransitionGeneration();
		const float clockRate = ShadowClockRate();
		const bool subframeUpdates = clockRate > 1.0f && frameDurationMs * clockRate >= kHistoryIntervalsMs[0];
		const bool refreshImmediately = subframeUpdates || generation != transitionGeneration;
		const auto worldID = manager->worldSpace->GetFormID();
		if (worldID != worldSpaceID)
			Invalidate();
		worldSpaceID = worldID;
		transitionGeneration = generation;
		const auto* tes = globals::game::tes;
		const int32_t gridX = tes ? tes->currentGridX : 0;
		const int32_t gridY = tes ? tes->currentGridY : 0;
		const bool refreshLoaded = refreshImmediately || visibleGeometry.empty() || gridX != collectedGridX ||
		                           gridY != collectedGridY || ++framesSinceCollect >= kCollectIntervalFrames;
		CollectScene(refreshLoaded);
		if (refreshLoaded) {
			framesSinceCollect = 0;
			collectedGridX = gridX;
			collectedGridY = gridY;
		}
		UpdateMaps(Util::GetEyePosition(), direction, refreshImmediately);
	} catch (const std::exception& error) {
		logger::error("LOD Shadows disabled after rendering failure: {}", error.what());
		ready = false;
		Invalidate();
	}
	Publish();
}
