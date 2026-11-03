#include "Features/DrawcallLimitFix/Integrations/HairSpecular/HairSpecular.h"

#include "Globals.h"

#include "Features/HairSpecular.h"

namespace DCLF::Integrations::HairSpecular
{
	namespace
	{
		void* TangentShiftView()
		{
			const auto& hair = globals::features::hairSpecular;
			return hair.loaded && hair.texTangentShift ? hair.texTangentShift->srv.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "hair specular tangent shift", .textureRegister = kTangentShiftRegister, .view = &TangentShiftView,
				.capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "hair specular", &Install };
	}
}
