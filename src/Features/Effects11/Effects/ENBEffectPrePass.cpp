#include "ENBEffectPrePass.h"

#include "../EffectManager.h"
#include "../TextureManager.h"
#include "Deferred.h"
#include "Features/ProceduralSun.h"
#include "Features/SkySync.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void ENBEffectPrePass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto& textureMain = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!textureMain.texture || !textureMain.SRV)
		return;

	auto* textureHDRTemp = textureManager.GetCommonTexture("TextureHDRTemp");
	auto* textureHDRTemp2 = textureManager.GetCommonTexture("TextureHDRTemp2");
	if (!textureHDRTemp || !textureHDRTemp2)
		return;

	if (!RenderGBufferTexture())
		return;

	auto [executed, inOutput, inTemp] = ExecuteTechniqueSequence(GetSelectedTechnique(), textureMain.SRV, *textureHDRTemp, *textureHDRTemp2);
	if (executed && (inOutput || inTemp))
		EffectManager::GetSingleton().CopyToMain(inOutput ? *textureHDRTemp : *textureHDRTemp2);
}

void ENBEffectPrePass::UpdateEffectVariables()
{
	SetShaderResourceVariable("TextureOriginal", globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV);
	SetShaderResourceVariable("TextureNormal", textureGBuffer.srv.get());
	SetShaderResourceVariable("TextureMask", textureGBuffer.srv.get());

	float4 sunDirection{ 0.0f, 0.0f, 1.0f, 0.0f };
	float4 sunColor{};
	auto sky = globals::game::sky;
	if (sky && sky->sun && sky->sun->light) {
		const float visibility = ProceduralSun::GetSunVisibility();
		const auto direction = globals::features::skySync.GetCelestialDirection(sky, SkySync::Caster::Sun);
		const auto view = globals::game::frameBufferCached.GetCameraView().Transpose();
		const auto viewDirection = DirectX::SimpleMath::Vector4::Transform(DirectX::SimpleMath::Vector4(direction.x, direction.y, direction.z, 0.0f), view);
		sunDirection = { viewDirection.x, viewDirection.y, viewDirection.z, visibility };
		const auto& diffuse = sky->sun->light->GetLightRuntimeData().diffuse;
		sunColor = { diffuse.red, diffuse.green, diffuse.blue, visibility };
	}
	SetVectorVariable("SunDirection", &sunDirection, sizeof(sunDirection));
	SetVectorVariable("SunColor", &sunColor, sizeof(sunColor));
}

void ENBEffectPrePass::CreateEffectTextures()
{
	auto wantsVariable = [&](const char* a_name) {
		auto* variable = GetCachedVariable(a_name);
		return variable && variable->IsValid();
	};
	textureGBuffer = {};
	if (wantsVariable("TextureNormal") || wantsVariable("TextureMask"))
		textureGBuffer = CreateTexture(globals::game::graphicsState->screenWidth, globals::game::graphicsState->screenHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "ENBEffectPrePass::TextureGBuffer");
}

bool ENBEffectPrePass::RenderGBufferTexture()
{
	if (!textureGBuffer.rtv)
		return true;

	if (!gbufferPS && !gbufferPSFailed) {
		gbufferPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Effects11\\PrepassGBufferPS.hlsl", {}, "ps_5_0"));
		if (!gbufferPS) {
			logger::error("[EFFECTS11] Failed to compile PrepassGBufferPS.hlsl; enbeffectprepass.fx is skipped");
			gbufferPSFailed = true;
		}
	}
	if (!gbufferPS)
		return false;

	auto& renderTargets = globals::game::renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* srvs[] = { renderTargets[NORMALROUGHNESS].SRV, renderTargets[MASKS].SRV };
	if (!srvs[0] || !srvs[1])
		return false;

	if (!gbufferCB)
		gbufferCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GBufferCB>(), "ENBEffectPrePass::GBufferCB");

	gbufferCB->Update(GBufferCB{ .ResolutionScale = globals::features::upscaling.resolutionScale });
	ID3D11Buffer* constantBuffer = gbufferCB->CB();

	auto context = globals::d3d::context;
	auto& effectManager = EffectManager::GetSingleton();

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(globals::game::graphicsState->screenWidth);
	viewport.Height = static_cast<float>(globals::game::graphicsState->screenHeight);
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* rtv = textureGBuffer.rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(effectManager.rasterizerState.get());
	context->OMSetBlendState(effectManager.blendState.get(), nullptr, 0xFFFFFFFF);
	context->VSSetShader(effectManager.copyVertexShader.get(), nullptr, 0);
	context->PSSetShader(gbufferPS, nullptr, 0);
	context->PSSetShaderResources(0, 2, srvs);
	context->PSSetConstantBuffers(0, 1, &constantBuffer);
	context->Draw(4, 0);

	ID3D11ShaderResourceView* nullSRVs[2]{};
	context->PSSetShaderResources(0, 2, nullSRVs);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	return true;
}

void ENBEffectPrePass::ClearShaderCache()
{
	if (gbufferPS) {
		gbufferPS->Release();
		gbufferPS = nullptr;
	}
	gbufferPSFailed = false;
}
