#pragma once

#include "EngineFix.h"

/**
 * @brief Skips RaceMenu overlay slots that still show the transparent default texture.
 *
 * RaceMenu attaches every configured overlay slot up front as a full copy of the
 * skinned body, hands, feet or face shape. An unused slot keeps a fully transparent
 * diffuse, yet it alpha-tests at threshold 0, which passes alpha 0, so every pixel
 * runs the whole lighting shader before blending to nothing.
 *
 * Hooks BSLightingShaderProperty::GetRenderPasses (vtable 0x2A) and returns an empty
 * pass list for such shapes. The check runs on every build, so a slot is drawn again
 * as soon as RaceMenu assigns it a real texture.
 */
struct EmptyOverlayCullFix : EngineFix
{
	std::string GetName() override { return "Empty Overlay Cull Fix"; }

	void Install() override;

private:
	struct BSLightingShaderProperty_GetRenderPasses
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t renderMode, RE::BSShaderAccumulator* accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
