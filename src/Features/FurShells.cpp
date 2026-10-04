#include "FurShells.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.fur_shells."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FurShells::Settings,
	Enabled,
	ShellCount,
	Length,
	Droop,
	RootThreshold,
	TipThreshold,
	RootDarkening,
	ShellColor,
	FadeStart,
	FadeEnd)

namespace
{
	constexpr uint32_t FurFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::FurShells);
	constexpr uint32_t FurDepthFlag = FurFlag | static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::FurShellsDepth);
	constexpr uint32_t ModelSpaceNormalsFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals);
	constexpr uint32_t ReflectionsFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);

	constexpr uint32_t MaxShells = 32;
	constexpr uint32_t ShellTextureSlot = 122;
	constexpr uint32_t PerPassSlot = 13;
	constexpr uint32_t ResolvesPerFrame = 16;
	constexpr size_t MaxEntries = 8192;
	constexpr size_t MaxMissingShells = 65536;
	constexpr float PixelsPerShell = 1.0f;

	bool CanDrawShells(RE::BSGeometry* a_geometry)
	{
		if (a_geometry->GetGeometryRuntimeData().skinInstance)
			return true;

		const auto type = a_geometry->GetType().get();
		return type == RE::BSGeometry::Type::kTriShape || type == RE::BSGeometry::Type::kMeshLODTriShape;
	}

	bool IsFurTechnique(uint32_t a_descriptor)
	{
		using enum SIE::ShaderCache::LightingShaderTechniques;

		switch (static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_descriptor >> 24) & 0x3F)) {
		case None:
		case Envmap:
		case Glowmap:
		case Parallax:
		case ParallaxOcc:
		case MultilayerParallax:
			return true;
		default:
			return false;
		}
	}

	uint32_t GetResolvableShells(float a_length, float a_distance)
	{
		const float projection = std::abs(globals::game::shadowState->GetRuntimeData().cameraData.getEye().projMat.m[1][1]);
		const float height = Util::ConvertToDynamic({ 0.0f, static_cast<float>(globals::game::graphicsState->screenHeight) }).y;
		const float pixels = a_length * 0.5f * height * projection / std::max(a_distance, 1.0f);
		if (!std::isfinite(pixels))
			return MaxShells;
		return static_cast<uint32_t>(std::clamp(std::ceil(pixels / PixelsPerShell), 1.0f, static_cast<float>(MaxShells)));
	}

	std::string NormalizeTexturePath(const char* a_path)
	{
		std::string path = a_path;
		std::ranges::transform(path, path.begin(), [](char a_char) {
			return a_char == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(a_char)));
		});
		if (path.starts_with("data\\"))
			path.erase(0, 5);
		if (!path.starts_with("textures\\"))
			path.insert(0, "textures\\");
		return path;
	}

	RE::NiSourceTexturePtr LoadShell(const std::string& a_path, bool& a_exists)
	{
		constexpr std::string_view pbrPrefix = "textures\\pbr\\";

		a_exists = false;
		if (!a_path.ends_with(".dds"))
			return nullptr;

		const std::string stem = a_path.substr(0, a_path.size() - 4);
		std::array<std::string, 2> candidates{ stem + "_shell.dds", std::string() };
		if (stem.starts_with(pbrPrefix))
			candidates[1] = "textures\\" + stem.substr(pbrPrefix.size()) + "_shell.dds";

		for (const auto& candidate : candidates) {
			if (candidate.empty() || !RE::BSResourceNiBinaryStream(candidate).good())
				continue;
			a_exists = true;

			RE::NiPointer<RE::NiTexture> texture;
			RE::BSShaderManager::GetTexture(candidate.c_str(), true, texture, false);
			if (texture && texture->GetRTTI() == globals::rtti::NiSourceTextureRTTI.get()) {
				logger::info("[Fur Shells] {} uses {}", a_path, candidate);
				return RE::NiSourceTexturePtr(static_cast<RE::NiSourceTexture*>(texture.get()));
			}
		}

		return nullptr;
	}
}

void FurShells::RestoreDefaultSettings()
{
	settings = {};
}

void FurShells::LoadSettings(json& o_json)
{
	settings = o_json;
}

void FurShells::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FurShells::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Draws fur shells on meshes whose texture has a _shell.dds beside it."));

	int shellCount = static_cast<int>(settings.ShellCount);
	if (ImGui::SliderInt(T(TKEY("shell_count"), "Shells"), &shellCount, 1, static_cast<int>(MaxShells), "%d", ImGuiSliderFlags_AlwaysClamp))
		settings.ShellCount = static_cast<uint32_t>(shellCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shell_count_tooltip"), "Layers drawn up close. Each one is another lighting pass over the fur."));

	ImGui::SliderFloat(T(TKEY("length"), "Length"), &settings.Length, 0.1f, 6.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("length_tooltip"), "Fur length in game units."));

	ImGui::SliderFloat(T(TKEY("droop"), "Droop"), &settings.Droop, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("droop_tooltip"), "How far the tips sag, in game units."));

	ImGui::SliderFloat(T(TKEY("root_threshold"), "Root Cutoff"), &settings.RootThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("root_threshold_tooltip"), "Shell texture alpha needed at the root. Lower is denser."));

	ImGui::SliderFloat(T(TKEY("tip_threshold"), "Tip Cutoff"), &settings.TipThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("tip_threshold_tooltip"), "Shell texture alpha needed at the tip. Higher thins the strands toward the end."));

	ImGui::SliderFloat(T(TKEY("root_darkening"), "Root Brightness"), &settings.RootDarkening, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("root_darkening_tooltip"), "Brightness at the root. Tips stay at full brightness."));

	ImGui::SliderFloat(T(TKEY("shell_color"), "Shell Texture Color"), &settings.ShellColor, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shell_color_tooltip"), "0 colors the fur from the armor's own texture, 1 from the shell texture."));

	ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("fade_tooltip"), "Shells thin out between these distances and stop past the end."));

	ImGui::Text("%s: %u", T(TKEY("fur_passes"), "Fur passes last frame"), lastPassCount);
}

void FurShells::Reset()
{
	resolveBudget = ResolvesPerFrame;
	lastPassCount = passCount;
	passCount = 0;
	lastDiffuse = nullptr;
	lastShell = nullptr;
}

void FurShells::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	using Flags = SIE::ShaderCache::LightingShaderFlags;
	using Techniques = SIE::ShaderCache::LightingShaderTechniques;

	if (a_shader->shaderType != RE::BSShader::Type::Lighting)
		return;

	constexpr uint32_t vertexColor = static_cast<uint32_t>(Flags::VC);
	constexpr std::array techniques{ Techniques::None, Techniques::Envmap };
	constexpr std::array alphaTestFlags{ 0u, static_cast<uint32_t>(Flags::DoAlphaTest) };
	constexpr std::array deferredFlags{ 0u, static_cast<uint32_t>(Flags::Deferred) };
	constexpr std::array vertexColorFlags{ 0u, vertexColor };
	constexpr std::array skinnedFlags{ 0u, static_cast<uint32_t>(Flags::Skinned) };

	auto* shaderCache = globals::shaderCache;
	for (const auto technique : techniques) {
		const uint32_t type = static_cast<uint32_t>(technique) << 24;
		for (const uint32_t alphaTest : alphaTestFlags)
			for (const uint32_t deferred : deferredFlags) {
				std::ignore = shaderCache->GetPixelShader(*a_shader, type | vertexColor | alphaTest | deferred | FurFlag);
				std::ignore = shaderCache->GetPixelShader(*a_shader, type | vertexColor | alphaTest | deferred | FurDepthFlag);
			}
		for (const uint32_t color : vertexColorFlags)
			for (const uint32_t skinned : skinnedFlags)
				std::ignore = shaderCache->GetVertexShader(*a_shader, type | color | skinned | FurFlag);
	}

	if (globals::features::truePBR.loaded) {
		for (const uint32_t alphaTest : alphaTestFlags)
			for (const uint32_t deferred : deferredFlags) {
				std::ignore = shaderCache->GetPixelShader(*a_shader, static_cast<uint32_t>(Flags::TruePbr) | vertexColor | alphaTest | deferred | FurFlag);
				std::ignore = shaderCache->GetPixelShader(*a_shader, static_cast<uint32_t>(Flags::TruePbr) | vertexColor | alphaTest | deferred | FurDepthFlag);
			}
	}
}

RE::NiSourceTexture* FurShells::FindShell(RE::BSRenderPass* a_pass)
{
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
	if (!diffuse)
		return nullptr;
	if (diffuse == lastDiffuse)
		return lastShell;

	auto it = entries.find(diffuse);
	if (it == entries.end() || it->second.name != diffuse->name) {
		RE::NiSourceTexturePtr shell;
		if (const char* diffusePath = material->textureSet ? material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr; diffusePath && *diffusePath) {
			std::string path = NormalizeTexturePath(diffusePath);
			if (!missingShells.contains(path)) {
				if (resolveBudget == 0)
					return nullptr;
				--resolveBudget;

				bool exists = false;
				shell = LoadShell(path, exists);
				if (!exists) {
					if (missingShells.size() >= MaxMissingShells)
						missingShells.clear();
					missingShells.emplace(std::move(path));
				}
			}
		}

		if (entries.size() >= MaxEntries)
			entries.clear();
		it = entries.insert_or_assign(diffuse, Entry{ diffuse->name, std::move(shell) }).first;
	}

	lastDiffuse = diffuse;
	lastShell = it->second.shell.get();
	return lastShell;
}

void FurShells::BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	using enum RE::BSGraphics::DepthStencilDepthMode;

	if (instanceCount != 0)
		EndPass();

	if (!settings.Enabled || !perPassCB || !noColorWrite || !a_pass || !globals::shaderCache->IsEnabled())
		return;

	auto* state = globals::state;
	if ((state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0 || (state->modifiedVertexDescriptor & ModelSpaceNormalsFlag) != 0 || !IsFurTechnique(state->currentPixelDescriptor))
		return;

	const auto depthMode = globals::game::shadowState->GetRuntimeData().depthStencilDepthMode;
	if (depthMode != kTestEqual && depthMode != kTestWrite)
		return;

	auto* geometry = a_pass->geometry;
	if (!geometry)
		return;

	const float distance = std::max(0.0f, geometry->worldBound.center.GetDistance(Util::GetEyePosition()) - geometry->worldBound.radius);
	const float fade = std::clamp((settings.FadeEnd - distance) / std::max(settings.FadeEnd - settings.FadeStart, 1.0f), 0.0f, 1.0f);
	auto shells = static_cast<uint32_t>(std::lround(static_cast<float>(std::min(settings.ShellCount, MaxShells)) * fade));
	if (shells == 0 || !CanDrawShells(geometry))
		return;

	auto* shell = FindShell(a_pass);
	auto* shellView = shell && shell->rendererTexture ? shell->rendererTexture->resourceView : nullptr;
	if (!shellView)
		return;

	shells = std::min(shells, GetResolvableShells(settings.Length, distance));

	auto* shaderCache = globals::shaderCache;
	auto* vertexShader = shaderCache->GetVertexShader(*a_shader, state->modifiedVertexDescriptor | FurFlag);
	auto* pixelShader = shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | FurFlag);
	auto* depthShader = shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | FurDepthFlag);
	if (!vertexShader || !pixelShader || !depthShader)
		return;

	const PerPass perPass{ settings.Length, static_cast<float>(shells), settings.Droop, settings.RootThreshold, settings.TipThreshold, settings.RootDarkening, settings.ShellColor, 0.0f };
	if (std::memcmp(&perPass, &lastPerPass, sizeof(PerPass)) != 0) {
		perPassCB->Update(perPass);
		lastPerPass = perPass;
	}

	furPixelShader = reinterpret_cast<ID3D11PixelShader*>(pixelShader->shader);
	depthPixelShader = reinterpret_cast<ID3D11PixelShader*>(depthShader->shader);

	auto* context = globals::d3d::context;
	context->VSGetShader(savedVertexShader.put(), nullptr, nullptr);
	context->PSGetShader(savedPixelShader.put(), nullptr, nullptr);
	context->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader), nullptr, 0);
	context->PSSetShader(furPixelShader, nullptr, 0);

	ID3D11Buffer* buffer = perPassCB->CB();
	context->VSSetConstantBuffers(PerPassSlot, 1, &buffer);
	context->PSSetConstantBuffers(PerPassSlot, 1, &buffer);
	context->PSSetShaderResources(ShellTextureSlot, 1, &shellView);

	instanceCount = shells + 1;
	++passCount;
}

void FurShells::EndPass()
{
	if (instanceCount == 0)
		return;
	instanceCount = 0;

	auto* context = globals::d3d::context;
	context->VSSetShader(savedVertexShader.get(), nullptr, 0);
	context->PSSetShader(savedPixelShader.get(), nullptr, 0);
	savedVertexShader = nullptr;
	savedPixelShader = nullptr;
}

const FurShells::DepthStates* FurShells::GetDepthStates(ID3D11DepthStencilState* a_source)
{
	if (!a_source)
		return nullptr;

	auto it = depthStates.find(a_source);
	if (it == depthStates.end()) {
		DepthStates states;
		states.source.copy_from(a_source);

		D3D11_DEPTH_STENCIL_DESC desc;
		a_source->GetDesc(&desc);
		desc.DepthEnable = TRUE;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;

		auto* device = globals::d3d::device;
		if (SUCCEEDED(device->CreateDepthStencilState(&desc, states.prepass.put()))) {
			desc.DepthFunc = D3D11_COMPARISON_EQUAL;
			desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (SUCCEEDED(device->CreateDepthStencilState(&desc, states.shade.put()))) {
				Util::SetResourceName(states.prepass.get(), "FurShells::PrepassDepthState");
				Util::SetResourceName(states.shade.get(), "FurShells::ShadeDepthState");
			} else {
				states.prepass = nullptr;
			}
		}
		if (!states.prepass)
			logger::warn("[Fur Shells] Failed to create depth states");

		it = depthStates.emplace(a_source, std::move(states)).first;
	}

	return it->second.prepass ? &it->second : nullptr;
}

bool FurShells::DrawShells(UINT a_indexCount, UINT a_startIndexLocation, INT a_baseVertexLocation)
{
	auto* context = globals::d3d::context;

	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	UINT stencilRef = 0;
	context->OMGetDepthStencilState(depthState.put(), &stencilRef);

	const auto* states = GetDepthStates(depthState.get());
	if (!states) {
		EndPass();
		return false;
	}

	winrt::com_ptr<ID3D11BlendState> blendState;
	FLOAT blendFactor[4]{};
	UINT sampleMask = 0;
	context->OMGetBlendState(blendState.put(), blendFactor, &sampleMask);

	context->OMSetBlendState(noColorWrite.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(states->prepass.get(), stencilRef);
	context->PSSetShader(depthPixelShader, nullptr, 0);
	context->DrawIndexedInstanced(a_indexCount, instanceCount, a_startIndexLocation, a_baseVertexLocation, 0);

	context->OMSetBlendState(blendState.get(), blendFactor, sampleMask);
	context->OMSetDepthStencilState(states->shade.get(), stencilRef);
	context->PSSetShader(furPixelShader, nullptr, 0);
	context->DrawIndexedInstanced(a_indexCount, instanceCount, a_startIndexLocation, a_baseVertexLocation, 0);

	context->OMSetDepthStencilState(depthState.get(), stencilRef);
	return true;
}

struct FurShells::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::furShells.BeginPass(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::furShells.EndPass();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawIndexed
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndexLocation, INT a_baseVertexLocation)
		{
			auto& furShells = globals::features::furShells;
			if (furShells.GetInstanceCount() <= 1 || a_context != globals::d3d::context || !furShells.DrawShells(a_indexCount, a_startIndexLocation, a_baseVertexLocation))
				func(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

void FurShells::SetupResources()
{
	static bool drawHookInstalled = false;

	perPassCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerPass>(), "FurShells::PerPass");

	noColorWrite = nullptr;
	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
	if (SUCCEEDED(globals::d3d::device->CreateBlendState(&blendDesc, noColorWrite.put())))
		Util::SetResourceName(noColorWrite.get(), "FurShells::NoColorWrite");
	else
		logger::warn("[Fur Shells] Failed to create blend state");

	if (!drawHookInstalled && globals::d3d::context) {
		drawHookInstalled = true;
		stl::detour_vfunc<12, Hooks::ID3D11DeviceContext_DrawIndexed>(globals::d3d::context);
		logger::info("[Fur Shells] Installed draw hook");
	}
}

void FurShells::PostPostLoad()
{
	stl::write_vfunc<0x6, Hooks::BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, Hooks::BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Fur Shells] Installed hooks");
}

#undef I18N_KEY_PREFIX
