#include "EmptyOverlayCullFix.h"

namespace
{
	// RaceMenu's default sDefaultTexture, matched as a suffix so the "textures\" prefix is optional.
	constexpr std::string_view DefaultOverlayTexture = "actors\\character\\overlays\\default.dds";

	bool HasDefaultOverlayDiffuse(RE::BSLightingShaderProperty* property)
	{
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
		if (!material || !material->textureSet)
			return false;

		const char* path = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
		if (!path)
			return false;

		const std::string_view diffuse(path);
		return diffuse.size() >= DefaultOverlayTexture.size() &&
		       _strnicmp(diffuse.data() + diffuse.size() - DefaultOverlayTexture.size(), DefaultOverlayTexture.data(), DefaultOverlayTexture.size()) == 0;
	}

	bool IsEmptyOverlay(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry)
	{
		if (property->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned))
			return false;

		// Drawn without blending or testing, the transparent texture would still be visible.
		const auto* alpha = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get());
		if (!alpha || !(alpha->GetAlphaBlending() || alpha->GetAlphaTesting()))
			return false;

		return HasDefaultOverlayDiffuse(property);
	}
}

void EmptyOverlayCullFix::Install()
{
	stl::write_vfunc<0x2A, BSLightingShaderProperty_GetRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
}

RE::BSShaderProperty::RenderPassArray* EmptyOverlayCullFix::BSLightingShaderProperty_GetRenderPasses::thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t renderMode, RE::BSShaderAccumulator* accumulator)
{
	if (geometry && IsEmptyOverlay(property, geometry)) {
		property->renderPassList.Clear();
		return &property->renderPassList;
	}
	return func(property, geometry, renderMode, accumulator);
}
