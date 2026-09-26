#include "ENBEffectPostPass.h"

#include "../TextureManager.h"

void ENBEffectPostPass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	auto textureSDRTemp2 = textureManager.GetCommonTexture("TextureSDRTemp2");
	auto textureSDRTemp3 = textureManager.GetCommonTexture("TextureSDRTemp3");

	if (!textureSDRTemp || !textureSDRTemp2 || !textureSDRTemp3) {
		return;
	}

	// TextureSDRTemp holds the enbeffect result and is bound as TextureOriginal for every technique,
	// so the ping-pong must never render into it (D3D11 would null the SRV while it is an RTV)
	auto [executed, inOutput] = ExecuteTechniqueSequence(GetSelectedTechnique(), textureSDRTemp->srv.get(), *textureSDRTemp2, *textureSDRTemp3);

	if (executed) {
		textureManager.SwapTextures("TextureSDRTemp", inOutput ? "TextureSDRTemp2" : "TextureSDRTemp3");
	}
}

void ENBEffectPostPass::UpdateEffectVariables()
{
	auto* textureSDRTemp = GetCachedCommonTexture("TextureSDRTemp");
	SetShaderResourceVariable("TextureOriginal", textureSDRTemp ? textureSDRTemp->srv.get() : nullptr);
}
