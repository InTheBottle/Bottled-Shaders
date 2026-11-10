#include "ENBSunSprite.h"

#include "../EffectManager.h"
#include "../TextureManager.h"
#include "Features/CloudShadows.h"
#include "Features/SkySync.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

void ENBSunSprite::Execute()
{
	auto& effectManager = EffectManager::GetSingleton();
	if (effectManager.GetCommonData().lightParameters[3] <= 0.0f || Util::IsInterior())
		return;

	auto sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || sky->flags.any(RE::Sky::Flags::kHideSky))
		return;

	if (globals::state->IsFullScreenMenuOpen() || globals::state->isMapMenuOpen)
		return;

	auto& textureMain = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto* textureHDRTemp2 = TextureManager::GetSingleton().GetCommonTexture("TextureHDRTemp2");
	if (!textureMain.SRV || !textureMain.RTV || !textureHDRTemp2 || !EnsureMaskResources() || !RenderMask())
		return;

	effectManager.CopyTexture(textureMain.SRV, textureHDRTemp2->rtv.get(), false);
	SetShaderResourceVariable("TextureMask", textureMask.srv.get());
	SetShaderResourceVariable("TextureOriginal", textureHDRTemp2->srv.get());

	ExecuteTechniquesOnto(GetSelectedTechnique(), textureHDRTemp2->srv.get(), textureMain.RTV, additiveBlendState.get());

	auto context = globals::d3d::context;
	context->OMSetBlendState(effectManager.blendState.get(), nullptr, 0xFFFFFFFF);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
}

void ENBSunSprite::CreateEffectTextures()
{
	textureMask = CreateTexture(1, 1, DXGI_FORMAT_R16_FLOAT, "ENBSunSprite::TextureMask");
}

bool ENBSunSprite::EnsureMaskResources()
{
	if (!textureMask.rtv)
		return false;

	if (!maskPS && !maskPSFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::cloudShadows.loaded)
			defines.push_back({ "CLOUD_SHADOWS", nullptr });
		maskPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Effects11\\SunSpriteMaskPS.hlsl", defines, "ps_5_0"));
		if (!maskPS) {
			logger::error("[EFFECTS11] Failed to compile SunSpriteMaskPS.hlsl; enbsunsprite.fx is skipped");
			maskPSFailed = true;
		}
	}
	if (!maskPS)
		return false;

	if (!maskCB)
		maskCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SunMaskCB>(), "ENBSunSprite::MaskCB");

	if (!additiveBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(globals::d3d::device->CreateBlendState(&blendDesc, additiveBlendState.put())))
			return false;
		Util::SetResourceName(additiveBlendState.get(), "ENBSunSprite::AdditiveBlendState");
	}

	return true;
}

bool ENBSunSprite::RenderMask()
{
	auto context = globals::d3d::context;
	auto& effectManager = EffectManager::GetSingleton();
	auto sky = globals::game::sky;

	const auto sunDirection = globals::features::skySync.GetCelestialDirection(sky, SkySync::Caster::Sun);
	const auto* lightParameters = effectManager.GetCommonData().lightParameters;

	maskCB->Update(SunMaskCB{
		.SunDirection = { sunDirection.x, sunDirection.y, sunDirection.z, 0.0f },
		.SunUV = { lightParameters[0] * 0.5f + 0.5f, 0.5f - lightParameters[1] * 0.5f } });

	D3D11_VIEWPORT viewport{};
	viewport.Width = 1.0f;
	viewport.Height = 1.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* rtv = textureMask.rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(effectManager.rasterizerState.get());
	context->OMSetBlendState(effectManager.blendState.get(), nullptr, 0xFFFFFFFF);
	context->VSSetShader(effectManager.copyVertexShader.get(), nullptr, 0);
	context->PSSetShader(maskPS, nullptr, 0);

	ID3D11ShaderResourceView* depthSRV = effectManager.GetEffectDepthSRV();
	context->PSSetShaderResources(0, 1, &depthSRV);
	ID3D11Buffer* constantBuffers[] = { maskCB->CB() };
	context->PSSetConstantBuffers(0, 1, constantBuffers);
	ID3D11SamplerState* samplers[] = { TextureManager::GetSingleton().GetLinearSampler() };
	context->PSSetSamplers(0, 1, samplers);

	auto& cloudShadows = globals::features::cloudShadows;
	const bool bindCloudShadows = cloudShadows.loaded && cloudShadows.texCubemapCloudOccCopy;
	if (bindCloudShadows) {
		ID3D11ShaderResourceView* cloudSRV = cloudShadows.texCubemapCloudOccCopy->srv.get();
		context->PSSetShaderResources(25, 1, &cloudSRV);
	}

	globals::profiler->BeginPass("Effects11::SunSpriteMask");
	context->Draw(4, 0);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
	if (bindCloudShadows)
		context->PSSetShaderResources(25, 1, &nullSRV);
	return true;
}

void ENBSunSprite::ClearShaderCache()
{
	if (maskPS) {
		maskPS->Release();
		maskPS = nullptr;
	}
	maskPSFailed = false;
}
