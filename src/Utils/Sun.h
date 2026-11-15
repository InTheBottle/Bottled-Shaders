#pragma once

namespace Util::Sun
{
	inline float GetVisibility()
	{
		const auto sky = globals::game::sky;
		const auto sun = sky ? sky->sun : nullptr;
		if (!sun || !sun->root || !sun->sunBaseNode || !sun->sunBase)
			return 0.0f;
		if (sun->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) || sun->sunBaseNode->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return 0.0f;

		const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get());
		if (!prop)
			return 0.0f;

		const float alpha = prop->kBlendColor.alpha;
		return alpha > 0.0f ? std::min(alpha, 1.0f) : 0.0f;
	}

	inline float GetMainTargetRadianceLimit()
	{
		const auto renderer = globals::game::renderer;
		const auto rtv = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV : nullptr;
		if (!rtv)
			return 1.0f;

		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		rtv->GetDesc(&desc);
		switch (desc.Format) {
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R11G11B10_FLOAT:
			return 4096.0f;
		default:
			return 1.0f;
		}
	}
}
