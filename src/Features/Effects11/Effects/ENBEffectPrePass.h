#pragma once

#include "Buffer.h"
#include "ExtendedEffect.h"

class ENBEffectPrePass : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbeffectprepass.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	void ClearShaderCache();

protected:
	void CreateEffectTextures() override;

private:
	struct alignas(16) GBufferCB
	{
		float2 ResolutionScale;
		float2 pad0;
	};

	bool RenderGBufferTexture();

	TextureManager::Texture textureGBuffer;
	ID3D11PixelShader* gbufferPS = nullptr;
	bool gbufferPSFailed = false;
	std::unique_ptr<ConstantBuffer> gbufferCB;
};
