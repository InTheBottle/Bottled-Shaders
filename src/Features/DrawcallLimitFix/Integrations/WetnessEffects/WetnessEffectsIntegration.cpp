#include "Features/DrawcallLimitFix/Integrations/WetnessEffects/WetnessEffects.h"

#include "Globals.h"

#include "Features/WetnessEffects.h"

namespace DCLF::Integrations::WetnessEffects
{
	namespace
	{
		void* PrecipitationOcclusionView()
		{
			auto* renderer = globals::game::renderer;
			if (!globals::features::wetnessEffects.loaded || !renderer)
				return nullptr;
			return renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP].depthSRV;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "wetness effects precipitation occlusion", .textureRegister = kPrecipitationOcclusionRegister,
				.view = &PrecipitationOcclusionView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "wetness effects", &Install };
	}
}
