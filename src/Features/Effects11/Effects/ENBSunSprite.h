#pragma once

#include "Buffer.h"
#include "ExtendedEffect.h"

class ENBSunSprite : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbsunsprite.fx"; }

	virtual void Execute() override;

	void ClearShaderCache();

protected:
	void CreateEffectTextures() override;

private:
	struct alignas(16) SunMaskCB
	{
		float4 SunDirection;
		float2 SunUV;
		float2 pad0;
	};

	bool EnsureMaskResources();
	bool RenderMask();

	TextureManager::Texture textureMask;
	ID3D11PixelShader* maskPS = nullptr;
	bool maskPSFailed = false;
	std::unique_ptr<ConstantBuffer> maskCB;
	winrt::com_ptr<ID3D11BlendState> additiveBlendState;
};
