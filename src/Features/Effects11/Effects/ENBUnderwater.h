#pragma once

#include "ENBEffectPostPass.h"

class ENBUnderwater : public ENBEffectPostPass
{
public:
	virtual std::string GetName() const override { return "enbunderwater.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	/** @brief True while the camera is at least partly below the water surface. */
	static bool IsCameraUnderwater();

protected:
	void CreateEffectTextures() override;

private:
	TextureManager::Texture textureFullMask;
};
