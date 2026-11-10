#include "ENBUnderwater.h"

#include "Globals.h"

bool ENBUnderwater::IsCameraUnderwater()
{
	const auto waterSystem = RE::TESWaterSystem::GetSingleton();
	return waterSystem && (waterSystem->playerUnderwater || waterSystem->partiallyUnderwater);
}

void ENBUnderwater::Execute()
{
	if (!effectTextureCache.contains("TextureScratch"))
		ENBEffectPostPass::CreateEffectTextures();
	ENBEffectPostPass::Execute();
}

void ENBUnderwater::UpdateEffectVariables()
{
	ENBEffectPostPass::UpdateEffectVariables();

	const auto waterSystem = RE::TESWaterSystem::GetSingleton();
	const bool partial = waterSystem && waterSystem->partiallyUnderwater;
	auto& underwaterMask = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kUNDERWATER_MASK];
	SetShaderResourceVariable("TextureMask", partial && underwaterMask.SRV ? underwaterMask.SRV : textureFullMask.srv.get());

	float4 tintColor{};
	if (auto imageSpaceManager = globals::game::imageSpaceManager) {
		const auto& tint = imageSpaceManager->GetRuntimeData().data.baseData.tint;
		tintColor = { tint.color.red, tint.color.green, tint.color.blue, tint.amount };
	}
	SetVectorVariable("TintColor", &tintColor, sizeof(tintColor));
}

void ENBUnderwater::CreateEffectTextures()
{
	textureFullMask = CreateTexture(1, 1, DXGI_FORMAT_R8_UNORM, "ENBUnderwater::TextureFullMask");
	static constexpr float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	globals::d3d::context->ClearRenderTargetView(textureFullMask.rtv.get(), white);
}
